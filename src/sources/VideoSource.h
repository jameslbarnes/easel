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
struct SwrContext;
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

    float volume() const { return m_volume; }
    void setVolume(float v) { m_volume = v; }
    bool hasAudio() const { return m_audioStreamIndex >= 0; }

private:
    std::string m_path;
    int m_width = 0, m_height = 0;
    double m_duration = 0.0;
    double m_currentTime = 0.0;
    double m_timeBase = 0.0;

    // FFmpeg video state
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int m_videoStreamIndex = -1;

    // FFmpeg audio state
    AVCodecContext* m_audioCodecCtx = nullptr;
    SwrContext* m_swrCtx = nullptr;
    int m_audioStreamIndex = -1;
    double m_audioTimeBase = 0.0;

    // WASAPI audio output (void* to avoid header pollution)
    void* m_audioClient = nullptr;
    void* m_renderClient = nullptr;
    void* m_audioDevice = nullptr;
    int m_audioOutSampleRate = 0;
    int m_audioOutChannels = 0;
    int m_audioBufferFrames = 0;

    // Audio ring buffer (float interleaved stereo)
    std::vector<float> m_audioRing;
    std::atomic<size_t> m_audioWritePos{0};
    std::atomic<size_t> m_audioReadPos{0};
    std::mutex m_audioMutex;
    float m_volume = 1.0f;

    // Audio-driven sync: count frames consumed by WASAPI
    std::atomic<uint64_t> m_audioFramesPlayed{0};

    bool initAudioOutput();
    void cleanupAudio();
    void feedAudioToWASAPI();
    void decodeAudioPacket(AVFrame* frame, AVPacket* pkt);

    // Triple buffer for decoded video frames
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
