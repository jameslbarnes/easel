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

class RTMPOutput {
public:
    ~RTMPOutput();

    // aspectNum/aspectDen: target aspect ratio (e.g. 16,9). 0,0 = use source as-is.
    bool start(const std::string& streamKey, int width, int height,
               int aspectNum = 16, int aspectDen = 9, int fps = 30);
    bool startCustom(const std::string& url, int width, int height,
                     int aspectNum = 16, int aspectDen = 9, int fps = 30);
    void stop();
    bool isActive() const { return m_active; }

    void sendFrame(GLuint texture, int w, int h);

    int droppedFrames() const { return m_droppedFrames; }
    double uptimeSeconds() const;

private:
    bool initEncoder(const std::string& url, int width, int height,
                     int aspectNum, int aspectDen, int fps);
    bool initAudioCapture();
    void encodeThread();
    void encodeVideoFrame(const uint8_t* rgbaData, int w, int h);
    void drainAudio();
    void encodeAudioSamples(const float* data, int numSamples, int channels);
    void cleanup();
    void cleanupAudio();

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<int> m_droppedFrames{0};

    // Video - FFmpeg
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_videoCodecCtx = nullptr;
    AVStream* m_videoStream = nullptr;
    AVFrame* m_videoFrame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int64_t m_videoFrameIndex = 0;
    int m_width = 0, m_height = 0;       // input (FBO) dimensions
    int m_encWidth = 0, m_encHeight = 0; // output (encode) dimensions
    int m_cropX = 0, m_cropY = 0;       // crop offset within FBO
    int m_cropW = 0, m_cropH = 0;       // cropped region size
    double m_startTime = 0;

    // Audio - FFmpeg
    AVCodecContext* m_audioCodecCtx = nullptr;
    AVStream* m_audioStream = nullptr;
    AVFrame* m_audioFrame = nullptr;
    SwrContext* m_swrCtx = nullptr;
    int64_t m_audioSamplesWritten = 0;
    int m_audioFrameSize = 0;
    std::vector<float> m_audioAccum; // accumulate samples until we have a full frame

    // Audio - WASAPI (opaque pointers to avoid header pollution)
    void* m_audioClient = nullptr;      // IAudioClient*
    void* m_captureClient = nullptr;    // IAudioCaptureClient*
    void* m_audioDevice = nullptr;      // IMMDevice*
    int m_wasapiSampleRate = 0;
    int m_wasapiChannels = 0;

    // Video frame handoff
    std::vector<uint8_t> m_readbackBuf;
    std::vector<uint8_t> m_encodeBuf;
    bool m_frameReady = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
};

#endif // HAS_FFMPEG
