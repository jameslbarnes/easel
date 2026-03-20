#ifdef HAS_WHEP

#include "sources/WHEPSource.h"

#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <iostream>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstring>

static void whepLog(const std::string& msg) {
    std::ofstream f("whep_debug.log", std::ios::app);
    f << msg << std::endl;
    f.close();
    std::cerr << msg << std::endl;
}

// WinHTTP for HTTPS support (WHEP endpoints are often HTTPS)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#endif

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    size_t schemeEnd = url.find("://");
    size_t hostStart = (schemeEnd != std::string::npos) ? schemeEnd + 3 : 0;
    size_t pathStart = url.find('/', hostStart);
    std::string authority = (pathStart != std::string::npos)
        ? url.substr(hostStart, pathStart - hostStart)
        : url.substr(hostStart);
    path = (pathStart != std::string::npos) ? url.substr(pathStart) : "/";

    size_t colonPos = authority.find(':');
    if (colonPos != std::string::npos) {
        host = authority.substr(0, colonPos);
        port = std::stoi(authority.substr(colonPos + 1));
    } else {
        host = authority;
        port = (url.rfind("https", 0) == 0) ? 443 : 80;
    }
}

// WinHTTP-based HTTP/HTTPS request (POST or GET)
std::string winHttpRequest(const std::string& method, const std::string& url,
                                   const std::string& body, const std::string& contentType,
                                   std::string* outLocationHeader) {
    bool isHttps = (url.rfind("https", 0) == 0);
    std::string host, path;
    int port = isHttps ? 443 : 80;
    parseUrl(url, host, port, path);

    // Convert to wide strings
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wMethod(method.begin(), method.end());

    HINTERNET hSession = WinHttpOpen(L"Easel/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(), wPath.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    // Set timeouts (10s)
    WinHttpSetTimeouts(hRequest, 10000, 10000, 10000, 10000);

    // Add content-type header if body present
    std::wstring headers;
    if (!contentType.empty()) {
        headers = L"Content-Type: " + std::wstring(contentType.begin(), contentType.end());
    }

    BOOL result = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)headers.size(),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    if (!result || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read Location header if requested
    if (outLocationHeader) {
        DWORD headerSize = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &headerSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headerSize > 0) {
            std::vector<wchar_t> buf(headerSize / sizeof(wchar_t));
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     buf.data(), &headerSize, WINHTTP_NO_HEADER_INDEX)) {
                std::wstring wLoc(buf.data());
                int len = WideCharToMultiByte(CP_UTF8, 0, wLoc.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string loc(len - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, wLoc.c_str(), -1, &loc[0], len, nullptr, nullptr);
                *outLocationHeader = loc;
            }
        }
    }

    // Read response body
    std::string response;
    DWORD bytesAvail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
        std::vector<char> buf(bytesAvail);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), bytesAvail, &bytesRead)) {
            response.append(buf.data(), bytesRead);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ─── WHEP HTTP POST (supports HTTP and HTTPS via WinHTTP) ─────────

std::string WHEPSource::whepPost(const std::string& url, const std::string& sdpOffer) {
    std::string location;
    std::string answer;

    if (url.find("/webrtc/offer") != std::string::npos) {
        // Scope WebRTC endpoint — send JSON, receive JSON
        // Escape SDP for JSON (replace newlines and quotes)
        std::string escapedSdp;
        for (char c : sdpOffer) {
            if (c == '"') escapedSdp += "\\\"";
            else if (c == '\n') escapedSdp += "\\n";
            else if (c == '\r') escapedSdp += "\\r";
            else if (c == '\t') escapedSdp += "\\t";
            else if (c == '\\') escapedSdp += "\\\\";
            else escapedSdp += c;
        }
        std::string jsonBody = "{\"sdp\":\"" + escapedSdp + "\",\"type\":\"offer\"}";
        std::string jsonResponse = winHttpRequest("POST", url, jsonBody, "application/json", &location);

        // Extract SDP from JSON response {"sdp": "...", "type": "answer"}
        size_t sdpPos = jsonResponse.find("\"sdp\"");
        if (sdpPos != std::string::npos) {
            size_t valStart = jsonResponse.find('"', sdpPos + 5);
            if (valStart != std::string::npos) {
                // Find end of string value (handle escaped quotes)
                size_t i = valStart + 1;
                while (i < jsonResponse.size()) {
                    if (jsonResponse[i] == '\\') { i += 2; continue; }
                    if (jsonResponse[i] == '"') break;
                    i++;
                }
                std::string raw = jsonResponse.substr(valStart + 1, i - valStart - 1);
                // Unescape
                for (size_t j = 0; j < raw.size(); j++) {
                    if (raw[j] == '\\' && j + 1 < raw.size()) {
                        if (raw[j+1] == 'n') { answer += '\n'; j++; }
                        else if (raw[j+1] == 'r') { answer += '\r'; j++; }
                        else if (raw[j+1] == 't') { answer += '\t'; j++; }
                        else if (raw[j+1] == '"') { answer += '"'; j++; }
                        else if (raw[j+1] == '\\') { answer += '\\'; j++; }
                        else answer += raw[j];
                    } else {
                        answer += raw[j];
                    }
                }
            }
        }
    } else {
        // Standard WHEP — send/receive raw SDP
        answer = winHttpRequest("POST", url, sdpOffer, "application/sdp", &location);
    }

    if (!location.empty()) {
        m_teardownUrl = location;
        if (m_teardownUrl.front() == '/') {
            std::string host, path;
            int port = 80;
            parseUrl(url, host, port, path);
            std::string scheme = (url.rfind("https", 0) == 0) ? "https" : "http";
            m_teardownUrl = scheme + "://" + host + ":" + std::to_string(port) + m_teardownUrl;
        }
    }

    return answer;
}

// ─── Decoder init/cleanup ────────────────────────────────────────────────────

bool WHEPSource::initDecoder() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::cerr << "[WHEP] H.264 decoder not found\n";
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    // Low-latency decode settings
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_count = 1;  // Single thread for minimum latency

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "[WHEP] Failed to open H.264 decoder\n";
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    return true;
}

