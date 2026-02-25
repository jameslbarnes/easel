#pragma once
#include "sources/ContentSource.h"
#include "render/Texture.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

// Forward declarations for FFmpeg
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVPacket;

class VideoSource : public ContentSource {
public:
    ~VideoSource();

    bool load(const std::string& path);
    void close();

    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    std::string typeName() const override { return "Video"; }
    std::string sourcePath() const override { return m_path; }

    bool isVideo() const override { return true; }
    void play() override;
    void pause() override;
    void seek(double seconds) override;
    bool isPlaying() const override { return m_playing; }
    double duration() const override { return m_duration; }
    double currentTime() const override { return m_currentTime; }

private:
    std::string m_path;
    int m_width = 0, m_height = 0;
    double m_duration = 0.0;
    double m_currentTime = 0.0;
    double m_timeBase = 0.0;

    // FFmpeg state
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int m_videoStreamIndex = -1;

    // Triple buffer for decoded frames
    struct FrameBuffer {
        std::vector<uint8_t> data;
        double pts = 0.0;
        std::atomic<bool> ready{false};
    };
    FrameBuffer m_buffers[3];
    std::atomic<int> m_writeIndex{0};
    std::atomic<int> m_readIndex{0};
    std::mutex m_frameMutex;

    // Decode thread
    std::thread m_decodeThread;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_seekRequested{false};
    std::atomic<double> m_seekTarget{0.0};

    Texture m_texture;
    bool m_hasNewFrame = false;
    double m_playbackStart = 0.0;
    double m_playbackOffset = 0.0;

    void decodeLoop();
    bool decodeFrame(AVFrame* frame, AVPacket* pkt);
};
