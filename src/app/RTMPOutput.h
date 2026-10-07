#pragma once
#ifdef HAS_FFMPEG

#include <glad/glad.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include "app/AudioCaptureClock.h"

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
    bool isActive() const { return m_active && !m_stopRequested; }
    int packetsWritten() const { return m_packetsWritten; }
    int videoPacketsWritten() const { return m_videoPacketsWritten; }
    double videoTimeSeconds() const { return m_videoTime; }
    double audioTimeSeconds() const { return m_audioTime; }
    int outputWidth() const { return m_encWidth; }
    int outputHeight() const { return m_encHeight; }
    const std::string& videoEncoder() const { return m_videoEncoder; }
    int audioCaptureGaps() const { return m_audioCaptureGaps; }
    int64_t audioSilenceFrames() const { return m_audioSilenceFrames; }

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
    void captureAudioThread();
    void encodeAudioThread();
    void encodeAudioSamples(const float* data, int numSamples, int channels);
    void cleanup();
    void cleanupAudio();
    int writePacket(AVPacket* packet = nullptr);
    static int interruptIO(void* opaque);

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<int> m_droppedFrames{0};
    std::atomic<int> m_packetsWritten{0};
    std::atomic<int> m_videoPacketsWritten{0};
    std::atomic<int64_t> m_ioDeadline{0};
    std::atomic<double> m_videoTime{0};
    std::atomic<double> m_audioTime{0};
    std::atomic<int> m_audioCaptureGaps{0};
    std::atomic<int64_t> m_audioSilenceFrames{0};
    std::string m_videoEncoder;
    int m_fps = 30;
    double m_nextReadback = 0;

    // Video - FFmpeg
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_videoCodecCtx = nullptr;
    AVStream* m_videoStream = nullptr;
    AVFrame* m_videoFrame = nullptr;
    AVPacket* m_packet = nullptr;
    AVPacket* m_audioPacket = nullptr;
    std::mutex m_muxMutex;
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
    AudioCaptureClock m_audioClock;
    struct CapturedAudio {
        std::vector<float> samples;
        uint64_t position = 0;
        int64_t wallSample = 0;
        uint32_t frames = 0;
        bool timestampValid = false;
    };
    std::deque<CapturedAudio> m_audioQueue;
    size_t m_queuedAudioFrames = 0;
    int64_t m_idleAudioTarget = 0; // capture thread confirms idle before filling
    std::mutex m_audioMutex;
    std::thread m_audioThread;
    std::thread m_audioEncodeThread;

    // PBO async readback (double-buffered)
    GLuint m_pbo[2] = {0, 0};
    int m_pboIndex = 0;
    bool m_pboReady = false;

    // Video frame handoff
    std::vector<uint8_t> m_readbackBuf;
    std::vector<uint8_t> m_encodeBuf;
    bool m_frameReady = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
};

#endif // HAS_FFMPEG
