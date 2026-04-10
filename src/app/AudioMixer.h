#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#ifdef HAS_NDI
#include "sources/NDIRuntime.h"
#endif

#ifdef _WIN32
// Forward declare WASAPI types to avoid Windows.h in header
struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioRenderClient;
struct IMMDevice;
#endif
struct SwrContext;

// --- Per-input WASAPI capture + resampler ---
struct AudioMixerInput {
    // Configuration (set from main thread before init, or via pending changes)
    std::string deviceId;       // WASAPI endpoint ID (empty = default)
    std::string name;           // Display name
    bool isCapture = false;     // true = mic, false = render endpoint (loopback)
    float volume = 1.0f;        // 0.0-1.0 gain
    bool muted = false;

#ifdef _WIN32
    // WASAPI state (owned by mixer thread)
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    IMMDevice* device = nullptr;
#elif defined(__APPLE__)
    void* macAudioImpl = nullptr;
#endif
    int sampleRate = 0;
    int channels = 0;
    bool initialized = false;

    // Resampler (to output sample rate/channels) — only when rates differ
    SwrContext* swrCtx = nullptr;

    // Per-input ring buffer (resampled to output rate, stereo float32 interleaved)
    // Single-thread access (mixer thread only) — no atomics needed
    std::vector<float> ringBuf;
    size_t writePos = 0;
    size_t readPos = 0;
    size_t ringSize = 0; // in samples (frames * channels)

    bool init(int outputSampleRate, int outputChannels);
    void cleanup();
    void drain(int outputSampleRate, int outputChannels);

    size_t available() const; // samples available to read
};

// --- WASAPI render output ---
struct AudioMixerOutput {
    std::string deviceId;       // empty = system default
    std::string name;

#ifdef _WIN32
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    IMMDevice* device = nullptr;
#elif defined(__APPLE__)
    void* macAudioImpl = nullptr;
#endif
    int sampleRate = 0;
    int channels = 0;
    int bufferFrames = 0;
    bool initialized = false;

    bool init();
    void cleanup();
    void feed(const float* mixedData, size_t frames);
};

// --- Main mixer coordinator ---
class AudioMixer {
public:
    AudioMixer() = default;
    ~AudioMixer();

    // Input management (main thread — applied on next mixer tick)
    int  addInput(const std::string& deviceId, const std::string& name, bool isCapture);
    void removeInput(int index);
    int  inputCount() const;

    // Per-input controls
    void setInputVolume(int index, float vol);
    float inputVolume(int index) const;
    void setInputMuted(int index, bool muted);
    bool isInputMuted(int index) const;
    std::string inputName(int index) const;

    // Output device selection (main thread)
    void setOutputDevice(const std::string& deviceId, const std::string& name);
    std::string outputDeviceId() const;
    std::string outputDeviceName() const;

    // Master volume
    void setMasterVolume(float vol);
    float masterVolume() const;

    // NDI audio output (sends mixed audio as an NDI source)
    void setNDIAudioEnabled(bool enabled);
    bool isNDIAudioEnabled() const { return m_ndiAudioEnabled.load(); }

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    // Main thread: drain mixed mono samples for AudioAnalyzer::feedSamples
    // Returns number of mono samples copied into buffer
    int drainMixedMono(float* buffer, int maxSamples);

    int outputSampleRate() const { return m_mixSampleRate; }
    bool hasLocalOutput() const;

private:
    // Thread
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    // Inputs and output (mutex protects config changes)
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<AudioMixerInput>> m_inputs;
    AudioMixerOutput m_output;
    std::atomic<float> m_masterVolume{1.0f};

    // Pending config changes (main thread writes, mixer thread applies)
    struct PendingInput {
        std::string deviceId;
        std::string name;
        bool isCapture;
    };
    std::vector<PendingInput> m_pendingAdds;
    std::vector<int> m_pendingRemoves;
    std::string m_pendingOutputDeviceId;
    std::string m_pendingOutputDeviceName;
    bool m_outputChanged = false;
    int m_nextInputId = 0; // monotonic ID for addInput return value

    // Effective mix format (from output device, or defaults when no output)
    int m_mixSampleRate = 48000;
    int m_mixChannels = 2;

    // Time-based frame counting (used when no local output device)
    std::chrono::steady_clock::time_point m_lastMixTime;

    // Scratch buffer for mixing (one tick's worth, stereo)
    std::vector<float> m_mixBuf;

    // SPSC ring buffer: mixer thread writes mono, main thread reads
    static constexpr size_t kAnalyzerRingSize = 48000; // ~1 second at 48kHz
    float m_analyzerRing[kAnalyzerRingSize] = {};
    std::atomic<size_t> m_analyzerWritePos{0};
    std::atomic<size_t> m_analyzerReadPos{0};

    // NDI audio output
    std::atomic<bool> m_ndiAudioEnabled{false};
#ifdef HAS_NDI
    NDIlib_send_instance_t m_ndiSend = nullptr;
    std::vector<float> m_ndiPlanarBuf;
#endif

    void mixerThread();
    void applyPendingChanges();
    void mixAndOutput();
};
