#include "app/OSCManager.h"
#include <cstring>
#include <iostream>
#include <algorithm>

bool OSCManager::s_wsaInit = false;

void OSCManager::initWSA() {
#ifdef _WIN32
    if (!s_wsaInit) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        s_wsaInit = true;
    }
#endif
}

OSCManager::~OSCManager() {
    stopReceiver();
    if (m_sendSocket != INVALID_SOCKET) closesocket(m_sendSocket);
}

bool OSCManager::startReceiver(int port) {
    if (m_receiving) stopReceiver();
    initWSA();

    m_recvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_recvSocket == INVALID_SOCKET) {
        std::cerr << "[OSC] Failed to create receive socket" << std::endl;
        return false;
    }

    // Allow reuse
    int reuse = 1;
    setsockopt(m_recvSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_recvSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[OSC] Failed to bind port " << port << std::endl;
        closesocket(m_recvSocket);
        m_recvSocket = INVALID_SOCKET;
        return false;
    }

    m_receivePort = port;
    m_receiving = true;
    m_recvThread = std::thread(&OSCManager::receiveLoop, this);
    std::cout << "[OSC] Listening on port " << port << std::endl;
    return true;
}

void OSCManager::stopReceiver() {
    m_receiving = false;
    // Join thread first (it will exit on next select timeout),
    // then close socket to avoid race condition
    if (m_recvThread.joinable()) m_recvThread.join();
    if (m_recvSocket != INVALID_SOCKET) {
        closesocket(m_recvSocket);
        m_recvSocket = INVALID_SOCKET;
    }
}

void OSCManager::receiveLoop() {
    uint8_t buf[4096];
    while (m_receiving) {
        // Set timeout so we can check m_receiving
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_recvSocket, &fds);

        int sel = select((int)m_recvSocket + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        int len = recvfrom(m_recvSocket, (char*)buf, sizeof(buf), 0, nullptr, nullptr);
        if (len <= 0) continue;

        OSCMessage msg;
        if (parseOSC(buf, len, msg)) {
            std::lock_guard<std::mutex> lock(m_msgMutex);
            m_pendingMessages.push_back(std::move(msg));
            // Cap queue
            if (m_pendingMessages.size() > 1000) {
                m_pendingMessages.erase(m_pendingMessages.begin(),
                                         m_pendingMessages.begin() + 500);
            }
        }
    }
}

// OSC message format:
// - Address string (null-terminated, padded to 4-byte boundary)
// - Type tag string starting with ',' (null-terminated, padded to 4-byte boundary)
// - Arguments (each 4-byte aligned)
bool OSCManager::parseOSC(const uint8_t* data, int len, OSCMessage& msg) {
    if (len < 4 || data[0] != '/') return false;

    // Read address (bounded by buffer length)
    int pos = 0;
    int addrEnd = 0;
    while (addrEnd < len && data[addrEnd] != '\0') addrEnd++;
    if (addrEnd >= len) return false; // no null terminator
    msg.address = std::string((const char*)data, addrEnd);
    pos = addrEnd + 1;
    pos = (pos + 3) & ~3; // align to 4

    if (pos >= len) return true; // address-only message

    // Read type tag (bounded)
    if (data[pos] != ',') return true;
    int tagEnd = pos;
    while (tagEnd < len && data[tagEnd] != '\0') tagEnd++;
    if (tagEnd >= len) return true;
    std::string typeTags = std::string((const char*)(data + pos), tagEnd - pos);
    pos = tagEnd + 1;
    pos = (pos + 3) & ~3;

    // Parse arguments
    for (int t = 1; t < (int)typeTags.size(); t++) {
        if (pos + 4 > len) break;
        switch (typeTags[t]) {
            case 'f': {
                uint32_t raw;
                memcpy(&raw, data + pos, 4);
                raw = ntohl(raw);
                float f;
                memcpy(&f, &raw, 4);
                msg.floats.push_back(f);
                pos += 4;
                break;
            }
            case 'i': {
                uint32_t raw;
                memcpy(&raw, data + pos, 4);
                int32_t i = (int32_t)ntohl(raw);
                msg.ints.push_back(i);
                pos += 4;
                break;
            }
            case 's': {
                int sEnd = pos;
                while (sEnd < len && data[sEnd] != '\0') sEnd++;
                if (sEnd >= len) return true;
                std::string s((const char*)(data + pos), sEnd - pos);
                msg.strings.push_back(s);
                pos = sEnd + 1;
                pos = (pos + 3) & ~3;
                break;
            }
            default:
                return true; // unknown type, stop parsing
        }
    }
    return true;
}

