#pragma once
#ifdef HAS_FFMPEG

#include <glad/glad.h>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>

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

    // fps is only a hint now: it feeds gop_size + the framerate metadata. Actual
    // frame timing is wall-clock VFR, so recording tracks the real render rate.
    bool start(const std::string& path, int width, int height, int fps = 60);
    void stop();
    bool isActive() const { return m_active; }

    void sendFrame(GLuint texture, int w, int h);

    double uptimeSeconds() const;
    const std::string& filePath() const { return m_path; }

private:
    // One captured frame handed from the GL thread to the encode thread. Carries
    // its own size (so a mid-recording zone resize is rescaled, not dropped) and
    // its wall-clock capture time (so PTS reflects real elapsed time → correct speed).
    struct RecFrame {
        std::vector<uint8_t> pixels;
        int w = 0;
        int h = 0;
        int64_t captureUs = 0;
    };

    // One WASAPI capture lane (system loopback or microphone). Each lane owns its
    // device session and a resampler to the fixed mix format; lanes are mixed
    // sample-aligned into the single AAC track. A lane whose device dies (endpoint
    // restart, driver reset) pads silence to keep the timeline in sync and is
    // periodically re-opened, so audio resumes instead of ending the track.
    struct AudioSource {
        std::string id;          // WASAPI endpoint ID, empty = default endpoint
        std::string name;        // display name (for logs)
        std::string label;       // "system" | "mic"
        bool isCapture = false;  // true = microphone, false = render loopback
        float gain = 1.0f;
        void* device = nullptr;         // IMMDevice*
        void* client = nullptr;         // IAudioClient*
        void* capture = nullptr;        // IAudioCaptureClient*
        // MME (waveIn) alternative path — this box's Focusrite WASAPI capture
        // endpoint delivers nothing in shared mode (the etherea capture helper
        // works around the same defect), so capture lanes can run on MME.
        bool useMme = false;
        int apiForce = 0;               // 0 = auto (WASAPI, fall back to MME), 1 = wasapi only, 2 = mme only
        void* mme = nullptr;            // MmeCapture*
        int64_t lastDataUs = 0;         // last cycle that delivered packets
        int64_t openedUs = 0;
        int rate = 0, ch = 0;           // device mix format
        SwrContext* swr = nullptr;      // device format -> mix format
        std::vector<float> fifo;        // resampled interleaved stereo @ mix rate
        int64_t produced = 0;           // mix-rate frames appended since anchor
        int64_t lastReinitUs = 0;
        int64_t lastFailLogUs = 0;
        // ok is written by async init workers and read by the encode thread.
        // initState (0 idle / 1 in-flight) guards against overlapping workers;
        // a worker hung inside a wedged driver call keeps it at 1 forever, which
        // parks the lane on silence-fill instead of hanging the encode thread.
        std::atomic<bool> ok{false};
        std::atomic<int> initState{0};
        bool sawSignal = false;
        std::vector<float> extractBuf;  // scratch for >2ch -> stereo extraction
        // periodic stats (reset each dump)
        int64_t statGot = 0;            // cycles with packet data
        int64_t statAppended = 0;       // frames appended
        int statSwrErrLogs = 0;
    };

    bool initEncoder(const std::string& path, int width, int height, int fps);
    bool initAudioCapture();
    bool initAudioSource(AudioSource& s);
    bool initAudioSourceMme(AudioSource& s);
    void teardownAudioSource(AudioSource& s);
    bool readAudioSource(AudioSource& s);
    bool readAudioSourceMme(AudioSource& s);
    void appendResampled(AudioSource& s, const float* data, int frames);
    void encodeThread();
    void encodeVideoFrame(const RecFrame& f);
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
    int m_width = 0, m_height = 0;          // fixed encoder output size (pinned at start)
    int m_srcW = 0, m_srcH = 0;             // current sws input size (differs after a zone/canvas resize)
    double m_startTime = 0;
    int64_t m_startPtsUs = 0;               // wall-clock anchor (av_gettime_relative) of the first encoded frame
    int64_t m_lastVideoPts = -1;            // strictly-monotonic guard for VFR pts (in codec time_base ticks)

    // Audio
    AVCodecContext* m_audioCodecCtx = nullptr;
    AVStream* m_audioStream = nullptr;
    AVFrame* m_audioFrame = nullptr;
    SwrContext* m_swrCtx = nullptr;
    int64_t m_audioSamplesWritten = 0;
    int64_t m_audioStartUs = 0;     // wall-clock anchor for loopback silence-fill
    int m_audioFrameSize = 0;
    std::vector<float> m_audioAccum;
    int m_audioSendErrLogs = 0;     // one-shot diagnostics for the encode/mux path
    int m_audioWriteErrLogs = 0;
    int m_audioPktLogs = 0;
    bool m_loggedFirstMix = false;
    int64_t m_lastStatsUs = 0;      // periodic per-lane stats dump
    int64_t m_statMixed = 0;
    int64_t m_hbUs = 0;             // drainAudio heartbeat (pre-guard)
    // Feed-stall detector: video muxing observes whether the audio side is
    // still delivering samples, so an audio-only death is loud in the log.
    int64_t m_lastAudioMuxUs = 0;      // wall clock when encoded-sample count last advanced
    int64_t m_lastAudioMuxSamples = 0; // encoded-sample count at that moment
    int64_t m_stallLogUs = 0;          // feed-stalled warning throttle

    // unique_ptr: lanes hold atomics (non-movable) and async init workers hold
    // references across cycles, so lane addresses must be stable.
    std::vector<std::unique_ptr<AudioSource>> m_audioSources;
    std::vector<float> m_mixScratch;
    int m_wasapiSampleRate = 0;   // fixed mix rate once audio is initialized
    int m_wasapiChannels = 0;

    // PBO async readback (double-buffered)
    GLuint m_pbo[2] = {0, 0};
    int m_pboIndex = 0;
    bool m_pboReady = false;
    int m_prevW = 0, m_prevH = 0;           // size of the frame whose readback was last issued
    size_t m_pboBytes = 0;                  // current PBO byte size (detects a zone/canvas resize)

    // Frame handoff — bounded queue of timestamped RGBA frames (true VFR), with a
    // small pool of recycled pixel buffers to avoid per-frame multi-MB allocations.
    std::deque<RecFrame> m_frameQueue;
    std::vector<std::vector<uint8_t>> m_bufPool;
    size_t m_maxQueue = 8;                  // cap; drop the OLDEST frame under sustained overload
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
};

#endif
