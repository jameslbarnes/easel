#pragma once
#include "render/Texture.h"
#include <vector>
#include <string>
#include <cmath>

#ifdef _WIN32
// Forward declare WASAPI types to avoid Windows.h in header
struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
#endif

struct AudioBands {
    float bass = 0;
    float lowMid = 0;
    float highMid = 0;
    float treble = 0;
};

class AudioAnalyzer {
public:
    static constexpr int kFFTSize = 512;
    static constexpr int kBins = kFFTSize / 2;

    AudioAnalyzer() = default;
    ~AudioAnalyzer();

    // Call once per frame with delta time
    void update(float dt);

    // Change audio device (-1 = system loopback, >=0 = device index)
    void setDevice(int deviceIdx);
    void setDeviceId(const std::string& id, bool isCapture);

    // Opt-in gate for macOS ScreenCaptureKit system-audio capture. macOS
    // triggers a "Screen Recording" TCC prompt the first time SCShareableContent
    // is called; for self-signed dev builds the grant is keyed to the binary's
    // cdhash and evaporates on every rebuild, so the prompt fires repeatedly.
    // Gating the call behind an explicit opt-in means the prompt only appears
    // when the user actually picks System Audio / starts recording, not on
    // every app launch.
    void setWantsSystemAudio(bool v) { m_wantsSystemAudio = v; }
    bool wantsSystemAudio() const    { return m_wantsSystemAudio; }

    // Smoothed frequency bands (0-1)
    float bass() const { return m_smoothBass; }
    float lowMid() const { return m_smoothLowMid; }
    float highMid() const { return m_smoothHighMid; }
    float treble() const { return m_smoothTreble; }

    // True RMS from sample buffer (0-1)
    float rms() const { return m_smoothRMS; }

    // Backward compat: smoothed RMS matching old IAudioMeterInformation behavior
    float smoothedRMS() const { return m_smoothRMS; }

    // Beat detection
    float beatDecay() const { return m_beatDecay; }
    bool beatDetected() const { return m_beatThisFrame; }

    // FFT texture (128x1 GL_R8, power spectrum normalized 0-255)
    GLuint fftTexture() const { return m_fftTex.id(); }

    // External feed mode: when true, skip internal WASAPI capture and
    // rely on feedSamples() from an external source (e.g., AudioMixer)
    void setExternalFeed(bool enabled) { m_externalFeed = enabled; }
    bool externalFeed() const { return m_externalFeed; }

    // Feed samples externally (bypasses WASAPI) — used by AudioMixer and tests
    void feedSamples(const float* mono, int count);

    // Expose raw values for testing
    float rawBass() const { return m_rawBass; }
    float rawLowMid() const { return m_rawLowMid; }
    float rawHighMid() const { return m_rawHighMid; }
    float rawTreble() const { return m_rawTreble; }
    float rawRMS() const { return m_rawRMS; }

    // User-adjustable gains (applied on top of default band scaling)
    float& inputGain() { return m_inputGain; }
    float& bassGain() { return m_bassGain; }
    float& lowMidGain() { return m_lowMidGain; }
    float& highMidGain() { return m_highMidGain; }
    float& trebleGain() { return m_trebleGain; }
    float& noiseGate() { return m_noiseGate; }

private:
#ifdef _WIN32
    // WASAPI capture
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    IMMDevice* m_device = nullptr;
#elif defined(__APPLE__)
    void* m_macAudioImpl = nullptr; // Opaque pointer to macOS audio capture
#endif
    int m_deviceIdx = -2; // -2 = uninitialized
    int m_requestedDevice = -1;
    std::string m_deviceId;         // Windows endpoint ID string
    std::string m_requestedDeviceId;
    bool m_requestedIsCapture = false;
    int m_sampleRate = 48000;
    int m_channels = 2;
    bool m_initialized = false;
    bool m_externalFeed = false;
    bool m_captureFailed = false;  // true after permission denied — don't retry
    bool m_wantsSystemAudio = false; // opt-in gate for ScreenCaptureKit (see header)

    void initCapture();
    void cleanupCapture();
    void drainPackets();

    // Ring buffer (mono, 512 samples)
    float m_ringBuf[kFFTSize] = {};
    int m_ringPos = 0;
    int m_samplesAccumulated = 0;

    // FFT output (power spectrum, 256 bins)
    float m_spectrum[kBins] = {};

    // Raw band energies (before smoothing)
    float m_rawBass = 0, m_rawLowMid = 0, m_rawHighMid = 0, m_rawTreble = 0;
    float m_rawRMS = 0;

    // User gains
    float m_inputGain = 1.0f;   // master input multiplier (applied to RMS + bands)
    float m_bassGain = 1.0f;
    float m_lowMidGain = 1.0f;
    float m_highMidGain = 1.0f;
    float m_trebleGain = 1.0f;
    float m_noiseGate = 0.0f;   // values below this threshold are squashed to 0

    // Smoothed values
    float m_smoothBass = 0, m_smoothLowMid = 0, m_smoothHighMid = 0, m_smoothTreble = 0;
    float m_smoothRMS = 0;

    // Beat detection
    float m_beatDecay = 0;
    bool m_beatThisFrame = false;
    float m_energyHistory[32] = {};
    int m_energyHistoryPos = 0;
    float m_beatCooldown = 0; // seconds remaining

    // FFT texture
    Texture m_fftTex;
    uint8_t m_fftTexData[128] = {};

    void runFFT();
    void computeBands();
    void detectBeat(float dt);
    void smoothBands(float dt);
    void updateFFTTexture();

    // dt-based exponential smoothing helper
    static float expSmooth(float current, float target, float rate, float dt) {
        return current + (target - current) * (1.0f - std::exp(-rate * dt));
    }
};
