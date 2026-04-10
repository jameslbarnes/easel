#include "speech/EthereaClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#define MAKEWORD(a,b) 0
inline int WSAStartup(int, void*) { return 0; }
inline void WSACleanup() {}
struct WSADATA { int dummy; };
#endif

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ─── Logging ───────────────────────────────────────────────────────────────

static void etLog(const std::string& msg) {
    std::ofstream f("etherea_debug.log", std::ios::app);
    f << msg << std::endl;
}

// ─── Minimal JSON helpers ──────────────────────────────────────────────────

static std::string jsonStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            if (json[pos] == 'n') result += '\n';
            else if (json[pos] == 't') result += '\t';
            else if (json[pos] == 'u') pos += 4; // skip unicode
            else result += json[pos];
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

static bool jsonBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return (pos < json.size() && json[pos] == 't');
}

static double jsonNum(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return std::atof(json.c_str() + pos);
}

// Parse a JSON array of strings: ["a", "b", "c"]
static std::vector<std::string> jsonStrArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    size_t end = json.find(']', pos);
    if (end == std::string::npos) return result;
    std::string arr = json.substr(pos + 1, end - pos - 1);
    // Extract each quoted string
    size_t p = 0;
    while ((p = arr.find('"', p)) != std::string::npos) {
        p++;
        size_t q = arr.find('"', p);
        if (q == std::string::npos) break;
        result.push_back(arr.substr(p, q - p));
        p = q + 1;
    }
    return result;
}

// ─── Base64 (for WebSocket handshake key) ──────────────────────────────────

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        result += b64chars[(n >> 18) & 63];
        result += b64chars[(n >> 12) & 63];
        result += (i + 1 < len) ? b64chars[(n >> 6) & 63] : '=';
        result += (i + 2 < len) ? b64chars[n & 63] : '=';
    }
    return result;
}

// ─── Buffered WebSocket reader ─────────────────────────────────────────────

// Persistent read buffer so handshake leftovers feed into frame parsing.
struct WSReader {
    SOCKET sock = INVALID_SOCKET;
    std::string buf;

    // Read exactly `len` bytes into `out`, using buffer + socket.
    bool readExact(char* out, int len, int timeoutMs = 10000) {
        int have = 0;
        // Drain from buffer first
        if (!buf.empty()) {
            int take = std::min(len, (int)buf.size());
            memcpy(out, buf.data(), take);
            buf.erase(0, take);
            have = take;
        }
        // Read remainder from socket
        while (have < len) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
            int sel = select(0, &readSet, nullptr, nullptr, &tv);
#else
            int sel = select(sock + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (sel <= 0) return false;
            int n = recv(sock, out + have, len - have, 0);
            if (n <= 0) return false;
            have += n;
        }
        return true;
    }
};

struct WSFrame {
    uint8_t opcode;
    std::string payload;
    bool fin;
};

static bool wsReadFrame(WSReader& rd, WSFrame& frame) {
    uint8_t header[2];
    if (!rd.readExact((char*)header, 2)) return false;

    frame.fin = (header[0] & 0x80) != 0;
    frame.opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        if (!rd.readExact((char*)ext, 2)) return false;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!rd.readExact((char*)ext, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (!rd.readExact((char*)mask, 4)) return false;
    }

    frame.payload.resize((size_t)len);
    if (len > 0) {
        if (!rd.readExact(&frame.payload[0], (int)len)) return false;
        if (masked) {
            for (size_t i = 0; i < len; i++)
                frame.payload[i] ^= mask[i % 4];
        }
    }
    return true;
}

static bool wsSendFrame(SOCKET sock, uint8_t opcode, const std::string& data) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode);

    // Client frames must be masked
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xFF);

    if (data.size() < 126) {
        frame.push_back(0x80 | (uint8_t)data.size());
    } else if (data.size() < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)((data.size() >> 8) & 0xFF));
        frame.push_back((uint8_t)(data.size() & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        uint64_t len = data.size();
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }

    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < data.size(); i++)
        frame.push_back((uint8_t)data[i] ^ mask[i % 4]);

    return send(sock, (const char*)frame.data(), (int)frame.size(), 0) == (int)frame.size();
}

// ─── HTTP helper (blocking GET) ────────────────────────────────────────────

static std::string httpGet(const std::string& host, int port, const std::string& path) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        WSACleanup();
        return "";
    }
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(result); WSACleanup(); return ""; }
    if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        closesocket(sock); freeaddrinfo(result); WSACleanup(); return "";
    }
    freeaddrinfo(result);

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), (int)req.size(), 0);

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }
    closesocket(sock);
    WSACleanup();

    size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd != std::string::npos) return response.substr(headerEnd + 4);
    return response;
}

// ─── EthereaClient implementation ──────────────────────────────────────────

