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

void whepLog(const std::string& msg) {
    std::ofstream f("whep_debug.log", std::ios::app);
    f << msg << std::endl;
    f.close();
    std::cerr << msg << std::endl;
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
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

// Cross-platform HTTP request (POSIX sockets on macOS/Linux, WinHTTP on Windows)
std::string httpRequest(const std::string& method, const std::string& url,
                        const std::string& body, const std::string& contentType,
                        std::string* outLocationHeader) {
    std::string host, path;
    int port = 80;
    parseUrl(url, host, port, path);

#ifdef _WIN32
    // --- WinHTTP implementation ---
    bool isHttps = (url.rfind("https", 0) == 0);
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

    WinHttpSetTimeouts(hRequest, 10000, 10000, 10000, 10000);

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

#else
    // --- libcurl implementation (HTTP + HTTPS) ---
    struct CurlWriteCtx {
        std::string data;
        static size_t callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
            size_t total = size * nmemb;
            static_cast<CurlWriteCtx*>(userdata)->data.append(ptr, total);
            return total;
        }
    };
    struct CurlHeaderCtx {
        std::string location;
        static size_t callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
            size_t total = size * nmemb;
            std::string line(ptr, total);
            auto* ctx = static_cast<CurlHeaderCtx*>(userdata);
            if (line.compare(0, 10, "Location: ") == 0 || line.compare(0, 10, "location: ") == 0) {
                ctx->location = line.substr(10);
                while (!ctx->location.empty() && (ctx->location.back() == '\r' || ctx->location.back() == '\n'))
                    ctx->location.pop_back();
            }
            return total;
        }
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        whepLog("[HTTP] curl_easy_init failed");
        return "";
    }

    CurlWriteCtx writeCtx;
    CurlHeaderCtx headerCtx;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCtx::callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCtx::callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    struct curl_slist* curlHeaders = nullptr;
    if (!contentType.empty()) {
        curlHeaders = curl_slist_append(curlHeaders, ("Content-Type: " + contentType).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);
    }

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode res = curl_easy_perform(curl);
    if (curlHeaders) curl_slist_free_all(curlHeaders);

    if (res != CURLE_OK) {
        whepLog("[HTTP] curl error: " + std::string(curl_easy_strerror(res)) + " for " + url);
        curl_easy_cleanup(curl);
        return "";
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    whepLog("[HTTP] " + method + " " + url + " -> " + std::to_string(httpCode) + " (" + std::to_string(writeCtx.data.size()) + " bytes)");

    if (outLocationHeader && !headerCtx.location.empty()) {
        *outLocationHeader = headerCtx.location;
    }

    curl_easy_cleanup(curl);
    return writeCtx.data;
#endif
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
        std::string jsonBody = "{\"sdp\":\"" + escapedSdp + "\",\"type\":\"offer\","
            "\"initialParameters\":{\"input_mode\":\"text\"}}";
        std::string jsonResponse = httpRequest("POST", url, jsonBody, "application/json", &location);

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
        answer = httpRequest("POST", url, sdpOffer, "application/sdp", &location);
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
            if (nal.empty()) continue;

            uint8_t nalType = nal[0] & 0x1F;

            // Wait for SPS (7) before feeding anything to the decoder
            if (!m_gotKeyframe) {
                if (nalType == 7) {
                    m_gotKeyframe = true;
                    whepLog("[WHEP] Got SPS — starting decode");
                } else {
                    continue; // Discard until we get SPS
                }
            }

            // On packet loss (detected by sequence gap), reset and wait for next keyframe
            // This is handled by clearing m_gotKeyframe when we detect loss
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

            // Debug: save first good frame to disk
            static bool savedFrame = false;
            if (!savedFrame) {
                savedFrame = true;
                FILE* f = fopen("/tmp/whep_frame.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    // RGBA -> RGB
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            int idx = (y * w + x) * 4;
                            fwrite(&buf.data[idx], 1, 3, f);
                        }
                    }
                    fclose(f);
                    whepLog("[WHEP] Saved debug frame to /tmp/whep_frame.ppm (" + std::to_string(w) + "x" + std::to_string(h) + ")");
                }
            }
        }
    }

    av_frame_free(&frame);
}

// ─── Auto-discover WHEP URL from Etherea ────────────────────────────────────

