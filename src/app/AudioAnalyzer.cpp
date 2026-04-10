#include "app/AudioAnalyzer.h"
#include <kiss_fft.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

// Hann window coefficients (precomputed for kFFTSize=512)
static float s_hannWindow[AudioAnalyzer::kFFTSize];
static bool s_hannInit = false;

static void initHannWindow() {
    if (s_hannInit) return;
    for (int i = 0; i < AudioAnalyzer::kFFTSize; i++) {
        s_hannWindow[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (AudioAnalyzer::kFFTSize - 1)));
    }
    s_hannInit = true;
}

AudioAnalyzer::~AudioAnalyzer() {
    cleanupCapture();
}

void AudioAnalyzer::setDevice(int deviceIdx) {
    m_requestedDevice = deviceIdx;
}

void AudioAnalyzer::setDeviceId(const std::string& id, bool isCapture) {
    m_requestedDeviceId = id;
    m_requestedIsCapture = isCapture;
}

void AudioAnalyzer::update(float dt) {
    if (dt <= 0) return;

    // When external feed is active (e.g., AudioMixer), skip internal WASAPI capture
    if (!m_externalFeed) {
        // Reinit capture if device changed or previous init failed
        bool deviceChanged = (m_deviceIdx != m_requestedDevice) || (m_deviceId != m_requestedDeviceId);
        if (deviceChanged || (!m_initialized && m_deviceIdx != -2)) {
            cleanupCapture();
            m_deviceIdx = m_requestedDevice;
            m_deviceId = m_requestedDeviceId;
            initCapture();
        }

        // Drain WASAPI packets (non-blocking)
        if (m_initialized) {
            drainPackets();
        }
    }

    // Run analysis if we have enough samples
    if (m_samplesAccumulated >= kFFTSize) {
        runFFT();
        computeBands();
        m_samplesAccumulated = 0;
    }

    detectBeat(dt);
    smoothBands(dt);
    updateFFTTexture();
}

// --- WASAPI capture ---
// On macOS, these functions are defined in AudioAnalyzer_mac.mm
#ifndef __APPLE__
void AudioAnalyzer::initCapture() {
#ifdef _WIN32
    initHannWindow();

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE is OK — COM is still usable in existing apartment mode
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioAnalyzer] CoInitializeEx failed (hr=0x" << std::hex << comHr << std::dec << ")" << std::endl;
        return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        std::cerr << "[AudioAnalyzer] CoCreateInstance(MMDeviceEnumerator) failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return;
    }

    IMMDevice* device = nullptr;
    bool isLoopback = false;

    if (m_deviceIdx == -1) {
        // System audio loopback (default render endpoint)
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        isLoopback = true;
    } else if (!m_deviceId.empty()) {
        // Look up by device ID string (matches the meter and UI selection)
        std::wstring wid(m_deviceId.begin(), m_deviceId.end());
        hr = enumerator->GetDevice(wid.c_str(), &device);
        isLoopback = !m_requestedIsCapture;
    } else {
        // Fallback: specific device by index — enumerate all devices
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            if (m_deviceIdx >= 0 && m_deviceIdx < (int)count) {
                collection->Item(m_deviceIdx, &device);

                // Determine if this is a render or capture device
                IMMEndpoint* endpoint = nullptr;
                if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), (void**)&endpoint))) {
                    EDataFlow flow;
                    if (SUCCEEDED(endpoint->GetDataFlow(&flow))) {
                        isLoopback = (flow == eRender);
                    }
                    endpoint->Release();
                }
            }
            collection->Release();
        }
    }
    enumerator->Release();

    if (!device) {
        std::cerr << "[AudioAnalyzer] No audio device found (dev=" << m_deviceIdx << ")" << std::endl;
        return;
    }
    m_device = device;

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr) || !audioClient) {
        std::cerr << "[AudioAnalyzer] IAudioClient activation failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return;
    }
    m_audioClient = audioClient;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        std::cerr << "[AudioAnalyzer] GetMixFormat failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return;
    }

    m_sampleRate = mixFmt->nSamplesPerSec;
    m_channels = mixFmt->nChannels;

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    REFERENCE_TIME bufferDuration = defaultPeriod > 0 ? defaultPeriod * 4 : 2000000;

    // Loopback for render devices, normal capture for microphones
    DWORD streamFlags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  streamFlags,
                                  bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[AudioAnalyzer] Initialize failed (hr=0x" << std::hex << hr << std::dec
                  << (isLoopback ? " loopback" : " capture") << ")" << std::endl;
        return;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr) || !captureClient) {
        std::cerr << "[AudioAnalyzer] GetService(IAudioCaptureClient) failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return;
    }
    m_captureClient = captureClient;

    audioClient->Start();
    m_initialized = true;
    std::cout << "[AudioAnalyzer] Capture started (rate=" << m_sampleRate << " ch=" << m_channels << ")" << std::endl;