void WHEPSource::cleanupDecoder() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}

bool WHEPSource::decodeNalUnit(const uint8_t* data, int size, AVFrame* frame) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    // Prepend Annex-B start code
    std::vector<uint8_t> annexB(4 + size);
    annexB[0] = 0; annexB[1] = 0; annexB[2] = 0; annexB[3] = 1;
    std::memcpy(annexB.data() + 4, data, size);

    pkt->data = annexB.data();
    pkt->size = (int)annexB.size();

    bool decoded = false;
    int ret = avcodec_send_packet(m_codecCtx, pkt);
    if (ret >= 0) {
        ret = avcodec_receive_frame(m_codecCtx, frame);
        if (ret >= 0) decoded = true;
    }

    av_packet_free(&pkt);
    return decoded;
}

// ─── Decode loop (background thread) ────────────────────────────────────────

void WHEPSource::decodeLoop() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;

    while (m_running.load()) {
        // Grab pending NAL units
        std::vector<std::vector<uint8_t>> nals;
        {
            std::lock_guard<std::mutex> lk(m_nalMutex);
            nals.swap(m_pendingNals);
        }

        if (nals.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        for (auto& nal : nals) {
            if (!m_running.load()) break;
            if (!decodeNalUnit(nal.data(), (int)nal.size(), frame)) continue;

            int w = frame->width;
            int h = frame->height;

            // Init/reinit sws if dimensions changed
            if (!m_swsCtx || w != m_width || h != m_height) {
                if (m_swsCtx) sws_freeContext(m_swsCtx);
                m_swsCtx = sws_getContext(
                    w, h, (AVPixelFormat)frame->format,
                    w, h, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr
                );
                m_width = w;
                m_height = h;
            }

            // Convert to RGBA into triple buffer
            int writeIdx = (m_writeIndex.load() + 1) % 3;
            auto& buf = m_buffers[writeIdx];
            buf.data.resize(w * h * 4);
            buf.width = w;
            buf.height = h;

            // OpenGL expects bottom-up, so flip vertically during sws_scale
            uint8_t* dstSlice[1] = { buf.data.data() + (h - 1) * w * 4 };
            int dstStride[1] = { -(w * 4) };
            sws_scale(m_swsCtx, frame->data, frame->linesize, 0, h, dstSlice, dstStride);

            buf.ready.store(true);
            m_writeIndex.store(writeIdx);
        }
    }

    av_frame_free(&frame);
}

// ─── Auto-discover WHEP URL from Etherea ────────────────────────────────────