std::string WHEPSource::discoverUrl(const std::string& baseUrl) {
    // First, ensure the local mediamtx has the stream by setting up the pull source
    std::string statusJson = httpRequest("GET", baseUrl + "/api/scope/status", "", "");
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
                    httpRequest("POST", "http://localhost:9997/v3/config/paths/add/longlive", body, "application/json");
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

    // For remote URLs on macOS, use WKWebView (browser's native WebRTC stack)
    // This gives identical ICE/DTLS/NACK handling to Safari
    bool isLocal = (whepUrl.find("localhost") != std::string::npos ||
                    whepUrl.find("127.0.0.1") != std::string::npos);
#ifdef __APPLE__
    if (!isLocal) {
        m_useWebView = true;
        m_statusText = "connecting (WebView)";
        // Build ICE servers JSON from etherea
        std::string iceJson = "[{\"urls\":\"stun:stun.l.google.com:19302\"}]";
        std::string turnJson = httpRequest("GET", "http://localhost:7860/api/turn-credentials", "", "");
        if (!turnJson.empty()) {
            // Convert etherea format to browser RTCIceServer format
            // The etherea API returns {"iceServers": [...]}
            size_t arrStart = turnJson.find("[");
            size_t arrEnd = turnJson.rfind("]");
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                iceJson = turnJson.substr(arrStart, arrEnd - arrStart + 1);
                // Fix field names: "urls" (singular in etherea) to "urls" (browser expects "urls")
                // etherea already uses "urls" so this should work
            }
        }
        whepLog("[WHEP] Using WKWebView for remote WHEP");
        startWebView(whepUrl, iceJson);
        return true;
    }