#endif
}

void AudioAnalyzer::cleanupCapture() {
#ifdef _WIN32
    if (m_audioClient) {
        m_audioClient->Stop();
    }
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    m_initialized = false;
    m_samplesAccumulated = 0;
#endif
}

void AudioAnalyzer::drainPackets() {
#ifdef _WIN32
    if (!m_captureClient) return;

    UINT32 packetLength = 0;
    while (SUCCEEDED(m_captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        HRESULT hr = m_captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
            const float* samples = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < numFrames; i++) {
                // Average stereo to mono
                float mono = 0;
                for (int ch = 0; ch < m_channels; ch++) {
                    mono += samples[i * m_channels + ch];
                }
                mono /= m_channels;

                m_ringBuf[m_ringPos] = mono;
                m_ringPos = (m_ringPos + 1) % kFFTSize;
                m_samplesAccumulated++;
            }
        }

        m_captureClient->ReleaseBuffer(numFrames);
    }
#endif
}
#endif // !__APPLE__

// --- Test injection ---

void AudioAnalyzer::feedSamples(const float* mono, int count) {
    initHannWindow();
    for (int i = 0; i < count; i++) {
        m_ringBuf[m_ringPos] = mono[i];
        m_ringPos = (m_ringPos + 1) % kFFTSize;
        m_samplesAccumulated++;
    }
    // Process immediately when enough samples (for test injection)
    while (m_samplesAccumulated >= kFFTSize) {
        runFFT();
        computeBands();
        m_samplesAccumulated -= kFFTSize;
    }
}

// --- FFT ---

void AudioAnalyzer::runFFT() {
    kiss_fft_cfg cfg = kiss_fft_alloc(kFFTSize, 0, nullptr, nullptr);
    if (!cfg) return;

    kiss_fft_cpx in[kFFTSize];
    kiss_fft_cpx out[kFFTSize];

    // Copy ring buffer with Hann window, starting from oldest sample
    for (int i = 0; i < kFFTSize; i++) {
        int idx = (m_ringPos + i) % kFFTSize;
        in[i].r = m_ringBuf[idx] * s_hannWindow[i];
        in[i].i = 0;
    }

    kiss_fft(cfg, in, out);

    // Compute magnitude spectrum (linear amplitude, not power)
    // Using magnitude instead of power preserves perceptual loudness relationship:
    // power (mag^2) squashes moderate-level audio to near-zero, making bands unusable.
    // Normalize by N/2 so a full-scale sine gives magnitude 1.0 in its bin.
    const float normFactor = 1.0f / (float)(kFFTSize / 2);
    for (int i = 0; i < kBins; i++) {
        float mag = std::sqrt(out[i].r * out[i].r + out[i].i * out[i].i);
        m_spectrum[i] = std::min(mag * normFactor, 1.0f);
    }

    kiss_fft_free(cfg);

    // Compute RMS from raw samples
    float sumSq = 0;
    for (int i = 0; i < kFFTSize; i++) {
        sumSq += m_ringBuf[i] * m_ringBuf[i];
    }
    m_rawRMS = std::sqrt(sumSq / kFFTSize);
    // Clamp to 0-1 (typically peaks around 0.5 for loud audio)
    m_rawRMS = std::min(m_rawRMS * 2.0f, 1.0f);
}

// --- Band computation ---

