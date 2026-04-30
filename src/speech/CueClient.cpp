#include "speech/CueClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

void cueLog(const std::string& msg) {
    static int suppressCount = 0;
    static std::string lastMsg;
    if (msg == lastMsg) {
        suppressCount++;
        if (suppressCount % 10 != 0) return;
    } else {
        suppressCount = 0;
        lastMsg = msg;
    }
    std::ofstream f("cue_debug.log", std::ios::app);
    f << msg;
    if (suppressCount > 0) f << " (x" << suppressCount << ")";
    f << std::endl;
}

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        result += kBase64Chars[(n >> 18) & 63];
        result += kBase64Chars[(n >> 12) & 63];
        result += (i + 1 < len) ? kBase64Chars[(n >> 6) & 63] : '=';
        result += (i + 2 < len) ? kBase64Chars[n & 63] : '=';
    }
    return result;
}

struct WSReader {
    SOCKET sock = INVALID_SOCKET;
    std::string buf;
    std::atomic<bool>* running = nullptr;

    bool readExact(char* out, int len) {
        int have = 0;
        if (!buf.empty()) {
            int take = std::min(len, (int)buf.size());
            memcpy(out, buf.data(), take);
            buf.erase(0, take);
            have = take;
        }

        while (have < len && (!running || running->load())) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
#ifdef _WIN32
            int sel = select(0, &readSet, nullptr, nullptr, &tv);
#else
            int sel = select(sock + 1, &readSet, nullptr, nullptr, &tv);
#endif
            if (sel == 0) continue;
            if (sel < 0) return false;
            int n = recv(sock, out + have, len - have, 0);
            if (n <= 0) return false;
            have += n;
        }
        return have == len;
    }
};

struct WSFrame {
    uint8_t opcode = 0;
    std::string payload;
    bool fin = false;
};

bool wsReadFrame(WSReader& rd, WSFrame& frame) {
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
    if (masked && !rd.readExact((char*)mask, 4)) return false;

    frame.payload.resize((size_t)len);
    if (len > 0) {
        if (!rd.readExact(&frame.payload[0], (int)len)) return false;
        if (masked) {
            for (size_t i = 0; i < len; i++) frame.payload[i] ^= mask[i % 4];
        }
    }
    return true;
}

bool wsSendFrame(SOCKET sock, uint8_t opcode, const std::string& data) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode);

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
        for (int i = 7; i >= 0; i--) {
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < data.size(); i++) {
        frame.push_back((uint8_t)data[i] ^ mask[i % 4]);
    }
    return send(sock, (const char*)frame.data(), (int)frame.size(), 0) == (int)frame.size();
}

std::string jsonString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number()) return value.dump();
    if (value.is_null() || value.is_discarded()) return "";
    return value.dump();
}

std::string fieldString(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return "";
    auto it = object.find(key);
    if (it == object.end()) return "";
    return jsonString(*it);
}

bool fieldBool(const nlohmann::json& object, const char* key, bool fallback = false) {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) {
        std::string value = it->get<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return (char)std::tolower(ch);
        });
        return value == "true" || value == "1" || value == "yes";
    }
    return fallback;
}

std::string pcm16Chunk(const float* mono, int count) {
    std::string out;
    if (!mono || count <= 0) return out;
    out.resize((size_t)count * 2);
    for (int i = 0; i < count; i++) {
        float sample = std::max(-1.0f, std::min(1.0f, mono[i]));
        int16_t pcm = (int16_t)std::lrint(sample * 32767.0f);
        out[(size_t)i * 2] = (char)(pcm & 0xFF);
        out[(size_t)i * 2 + 1] = (char)((pcm >> 8) & 0xFF);
    }
    return out;
}