EthereaClient::~EthereaClient() {
    disconnect();
}

void EthereaClient::parseUrl() {
    m_host = "localhost";
    m_port = 7860;
    std::string u = m_baseUrl;
    if (u.substr(0, 7) == "http://") u = u.substr(7);
    size_t slashPos = u.find('/');
    std::string hostPort = (slashPos != std::string::npos) ? u.substr(0, slashPos) : u;
    size_t colonPos = hostPort.find(':');
    m_host = (colonPos != std::string::npos) ? hostPort.substr(0, colonPos) : hostPort;
    if (colonPos != std::string::npos) m_port = std::stoi(hostPort.substr(colonPos + 1));
}

bool EthereaClient::connect(const std::string& baseUrl, const std::string& sessionId) {
    if (m_running.load()) disconnect();
    m_baseUrl = baseUrl;
    m_sessionId = sessionId;
    parseUrl();

    {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_fullTranscript.clear();
        m_latestWords.clear();
        m_hints.clear();
        m_prompt.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        m_pendingEvents.clear();
    }

    m_running.store(true);
    m_wsThread = std::thread(&EthereaClient::wsLoop, this);
    m_sseThread = std::thread(&EthereaClient::sseLoop, this);
    etLog("EthereaClient: connecting to " + baseUrl + " session=" + (sessionId.empty() ? "(default)" : sessionId));
    return true;
}

void EthereaClient::disconnect() {
    m_running.store(false);
    m_wsConnected.store(false);
    m_sseConnected.store(false);
    if (m_wsThread.joinable()) m_wsThread.join();
    if (m_sseThread.joinable()) m_sseThread.join();
}

void EthereaClient::poll() {
    std::vector<TranscriptEvent> events;
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        events.swap(m_pendingEvents);
    }
    for (auto& ev : events) {
        if (m_transcriptCb) m_transcriptCb(ev.text, ev.isFinal);
    }
}

std::vector<EthereaSession> EthereaClient::fetchSessions(const std::string& baseUrl) {
    std::vector<EthereaSession> sessions;
    std::string host = "localhost";
    int port = 7860;
    {
        std::string u = baseUrl;
        if (u.substr(0, 7) == "http://") u = u.substr(7);
        size_t slashPos = u.find('/');
        std::string hostPort = (slashPos != std::string::npos) ? u.substr(0, slashPos) : u;
        size_t colonPos = hostPort.find(':');
        host = (colonPos != std::string::npos) ? hostPort.substr(0, colonPos) : hostPort;
        if (colonPos != std::string::npos) port = std::stoi(hostPort.substr(colonPos + 1));
    }

    std::string body = httpGet(host, port, "/api/sessions");
    if (body.empty()) return sessions;

    size_t pos = 0;
    while ((pos = body.find('{', pos)) != std::string::npos) {
        size_t end = body.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = body.substr(pos, end - pos + 1);
        pos = end + 1;
        std::string sid = jsonStr(obj, "session_id");
        if (sid.empty()) continue;
        EthereaSession s;
        s.id = sid;
        s.idleSeconds = (float)jsonNum(obj, "idle_seconds");
        s.transcriptLength = (int)jsonNum(obj, "transcript_length");
        s.isPaused = jsonBool(obj, "is_paused");
        sessions.push_back(s);
    }
    return sessions;
}

// ─── WebSocket thread (real-time transcript) ───────────────────────────────