#endif

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
        std::string turnJson = httpRequest("GET", "http://localhost:7860/api/turn-credentials", "", "");
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

        m_pc->onIceStateChange([this](rtc::PeerConnection::IceState state) {
            const char* names[] = {"New", "Checking", "Connected", "Completed", "Disconnected", "Failed", "Closed"};
            int idx = (int)state;
            std::string name = (idx >= 0 && idx < 7) ? names[idx] : std::to_string(idx);
            whepLog("[WHEP] ICE state: " + name);
            m_statusText = "ICE " + name;
        });

        m_pc->onStateChange([this](rtc::PeerConnection::State state) {
            const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            int idx = (int)state;
            std::string name = (idx >= 0 && idx < 6) ? names[idx] : std::to_string(idx);
            whepLog("[WHEP] PeerConnection state: " + name);
            if (state == rtc::PeerConnection::State::Connected) {
                m_connected.store(true);
                m_failed.store(false);
                m_statusText = "connected";
                whepLog("[WHEP] Connected!");
                // Request a keyframe via RTCP PLI
                if (m_track) {
                    try {
                        // Send PLI (Picture Loss Indication) to request IDR frame
                        const uint32_t mediaSsrc = 0; // Will be filled by the stack
                        m_track->requestKeyframe();
                        whepLog("[WHEP] Requested keyframe (PLI)");
                    } catch (...) {
                        whepLog("[WHEP] PLI request not supported");
                    }
                }
            } else if (state == rtc::PeerConnection::State::Failed) {
                m_connected.store(false);
                m_failed.store(true);
                m_statusText = "ICE failed - server may lack TURN config";
                whepLog("[WHEP] ICE failed - remote server likely missing TURN relay candidates");
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Closed) {
                m_connected.store(false);
            }
        });

        m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            const char* gatherNames[] = {"New", "InProgress", "Complete"};
            int gIdx = (int)state;
            whepLog("[WHEP] GatheringState: " + std::string((gIdx >= 0 && gIdx < 3) ? gatherNames[gIdx] : std::to_string(gIdx)));
            if (state == rtc::PeerConnection::GatheringState::InProgress) {
                m_statusText = "gathering ICE candidates";
            }
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                m_statusText = "signaling";
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

                        std::string signalingUrl = m_whepUrl;

                        std::string answer = whepPost(signalingUrl, offer);
                        if (answer.empty()) {
                            whepLog("[WHEP] Empty answer from server");
                            m_statusText = "no answer from server";
                            m_failed.store(true);
                            return;
                        }

                        whepLog("[WHEP] Got answer (" + std::to_string(answer.size()) + " bytes):\n" + answer);

                        // Remove a=end-of-candidates to allow trickle ICE / peer-reflexive discovery
                        // (the server only has Docker-internal candidates, but peer-reflexive
                        //  candidates will be discovered when the server sends STUN through NAT)
                        size_t eocPos = answer.find("a=end-of-candidates");
                        if (eocPos != std::string::npos) {
                            // Find the start of the line
                            size_t lineStart = answer.rfind('\n', eocPos);
                            if (lineStart == std::string::npos) lineStart = 0;
                            else lineStart++;
                            size_t lineEnd = answer.find('\n', eocPos);
                            if (lineEnd == std::string::npos) lineEnd = answer.size();
                            else lineEnd++;
                            answer.erase(lineStart, lineEnd - lineStart);
                            whepLog("[WHEP] Removed end-of-candidates to allow peer-reflexive discovery");
                        }

                        rtc::Description remoteDesc(answer, rtc::Description::Type::Answer);
                        m_pc->setRemoteDescription(remoteDesc);
                        whepLog("[WHEP] Remote description set");
                    } catch (const std::exception& e) {
                        whepLog("[WHEP] Signaling error: " + std::string(e.what()));
                        m_statusText = std::string("signaling error: ") + e.what();
                        m_failed.store(true);
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

        // Receive raw RTP and manually depacketize H.264
        // (libdatachannel's H264RtpDepacketizer has a bug with mediamtx)
        m_track->onMessage([this](rtc::binary msg) {
            auto* data = reinterpret_cast<const uint8_t*>(msg.data());
            int size = (int)msg.size();
            if (size < 12) return;

            if ((data[0] & 0xC0) != 0x80) return; // Not RTP
            uint8_t pt = data[1] & 0x7F;
            if (pt != 96) return; // Skip RTCP and non-H264

            // Sequence number for packet loss detection
            uint16_t seq = (data[2] << 8) | data[3];
            if (m_seqInitialized) {
                uint16_t expected = m_lastSeq + 1;
                if (seq != expected) {
                    // Packet loss — discard in-progress FU-A and wait for next keyframe
                    if (m_fuInProgress) {
                        m_fuBuffer.clear();
                        m_fuInProgress = false;
                    }
                    m_gotKeyframe = false;
                    // Request new keyframe
                    if (m_track) {
                        try { m_track->requestKeyframe(); } catch (...) {}
                    }
                }
            }
            m_lastSeq = seq;
            m_seqInitialized = true;

            // Parse RTP header
            bool hasExtension = (data[0] >> 4) & 1;
            int csrcCount = data[0] & 0x0F;
            int headerLen = 12 + csrcCount * 4;
            if (hasExtension && headerLen + 4 <= size) {
                int extLen = (data[headerLen + 2] << 8) | data[headerLen + 3];
                headerLen += 4 + extLen * 4;
            }
            if (headerLen >= size) return;

            bool hasPadding = (data[0] >> 5) & 1;
            int payloadLen = size - headerLen;
            if (hasPadding && payloadLen > 0) payloadLen -= data[size - 1];
            if (payloadLen <= 0) return;
            const uint8_t* payload = data + headerLen;

            uint8_t nalType = payload[0] & 0x1F;

            if (nalType >= 1 && nalType <= 23) {
                // Single NAL unit
                std::lock_guard<std::mutex> lk(m_nalMutex);
                m_pendingNals.emplace_back(payload, payload + payloadLen);
            } else if (nalType == 28) {
                // FU-A
                if (payloadLen < 2) return;
                uint8_t fuHeader = payload[1];
                bool isStart = (fuHeader >> 7) & 1;
                bool isEnd = (fuHeader >> 6) & 1;

                if (isStart) {
                    m_fuBuffer.clear();
                    m_fuInProgress = true;
                    uint8_t reconstructed = (payload[0] & 0xE0) | (fuHeader & 0x1F);
                    m_fuBuffer.push_back(reconstructed);
                    m_fuBuffer.insert(m_fuBuffer.end(), payload + 2, payload + payloadLen);
                } else if (m_fuInProgress) {
                    m_fuBuffer.insert(m_fuBuffer.end(), payload + 2, payload + payloadLen);
                }

                if (isEnd && m_fuInProgress) {
                    std::lock_guard<std::mutex> lk(m_nalMutex);
                    m_pendingNals.push_back(std::move(m_fuBuffer));
                    m_fuBuffer.clear();
                    m_fuInProgress = false;
                }
            } else if (nalType == 24) {
                // STAP-A
                std::lock_guard<std::mutex> lk(m_nalMutex);
                int offset = 1;
                while (offset + 2 <= payloadLen) {
                    int nalSize = (payload[offset] << 8) | payload[offset + 1];
                    offset += 2;
                    if (offset + nalSize > payloadLen) break;
                    m_pendingNals.emplace_back(payload + offset, payload + offset + nalSize);
                    offset += nalSize;
                }
            }
        }, nullptr);

        // Also log track open/close/error
        m_track->onOpen([this]() { whepLog("[WHEP] Track opened"); });
        m_track->onClosed([this]() { whepLog("[WHEP] Track closed"); });
        m_track->onError([this](std::string err) { whepLog("[WHEP] Track error: " + err); });

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

    if (m_useWebView) {
        stopWebView();
        m_useWebView = false;
        m_width = 0;
        m_height = 0;
        m_whepUrl.clear();
        return;
    }

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    if (m_dataChannel) {
        m_dataChannel->close();
        m_dataChannel.reset();
    }
    m_useScopeEndpoint = false;

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
        httpRequest("DELETE", m_teardownUrl, "", "");
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