void sleepBackoff(std::atomic<bool>& running, int seconds) {
    for (int tick = 0; tick < seconds * 10 && running.load(); tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

CueClient::~CueClient() {
    disconnect();
}

bool CueClient::connect(const std::string& baseUrl, const std::string& sessionId) {
    if (m_running.load()) disconnect();

    m_baseUrl = baseUrl;
    m_sessionId = sessionId.empty() ? "demo" : sessionId;
    parseUrl();

    {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_fullTranscript.clear();
        m_latestWords.clear();
        m_prompt.clear();
        m_latestVisionDescription.clear();
        m_metadata.clear();
        m_sources.clear();
        m_metadata["cue.session"] = m_sessionId;
    }
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        m_pendingTranscriptEvents.clear();
    }
    m_promptReset.store(false);
    m_running.store(true);
    m_thread = std::thread(&CueClient::eventLoop, this);
    if (m_transcriptionEnabled.load()) {
        m_transcriptionRunning.store(true);
        m_transcriptionThread = std::thread(&CueClient::transcriptionLoop, this);
    }
    cueLog("CueClient: connecting to " + m_baseUrl + " session=" + m_sessionId);
    return true;
}

void CueClient::disconnect() {
    m_running.store(false);
    m_transcriptionRunning.store(false);
    m_audioCv.notify_all();
    m_connected.store(false);
    m_transcriptionConnected.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_transcriptionThread.joinable()) m_transcriptionThread.join();
    {
        std::lock_guard<std::mutex> lk(m_audioMutex);
        m_pendingAudioChunks.clear();
    }
}

void CueClient::poll() {
    std::vector<TranscriptEvent> events;
    {
        std::lock_guard<std::mutex> lk(m_eventMutex);
        events.swap(m_pendingTranscriptEvents);
    }
    for (const auto& event : events) {
        if (m_transcriptCb) m_transcriptCb(event.text, event.isFinal);
    }
}

void CueClient::setTranscriptionEnabled(bool enabled) {
    bool wasEnabled = m_transcriptionEnabled.exchange(enabled);
    if (enabled == wasEnabled) return;

    if (enabled) {
        if (m_running.load() && !m_transcriptionRunning.load()) {
            m_transcriptionRunning.store(true);
            m_transcriptionThread = std::thread(&CueClient::transcriptionLoop, this);
        }
        return;
    }

    m_transcriptionRunning.store(false);
    m_transcriptionConnected.store(false);
    m_audioCv.notify_all();
    if (m_transcriptionThread.joinable()) m_transcriptionThread.join();
    {
        std::lock_guard<std::mutex> lk(m_audioMutex);
        m_pendingAudioChunks.clear();
    }
}

void CueClient::feedAudioSamples(const float* mono, int count, int sampleRate) {
    (void)sampleRate;
    if (!m_running.load() || !m_transcriptionEnabled.load()) return;
    std::string chunk = pcm16Chunk(mono, count);
    if (chunk.empty()) return;

    {
        std::lock_guard<std::mutex> lk(m_audioMutex);
        m_pendingAudioChunks.push_back(std::move(chunk));
        while (m_pendingAudioChunks.size() > 100) {
            m_pendingAudioChunks.erase(m_pendingAudioChunks.begin());
        }
    }
    m_audioCv.notify_one();
}

void CueClient::parseUrl() {
    m_host = "localhost";
    m_port = 8792;

    std::string url = m_baseUrl;
    if (url.rfind("http://", 0) == 0) url = url.substr(7);
    else if (url.rfind("ws://", 0) == 0) url = url.substr(5);
    else if (url.rfind("https://", 0) == 0) {
        url = url.substr(8);
        m_port = 443;
    } else if (url.rfind("wss://", 0) == 0) {
        url = url.substr(6);
        m_port = 443;
    }

    size_t slash = url.find('/');
    std::string hostPort = (slash == std::string::npos) ? url : url.substr(0, slash);
    size_t colon = hostPort.rfind(':');
    if (colon == std::string::npos) {
        m_host = hostPort.empty() ? "localhost" : hostPort;
        return;
    }
    m_host = hostPort.substr(0, colon);
    try {
        m_port = std::stoi(hostPort.substr(colon + 1));
    } catch (...) {
        m_port = 8792;
    }
}

