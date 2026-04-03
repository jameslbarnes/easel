#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close
#endif

struct OSCMessage {
    std::string address;
    std::vector<float> floats;
    std::vector<int32_t> ints;
    std::vector<std::string> strings;
};

class OSCManager {
public:
    ~OSCManager();

    // Start listening on a port
    bool startReceiver(int port = 9000);
    void stopReceiver();

    // Set target for sending
    void setSendTarget(const std::string& host, int port);

    // Send messages
    void sendFloat(const std::string& address, float value);
    void sendInt(const std::string& address, int32_t value);
    void sendString(const std::string& address, const std::string& value);

    // Poll received messages (call from main thread)
    std::vector<OSCMessage> pollMessages();

    bool isReceiving() const { return m_receiving; }
    int receivePort() const { return m_receivePort; }
    std::string sendHost() const { return m_sendHost; }
    int sendPort() const { return m_sendPort; }

private:
    // Receiver
    SOCKET m_recvSocket = INVALID_SOCKET;
    std::thread m_recvThread;
    std::atomic<bool> m_receiving{false};
    int m_receivePort = 9000;

    std::mutex m_msgMutex;
    std::vector<OSCMessage> m_pendingMessages;

    void receiveLoop();
    bool parseOSC(const uint8_t* data, int len, OSCMessage& msg);

    // Sender
    SOCKET m_sendSocket = INVALID_SOCKET;
    std::string m_sendHost = "127.0.0.1";
    int m_sendPort = 9001;
    struct sockaddr_in m_sendAddr{};
    bool m_sendReady = false;

    void ensureSendSocket();
    std::vector<uint8_t> buildOSCMessage(const std::string& address, char typeTag, const void* value, int valueSize);

    static bool s_wsaInit;
    static void initWSA();
};