std::string WHEPSource::discoverUrl(const std::string& baseUrl) {
    // First, ensure the local mediamtx has the stream by setting up the pull source
    std::string statusJson = winHttpRequest("GET", baseUrl + "/api/scope/status", "", "");
    if (!statusJson.empty()) {
        // Extract rtmp_url
        size_t pos = statusJson.find("\"rtmp_url\"");
        if (pos != std::string::npos) {
            size_t valStart = statusJson.find('"', pos + 10);
            if (valStart != std::string::npos) {
                size_t valEnd = statusJson.find('"', valStart + 1);
                if (valEnd != std::string::npos) {
                    std::string rtmpUrl = statusJson.substr(valStart + 1, valEnd - valStart - 1);
                    // Add pull source to local mediamtx (idempotent — ignores if already exists)
                    std::string body = "{\"source\": \"" + rtmpUrl + "\"}";
                    winHttpRequest("POST", "http://localhost:9997/v3/config/paths/add/longlive", body, "application/json");
                    whepLog("[WHEP] Configured local mediamtx pull from: " + rtmpUrl);
                }
            }
        }
    }
    // Use local mediamtx WHEP (localhost — no NAT/TURN issues)
    return "http://localhost:8889/longlive/whep";
}

// ─── Connect ────────────────────────────────────────────────────────────────

bool WHEPSource::connect(const std::string& whepUrl) {
    disconnect();
    m_whepUrl = whepUrl;

    whepLog("[WHEP] connect() called with url: " + whepUrl);

    try {
        if (!initDecoder()) {
            whepLog("[WHEP] initDecoder failed");
            return false;
        }
        whepLog("[WHEP] decoder initialized");

        // Start decode thread
        m_running.store(true);
        m_decodeThread = std::thread(&WHEPSource::decodeLoop, this);

        // Configure WebRTC
        rtc::Configuration config;
        config.disableAutoNegotiation = true;

        // Fetch ICE servers (TURN credentials from Etherea if available)
        std::string turnJson = winHttpRequest("GET", "http://localhost:7860/api/turn-credentials", "", "");
        if (!turnJson.empty()) {
            whepLog("[WHEP] Got TURN config (" + std::to_string(turnJson.size()) + " bytes)");
            // Parse iceServers array — extract urls, username, credential per object
            size_t pos = 0;
            while ((pos = turnJson.find("\"urls\"", pos)) != std::string::npos) {
                // Extract URL value
                size_t valStart = turnJson.find('"', pos + 6);
                if (valStart == std::string::npos) break;
                size_t valEnd = turnJson.find('"', valStart + 1);
                if (valEnd == std::string::npos) break;
                std::string url = turnJson.substr(valStart + 1, valEnd - valStart - 1);

                // Find the enclosing { } block for this object
                size_t blockStart = turnJson.rfind('{', pos);
                size_t blockEnd = turnJson.find('}', pos);
                std::string username, credential;
                if (blockStart != std::string::npos && blockEnd != std::string::npos) {
                    std::string block = turnJson.substr(blockStart, blockEnd - blockStart + 1);
                    auto extractField = [&](const std::string& field) -> std::string {
                        size_t fp = block.find("\"" + field + "\"");
                        if (fp == std::string::npos) return "";
                        size_t colonPos = block.find(':', fp + field.size() + 2);
                        if (colonPos == std::string::npos) return "";
                        // Skip whitespace after colon
                        size_t afterColon = block.find_first_not_of(" \t\r\n", colonPos + 1);
                        if (afterColon == std::string::npos) return "";
                        // Check for null
                        if (block.compare(afterColon, 4, "null") == 0) return "";
                        // Extract string value
                        if (block[afterColon] != '"') return "";
                        size_t ve = block.find('"', afterColon + 1);
                        if (ve == std::string::npos) return "";
                        return block.substr(afterColon + 1, ve - afterColon - 1);
                    };
                    username = extractField("username");
                    credential = extractField("credential");
                }

                if (url.find("turn:") == 0 && !username.empty() && !credential.empty()) {
                    // Parse turn:host:port?transport=xxx
                    rtc::IceServer turn(url);
                    turn.username = username;
                    turn.password = credential;
                    config.iceServers.push_back(turn);
                    whepLog("[WHEP] Added TURN server: " + url);
                } else if (url.find("stun:") == 0) {
                    config.iceServers.push_back(rtc::IceServer(url));
                    whepLog("[WHEP] Added STUN server: " + url);
                }
                pos = valEnd + 1;
            }
        }

        // Fallback if no servers were configured
        if (config.iceServers.empty()) {
            config.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
            whepLog("[WHEP] Using fallback STUN server");
        }

        m_pc = std::make_shared<rtc::PeerConnection>(config);
        whepLog("[WHEP] PeerConnection created");

        m_pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
            whepLog("[WHEP] ICE state: " + std::to_string((int)state));
        });

        m_pc->onStateChange([this](rtc::PeerConnection::State state) {
            whepLog("[WHEP] PeerConnection state: " + std::to_string((int)state));
            if (state == rtc::PeerConnection::State::Connected) {
                m_connected.store(true);
                whepLog("[WHEP] Connected!");
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                m_connected.store(false);
            }
        });

        m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            whepLog("[WHEP] GatheringState: " + std::to_string((int)state));
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                // Run signaling on a separate thread to avoid blocking libjuice's ICE thread
                std::thread([this]() {
                    try {
                        auto desc = m_pc->localDescription();
                        if (!desc) {
                            whepLog("[WHEP] No local description after gathering");
                            return;
                        }

                        std::string offer = std::string(desc.value());

                        // Strip ice2 option (some servers don't support it)
                        size_t ice2Pos = offer.find("a=ice-options:ice2,trickle\r\n");
                        if (ice2Pos != std::string::npos) {
                            offer.replace(ice2Pos, 27, "a=ice-options:trickle\r\n");
                        }

                        whepLog("[WHEP] Sending offer (" + std::to_string(offer.size()) + " bytes):\n" + offer);

                        std::string answer = whepPost(m_whepUrl, offer);
                        if (answer.empty()) {
                            whepLog("[WHEP] Empty answer from server");
                            return;
                        }

                        whepLog("[WHEP] Got answer (" + std::to_string(answer.size()) + " bytes):\n" + answer);

                        rtc::Description remoteDesc(answer, rtc::Description::Type::Answer);
                        m_pc->setRemoteDescription(remoteDesc);
                        whepLog("[WHEP] Remote description set");
                    } catch (const std::exception& e) {
                        whepLog("[WHEP] Signaling error: " + std::string(e.what()));
                    }
                }).detach();
            }
        });

        m_pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
            whepLog("[WHEP] Track received: " + std::string(track->mid()));
            m_track = track;

            auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
            track->setMediaHandler(depacketizer);

            track->onMessage([this](rtc::binary msg) {
                auto* data = reinterpret_cast<const uint8_t*>(msg.data());
                int size = (int)msg.size();
                if (size <= 0) return;

                std::lock_guard<std::mutex> lk(m_nalMutex);
                m_pendingNals.emplace_back(data, data + size);
            }, nullptr);
        });

        // Add recvonly video track
        rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
        video.addH264Codec(96);
        m_track = m_pc->addTrack(video);

        // Set up H.264 depacketizer on our track (onTrack won't fire for tracks we add)
        auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        m_track->setMediaHandler(depacketizer);
        m_track->onMessage([this](rtc::binary msg) {
            auto* data = reinterpret_cast<const uint8_t*>(msg.data());
            int size = (int)msg.size();
            if (size <= 0) return;
            static int msgCount = 0;
            if (++msgCount <= 5) whepLog("[WHEP] onMessage: " + std::to_string(size) + " bytes (msg #" + std::to_string(msgCount) + ")");
            std::lock_guard<std::mutex> lk(m_nalMutex);
            m_pendingNals.emplace_back(data, data + size);
        }, nullptr);

        whepLog("[WHEP] Track added with depacketizer, setting local description...");

        // Generate and set local description (triggers ICE gathering)
        m_pc->setLocalDescription();

        whepLog("[WHEP] Gathering ICE candidates...");
        return true;
    } catch (const std::exception& e) {
        whepLog("[WHEP] Connect failed: " + std::string(e.what()));
        disconnect();
        return false;
    }
}