void CueClient::transcriptionLoop() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cueLog("CueClient transcription: WSAStartup failed");
        return;
    }

    int backoff = 2;
    while (m_running.load() && m_transcriptionRunning.load()) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(m_port);
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result) != 0) {
            cueLog("CueClient transcription: DNS failed");
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }

        SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == INVALID_SOCKET) {
            freeaddrinfo(result);
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }
        if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            cueLog("CueClient transcription: connect failed");
            closesocket(sock);
            freeaddrinfo(result);
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }
        freeaddrinfo(result);
        backoff = 2;

        uint8_t keyBytes[16];
        for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)(rand() & 0xFF);
        std::string wsKey = base64Encode(keyBytes, 16);
        std::string path = "/sessions/" + m_sessionId + "/transcription";

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wsKey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        send(sock, request.c_str(), (int)request.size(), 0);

        std::string response;
        char buf[4096];
        while (m_running.load() && m_transcriptionRunning.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv = {3, 0};
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
            cueLog("CueClient transcription: websocket upgrade failed");
            closesocket(sock);
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }

        WSReader reader;
        reader.sock = sock;
        reader.running = &m_transcriptionRunning;
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos && headerEnd + 4 < response.size()) {
            reader.buf = response.substr(headerEnd + 4);
        }

        bool ready = false;
        while (m_running.load() && m_transcriptionRunning.load() && !ready) {
            WSFrame frame;
            if (!wsReadFrame(reader, frame)) break;
            if (frame.opcode == 0x8) break;
            if (frame.opcode == 0x9) {
                wsSendFrame(sock, 0xA, frame.payload);
                continue;
            }
            if (frame.opcode != 0x1) continue;
            auto message = nlohmann::json::parse(frame.payload, nullptr, false);
            std::string type = fieldString(message, "type");
            if (type == "transcriber.ready") ready = true;
            if (type == "error") {
                cueLog("CueClient transcription: " + fieldString(message, "error"));
                break;
            }
        }

        if (!ready) {
            closesocket(sock);
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }

        m_transcriptionConnected.store(true);
        cueLog("CueClient transcription: connected");

        bool sendFailed = false;
        while (m_running.load() && m_transcriptionRunning.load()) {
            std::vector<std::string> chunks;
            {
                std::unique_lock<std::mutex> lk(m_audioMutex);
                m_audioCv.wait_for(lk, std::chrono::milliseconds(250), [this]() {
                    return !m_pendingAudioChunks.empty() ||
                           !m_running.load() ||
                           !m_transcriptionRunning.load();
                });
                chunks.swap(m_pendingAudioChunks);
            }

            for (const auto& chunk : chunks) {
                if (!wsSendFrame(sock, 0x2, chunk)) {
                    sendFailed = true;
                    break;
                }
            }
            if (sendFailed) break;
        }

        m_transcriptionConnected.store(false);
        closesocket(sock);
        if (m_running.load() && m_transcriptionRunning.load()) {
            sleepBackoff(m_transcriptionRunning, backoff);
            backoff = std::min(backoff * 2, 30);
        }
    }

    m_transcriptionConnected.store(false);
    WSACleanup();
}

void CueClient::eventLoop() {
    srand((unsigned)time(nullptr));

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cueLog("CueClient: WSAStartup failed");
        return;
    }

    int backoff = 2;
    while (m_running.load()) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(m_port);
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result) != 0) {
            cueLog("CueClient: DNS failed");
            sleepBackoff(m_running, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }

        SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == INVALID_SOCKET) {
            freeaddrinfo(result);
            sleepBackoff(m_running, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }
        if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            cueLog("CueClient: connect failed");
            closesocket(sock);
            freeaddrinfo(result);
            sleepBackoff(m_running, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }
        freeaddrinfo(result);
        backoff = 2;

        uint8_t keyBytes[16];
        for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)(rand() & 0xFF);
        std::string wsKey = base64Encode(keyBytes, 16);
        std::string path = "/sessions/" + m_sessionId + "/events";

        std::string request =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wsKey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        send(sock, request.c_str(), (int)request.size(), 0);

        std::string response;
        char buf[4096];
        while (m_running.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            struct timeval tv = {3, 0};
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
            cueLog("CueClient: websocket upgrade failed");
            closesocket(sock);
            sleepBackoff(m_running, backoff);
            backoff = std::min(backoff * 2, 30);
            continue;
        }

        WSReader reader;
        reader.sock = sock;
        reader.running = &m_running;
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos && headerEnd + 4 < response.size()) {
            reader.buf = response.substr(headerEnd + 4);
        }

        m_connected.store(true);
        cueLog("CueClient: connected");

        while (m_running.load()) {
            WSFrame frame;
            if (!wsReadFrame(reader, frame)) {
                if (m_running.load()) cueLog("CueClient: read failed");
                break;
            }

            if (frame.opcode == 0x8) break;
            if (frame.opcode == 0x9) {
                wsSendFrame(sock, 0xA, frame.payload);
                continue;
            }
            if (frame.opcode != 0x1) continue;
            handleMessage(frame.payload);
        }

        m_connected.store(false);
        closesocket(sock);
        if (m_running.load()) {
            sleepBackoff(m_running, backoff);
            backoff = std::min(backoff * 2, 30);
        }
    }

    WSACleanup();
}