std::vector<OSCMessage> OSCManager::pollMessages() {
    std::lock_guard<std::mutex> lock(m_msgMutex);
    std::vector<OSCMessage> result = std::move(m_pendingMessages);
    m_pendingMessages.clear();
    return result;
}

// --- Sender ---

void OSCManager::setSendTarget(const std::string& host, int port) {
    m_sendHost = host;
    m_sendPort = port;
    m_sendReady = false; // force re-init
}

void OSCManager::ensureSendSocket() {
    if (m_sendReady) return;
    initWSA();
    if (m_sendSocket == INVALID_SOCKET) {
        m_sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    memset(&m_sendAddr, 0, sizeof(m_sendAddr));
    m_sendAddr.sin_family = AF_INET;
    m_sendAddr.sin_port = htons((uint16_t)m_sendPort);
    inet_pton(AF_INET, m_sendHost.c_str(), &m_sendAddr.sin_addr);
    m_sendReady = true;
}

std::vector<uint8_t> OSCManager::buildOSCMessage(const std::string& address, char typeTag,
                                                   const void* value, int valueSize) {
    std::vector<uint8_t> buf;

    // Address (padded to 4-byte)
    buf.insert(buf.end(), address.begin(), address.end());
    buf.push_back(0);
    while (buf.size() % 4 != 0) buf.push_back(0);

    // Type tag
    buf.push_back(',');
    buf.push_back((uint8_t)typeTag);
    buf.push_back(0);
    while (buf.size() % 4 != 0) buf.push_back(0);

    // Value (network byte order)
    if (valueSize == 4) {
        uint32_t raw;
        memcpy(&raw, value, 4);
        raw = htonl(raw);
        uint8_t bytes[4];
        memcpy(bytes, &raw, 4);
        buf.insert(buf.end(), bytes, bytes + 4);
    }

    return buf;
}

void OSCManager::sendFloat(const std::string& address, float value) {
    ensureSendSocket();
    auto buf = buildOSCMessage(address, 'f', &value, 4);
    sendto(m_sendSocket, (const char*)buf.data(), (int)buf.size(), 0,
           (struct sockaddr*)&m_sendAddr, sizeof(m_sendAddr));
}

void OSCManager::sendInt(const std::string& address, int32_t value) {
    ensureSendSocket();
    auto buf = buildOSCMessage(address, 'i', &value, 4);
    sendto(m_sendSocket, (const char*)buf.data(), (int)buf.size(), 0,
           (struct sockaddr*)&m_sendAddr, sizeof(m_sendAddr));
}

void OSCManager::sendString(const std::string& address, const std::string& value) {
    ensureSendSocket();
    // String OSC message
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), address.begin(), address.end());
    buf.push_back(0);
    while (buf.size() % 4 != 0) buf.push_back(0);
    buf.push_back(','); buf.push_back('s'); buf.push_back(0);
    while (buf.size() % 4 != 0) buf.push_back(0);
    buf.insert(buf.end(), value.begin(), value.end());
    buf.push_back(0);
    while (buf.size() % 4 != 0) buf.push_back(0);

    sendto(m_sendSocket, (const char*)buf.data(), (int)buf.size(), 0,
           (struct sockaddr*)&m_sendAddr, sizeof(m_sendAddr));
}