void AudioAnalyzer::computeBands() {
    // Bass: bins 1-2 (~94-188Hz at 48kHz)
    float bass = 0;
    for (int i = 1; i <= 2; i++) bass += m_spectrum[i];
    bass /= 2.0f;

    // LowMid: bins 3-10 (~282-940Hz)
    float lowMid = 0;
    for (int i = 3; i <= 10; i++) lowMid += m_spectrum[i];
    lowMid /= 8.0f;

    // HighMid: bins 11-42 (~1034-3948Hz)
    float highMid = 0;
    for (int i = 11; i <= 42; i++) highMid += m_spectrum[i];
    highMid /= 32.0f;

    // Treble: bins 43-128 (~4042-12kHz)
    float treble = 0;
    int trebleCount = std::min(kBins, 128) - 43 + 1;
    for (int i = 43; i < std::min(kBins, 129); i++) treble += m_spectrum[i];
    treble /= trebleCount;

    // Scale each band to useful 0-1 range
    // Using magnitude spectrum (linear), so gains are higher than power-based.
    // Tuned for typical system audio at moderate listening volume.
    m_rawBass = std::min(bass * 60.0f, 1.0f);
    m_rawLowMid = std::min(lowMid * 100.0f, 1.0f);
    m_rawHighMid = std::min(highMid * 200.0f, 1.0f);
    m_rawTreble = std::min(treble * 400.0f, 1.0f);
}

// --- Beat detection ---

void AudioAnalyzer::detectBeat(float dt) {
    m_beatThisFrame = false;

    // Update cooldown
    if (m_beatCooldown > 0) {
        m_beatCooldown -= dt;
    }

    // Current energy (bass-weighted)
    float energy = m_rawBass * 0.7f + m_rawLowMid * 0.3f;

    // Rolling average
    float avg = 0;
    for (int i = 0; i < 32; i++) avg += m_energyHistory[i];
    avg /= 32.0f;

    // Store current energy
    m_energyHistory[m_energyHistoryPos] = energy;
    m_energyHistoryPos = (m_energyHistoryPos + 1) % 32;

    // Beat detected if energy > 1.4x rolling average and cooldown expired
    if (energy > avg * 1.4f + 0.05f && m_beatCooldown <= 0) {
        m_beatThisFrame = true;
        m_beatDecay = 1.0f;
        m_beatCooldown = 0.15f; // 150ms minimum between beats
    }

    // Exponential decay (~85ms half-life)
    // half-life = ln(2) / rate -> rate = ln(2) / 0.085 ≈ 8.15
    m_beatDecay *= std::exp(-8.15f * dt);
    if (m_beatDecay < 0.001f) m_beatDecay = 0;
}

// --- Smoothing ---

void AudioAnalyzer::smoothBands(float dt) {
    // Attack: 8/s, Release: 3/s (dt-independent)
    // Lower attack rate reduces jitter while staying responsive to beats
    auto smooth = [&](float& current, float raw) {
        float rate = (raw > current) ? 8.0f : 3.0f;
        current = expSmooth(current, raw, rate, dt);
    };

    smooth(m_smoothBass, m_rawBass);
    smooth(m_smoothLowMid, m_rawLowMid);
    smooth(m_smoothHighMid, m_rawHighMid);
    smooth(m_smoothTreble, m_rawTreble);
    smooth(m_smoothRMS, m_rawRMS);
}

// --- FFT texture ---

void AudioAnalyzer::updateFFTTexture() {
    // Map first 128 bins to texture data
    for (int i = 0; i < 128; i++) {
        float v = (i < kBins) ? m_spectrum[i] : 0.0f;
        m_fftTexData[i] = (uint8_t)(std::min(v, 1.0f) * 255.0f);
    }

    // Lazy-create and upload to GPU (only if GL context is active)
    if (glGenTextures) {  // glad sets function pointers to null before loading
        if (!m_fftTex.id()) {
            m_fftTex.createEmpty(128, 1, GL_R8);
        }
        if (m_fftTex.id()) {
            m_fftTex.updateData(m_fftTexData, 128, 1, GL_RED, GL_UNSIGNED_BYTE);
        }
    }
}