// ─── Disconnect ──────────────────────────────────────────────────────────────

WHEPSource::~WHEPSource() {
    disconnect();
}

void WHEPSource::disconnect() {
    m_running.store(false);
    m_connected.store(false);

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    if (m_track) {
        m_track->close();
        m_track.reset();
    }

    if (m_pc) {
        m_pc->close();
        m_pc.reset();
    }

    cleanupDecoder();

    // WHEP teardown (DELETE to Location URL)
    if (!m_teardownUrl.empty()) {
        winHttpRequest("DELETE", m_teardownUrl, "", "");
        m_teardownUrl.clear();
    }

    m_width = 0;
    m_height = 0;
    m_whepUrl.clear();
}

// ─── Update (main thread — upload decoded frames to GPU) ─────────────────────

void WHEPSource::update() {
    int readIdx = m_writeIndex.load();  // Read latest written
    auto& buf = m_buffers[readIdx];

    if (!buf.ready.load()) return;
    if (buf.width <= 0 || buf.height <= 0) return;

    if (buf.width != m_texture.width() || buf.height != m_texture.height()) {
        m_texture.createEmpty(buf.width, buf.height);
    }

    m_texture.updateData(buf.data.data(), buf.width, buf.height);
    buf.ready.store(false);
    m_readIndex.store(readIdx);
}

#endif // HAS_WHEP
