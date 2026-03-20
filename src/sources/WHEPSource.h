#pragma once
#ifdef HAS_WHEP

#include "sources/ContentSource.h"
#include "render/Texture.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

// Shared HTTP/HTTPS utility (WinHTTP-based)
std::string winHttpRequest(const std::string& method, const std::string& url,
                           const std::string& body, const std::string& contentType,
                           std::string* outLocationHeader = nullptr);
#include <memory>

// Forward declarations
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
namespace rtc { class PeerConnection; class Track; }

class WHEPSource : public ContentSource {
public:
    ~WHEPSource();

    // Connect to a WHEP endpoint (e.g. http://localhost:7860/api/scope/mediamtx/whep/longlive)
    bool connect(const std::string& whepUrl);
    void disconnect();
    bool isConnected() const { return m_connected.load(); }

    // Auto-discover WHEP URL from Etherea backend
    static std::string discoverUrl(const std::string& baseUrl = "http://localhost:7860");

    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    std::string typeName() const override { return "WHEP"; }
    std::string sourcePath() const override { return m_whepUrl; }
    bool isVideo() const override { return true; }
    bool isPlaying() const override { return m_connected.load(); }

private:
    std::string m_whepUrl;
    std::string m_teardownUrl;  // WHEP teardown (Location header)
    int m_width = 0;
    int m_height = 0;

    // WebRTC
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::Track> m_track;

    // FFmpeg decode
    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;

    // NAL unit accumulator (fed from RTP depacketizer)
    std::mutex m_nalMutex;
    std::vector<std::vector<uint8_t>> m_pendingNals;

    // Decode thread
    std::thread m_decodeThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    void decodeLoop();

    // Triple buffer (decode thread writes, main thread reads)
    struct FrameBuffer {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        std::atomic<bool> ready{false};
    };
    FrameBuffer m_buffers[3];
    std::atomic<int> m_writeIndex{0};
    std::atomic<int> m_readIndex{0};

    Texture m_texture;

    // WHEP HTTP signaling
    std::string whepPost(const std::string& url, const std::string& sdpOffer);

    bool initDecoder();
    void cleanupDecoder();
    bool decodeNalUnit(const uint8_t* data, int size, AVFrame* frame);
};

#endif // HAS_WHEP