void EthereaClient::wsLoop() {
    srand((unsigned)time(nullptr));

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        etLog("EthereaClient WS: WSAStartup failed");
        return;
    }

    while (m_running.load()) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(m_port);
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result) != 0) {
            etLog("EthereaClient WS: DNS failed");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(result); std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            etLog("EthereaClient WS: connect failed (is Etherea running?)");
            closesocket(sock); freeaddrinfo(result);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        freeaddrinfo(result);

        // Generate WebSocket key
        uint8_t keyBytes[16];
        for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)(rand() & 0xFF);
        std::string wsKey = base64Encode(keyBytes, 16);

        // WebSocket upgrade request
        std::string path = "/ws/transcript";
        if (!m_sessionId.empty()) path += "?session_id=" + m_sessionId;

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wsKey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        send(sock, request.c_str(), (int)request.size(), 0);

        // Read upgrade response
        std::string response;
        char buf[4096];
        while (m_running.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv = { 3, 0 };
#ifdef _WIN32
            int sel = select(0, &readSet, nullptr, nullptr, &tv);
#else
            int sel = select(sock + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (sel <= 0) break;
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            response += buf;
            if (response.find("\r\n\r\n") != std::string::npos) break;
        }

        if (response.find("101") == std::string::npos) {
            etLog("EthereaClient WS: upgrade failed — " + response.substr(0, 80));
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // Preserve any leftover data after HTTP headers (may contain first WS frame)
        WSReader rd;
        rd.sock = sock;
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos && headerEnd + 4 < response.size()) {
            rd.buf = response.substr(headerEnd + 4);
        }

        m_wsConnected.store(true);
        etLog("EthereaClient WS: connected");

        // Read WebSocket frames
        while (m_running.load()) {
            WSFrame frame;
            if (!wsReadFrame(rd, frame)) {
                etLog("EthereaClient WS: read failed");
                break;
            }

            if (frame.opcode == 0x8) { // close
                etLog("EthereaClient WS: server closed");
                break;
            }
            if (frame.opcode == 0x9) { // ping
                wsSendFrame(sock, 0xA, frame.payload); // pong
                continue;
            }
            if (frame.opcode != 0x1) continue; // only text frames

            // Parse JSON message
            std::string type = jsonStr(frame.payload, "type");
            if (type == "transcript") {
                std::string text = jsonStr(frame.payload, "text");
                bool isFinal = jsonBool(frame.payload, "is_final");

                {
                    std::lock_guard<std::mutex> lk(m_dataMutex);
                    m_latestWords = text;
                    if (isFinal && !text.empty()) {
                        if (!m_fullTranscript.empty()) m_fullTranscript += " ";
                        m_fullTranscript += text;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(m_eventMutex);
                    m_pendingEvents.push_back({text, isFinal});
                }
            } else if (type == "ping") {
                // server keepalive, no action needed
            }
        }

        m_wsConnected.store(false);
        closesocket(sock);
        if (m_running.load()) {
            etLog("EthereaClient WS: reconnecting in 3s...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    WSACleanup();
}

// ─── SSE thread (hints, prompt, state) ─────────────────────────────────────

void EthereaClient::sseLoop() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        etLog("EthereaClient SSE: WSAStartup failed");
        return;
    }

    std::string path = "/stream";
    if (!m_sessionId.empty()) path += "?session_id=" + m_sessionId;

    while (m_running.load()) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        std::string portStr = std::to_string(m_port);
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result) != 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(result); std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            closesocket(sock); freeaddrinfo(result);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        freeaddrinfo(result);

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n"
            "Accept: text/event-stream\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(sock, request.c_str(), (int)request.size(), 0);

        std::string buffer;
        char chunk[4096];
        bool headersSkipped = false;
        m_sseConnected.store(true);
        etLog("EthereaClient SSE: connected");

        while (m_running.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv = { 1, 0 };
#ifdef _WIN32
            int sel = select(0, &readSet, nullptr, nullptr, &tv);
#else
            int sel = select(sock + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (sel == 0) continue;
            if (sel == SOCKET_ERROR) break;

            int bytesRead = recv(sock, chunk, sizeof(chunk) - 1, 0);
            if (bytesRead <= 0) break;
            chunk[bytesRead] = '\0';
            buffer += chunk;

            if (!headersSkipped) {
                size_t headerEnd = buffer.find("\r\n\r\n");
                if (headerEnd == std::string::npos) continue;
                buffer = buffer.substr(headerEnd + 4);
                headersSkipped = true;
            }

            // Process complete SSE messages
            size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string message = buffer.substr(0, pos);
                buffer = buffer.substr(pos + 2);

                // Parse data: lines
                std::string data;
                std::istringstream stream(message);
                std::string line;
                while (std::getline(stream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.size() >= 5 && line.substr(0, 5) == "data:") {
                        std::string payload = line.substr(5);
                        if (!payload.empty() && payload[0] == ' ') payload = payload.substr(1);
                        data += payload;
                    }
                }

                if (data.empty() || data[0] != '{') continue;

                // Extract hints/suggestions
                auto suggestions = jsonStrArray(data, "suggestions");

                // Extract prompt
                std::string promptVal = jsonStr(data, "prompt");

                // Extract full_transcript (fallback if WS not connected)
                std::string transcript = jsonStr(data, "full_transcript");
                std::string latestWords = jsonStr(data, "latest_words");

                // Extract reset_cache
                bool resetCache = jsonBool(data, "reset_cache");

                {
                    std::lock_guard<std::mutex> lk(m_dataMutex);
                    if (!suggestions.empty()) m_hints = suggestions;
                    if (!promptVal.empty()) m_prompt = promptVal;
                    // Only update transcript from SSE if WebSocket is not connected
                    if (!m_wsConnected.load()) {
                        if (!transcript.empty()) m_fullTranscript = transcript;
                        if (!latestWords.empty()) m_latestWords = latestWords;
                    }
                }
                m_resetCache.store(resetCache);
            }
        }

        m_sseConnected.store(false);
        closesocket(sock);
        if (m_running.load()) {
            etLog("EthereaClient SSE: reconnecting in 3s...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    WSACleanup();
}
