#pragma once
#ifdef HAS_FFMPEG

#include <glad/glad.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

struct RecAudioDevice {
    std::string name;
    std::string id;       // WASAPI endpoint ID
    bool isCapture;       // true = microphone, false = output (loopback)
};

class VideoRecorder {
public:
    ~VideoRecorder();

    static std::vector<RecAudioDevice> enumerateAudioDevices();

    void setAudioDevice(int index) { m_selectedAudioDevice = index; }
    int audioDevice() const { return m_selectedAudioDevice; }

    bool start(const std::string& path, int width, int height, int fps = 30);
    void stop();
    bool isActive() const { return m_active; }

    void sendFrame(GLuint texture, int w, int h);

    double uptimeSeconds() const;
    const std::string& filePath() const { return m_path; }

private:
    bool initEncoder(const std::string& path, int width, int height, int fps);
    bool initAudioCapture();
    void encodeThread();
    void encodeVideoFrame(const uint8_t* rgbaData);
    void drainAudio();
    void encodeAudioSamples(const float* data, int numSamples, int channels);
    void cleanup();
    void cleanupAudio();

    std::string m_path;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stopRequested{false};
    int m_selectedAudioDevice = -1; // -1 = default output loopback

    // Video
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_videoCodecCtx = nullptr;
    AVStream* m_videoStream = nullptr;
    AVFrame* m_videoFrame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int64_t m_videoFrameIndex = 0;
    int m_width = 0, m_height = 0;
    double m_startTime = 0;

    // Audio
    AVCodecContext* m_audioCodecCtx = nullptr;
    AVStream* m_audioStream = nullptr;
    AVFrame* m_audioFrame = nullptr;
    SwrContext* m_swrCtx = nullptr;
    int64_t m_audioSamplesWritten = 0;
    int m_audioFrameSize = 0;
    std::vector<float> m_audioAccum;

    void* m_audioClient = nullptr;
    void* m_captureClient = nullptr;
    void* m_audioDevice = nullptr;
    int m_wasapiSampleRate = 0;
    int m_wasapiChannels = 0;

    // Frame handoff
    std::vector<uint8_t> m_readbackBuf;
    std::vector<uint8_t> m_encodeBuf;
    bool m_frameReady = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
};

#endif