void CueClient::handleMessage(const std::string& payload) {
    auto message = nlohmann::json::parse(payload, nullptr, false);
    if (!message.is_object()) return;

    std::string type = fieldString(message, "type");
    if (type == "ready") {
        std::string session = fieldString(message, "sessionId");
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (!session.empty()) {
            m_sessionId = session;
            m_metadata["cue.session"] = session;
        }
        return;
    }

    if (type == "state.snapshot") {
        const auto& state = message["state"];
        if (!state.is_object()) return;
        std::lock_guard<std::mutex> lk(m_dataMutex);
        std::string transcript = fieldString(state, "transcript");
        if (!transcript.empty()) m_fullTranscript = transcript;
        std::string vision = fieldString(state, "latestVisionDescription");
        if (!vision.empty()) {
            m_latestVisionDescription = vision;
            m_metadata["cue.vision.description"] = vision;
        }
        m_metadata["cue.observation_count"] = fieldString(state, "observationCount");
        m_metadata["cue.decision_count"] = fieldString(state, "decisionCount");
        const auto& signals = state["signals"];
        if (signals.is_object()) {
            for (auto it = signals.begin(); it != signals.end(); ++it) {
                m_metadata["cue.signal." + it.key()] = jsonString(it.value());
            }
        }
        return;
    }

    if (type == "transcript") {
        std::string text = fieldString(message, "text");
        bool isFinal = fieldBool(message, "isFinal", true);
        std::string full = fieldString(message, "fullTranscript");
        {
            std::lock_guard<std::mutex> lk(m_dataMutex);
            m_latestWords = text;
            if (!full.empty()) {
                m_fullTranscript = full;
            } else if (isFinal && !text.empty()) {
                if (!m_fullTranscript.empty()) m_fullTranscript += " ";
                m_fullTranscript += text;
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_eventMutex);
            m_pendingTranscriptEvents.push_back({text, isFinal});
        }
        return;
    }

    if (type == "prompt") {
        std::string prompt = fieldString(message, "prompt");
        bool reset = fieldBool(message, "reset", false);
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (!prompt.empty()) m_prompt = prompt;
        m_promptReset.store(reset);
        m_metadata["cue.prompt.reset"] = reset ? "true" : "false";
        std::string actionType = fieldString(message, "actionType");
        if (!actionType.empty()) m_metadata["cue.prompt.action_type"] = actionType;
        return;
    }

    if (type == "source.available") {
        SourceOutput source;
        source.id = fieldString(message, "sourceId");
        source.kind = fieldString(message, "kind");
        source.label = fieldString(message, "label");
        source.provider = fieldString(message, "provider");
        source.transport = fieldString(message, "transport");
        source.url = fieldString(message, "url");
        if (source.id.empty()) source.id = source.url;
        if (source.label.empty()) source.label = source.id;
        if (source.kind.empty()) source.kind = "video";
        if (source.transport.empty()) source.transport = "whep";
        if (source.id.empty() || source.url.empty()) return;

        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_sources[source.id] = source;
        m_metadata["cue.source." + source.id + ".url"] = source.url;
        m_metadata["cue.source." + source.id + ".transport"] = source.transport;
        m_metadata["cue.source." + source.id + ".provider"] = source.provider;
        return;
    }

    if (type == "output.status") {
        std::string outputId = fieldString(message, "outputId");
        std::string status = fieldString(message, "status");
        std::string detail = fieldString(message, "detail");
        if (outputId.empty()) return;
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (!status.empty()) m_metadata["cue.output." + outputId + ".status"] = status;
        if (!detail.empty()) m_metadata["cue.output." + outputId + ".detail"] = detail;
        return;
    }

    if (type == "vision.description") {
        std::string text = fieldString(message, "text");
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (!text.empty()) {
            m_latestVisionDescription = text;
            m_metadata["cue.vision.description"] = text;
        }
        return;
    }

    if (type == "signal") {
        std::string name = fieldString(message, "name");
        if (name.empty()) return;
        std::string value = jsonString(message["value"]);
        std::lock_guard<std::mutex> lk(m_dataMutex);
        m_metadata["cue.signal." + name] = value;
        return;
    }
}
