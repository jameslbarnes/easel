#include "app/AudioAnalyzer.h"
#include <kiss_fft.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <thread>
#include <chrono>

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
    // A detached init thread may still be inside initCapture — let it land
    // before tearing the capture state down under it.
    while (m_initInFlight.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cleanupCapture();
    if (m_fftCfg) {
        kiss_fft_free(m_fftCfg);
        m_fftCfg = nullptr;
    }
}

void AudioAnalyzer::setDevice(int deviceIdx) {
    m_requestedDevice = deviceIdx;
}

void AudioAnalyzer::setDeviceId(const std::string& id, bool isCapture) {
    m_requestedDeviceId = id;
    m_requestedIsCapture = isCapture;
}

void AudioAnalyzer::stopCapture() {
    cleanupCapture();
    m_deviceIdx = -2; // uninitialized — forces reinit on next update() with a device
    m_initialized = false;
    m_captureFailed = false;
    m_samplesAccumulated = 0;
    m_ringPos = 0;
}

void AudioAnalyzer::update(float dt) {
    if (dt <= 0) return;

    // When external feed is active (e.g., AudioMixer), skip internal WASAPI capture
    if (!m_externalFeed) {
        // Reinit capture if device changed or previous init failed
        bool deviceChanged = (m_deviceIdx != m_requestedDevice) || (m_deviceId != m_requestedDeviceId);
        if (deviceChanged) {
            m_captureFailed = false; // reset on device change
        }
        if (!m_captureFailed && !m_initInFlight.load(std::memory_order_acquire) &&
            (deviceChanged || (!m_initialized && m_deviceIdx != -2))) {
            cleanupCapture();
            m_deviceIdx = m_requestedDevice;
            m_deviceId = m_requestedDeviceId;
            // initCapture blocks for seconds on macOS (ScreenCaptureKit
            // bring-up, TCC permission waits) — run it off the render
            // thread so launch never freezes. While it's in flight the
            // condition above stays false via m_initInFlight.
            m_initInFlight.store(true, std::memory_order_release);
            std::thread([this]() {
                initCapture();
                if (!m_initialized) {
                    m_captureFailed = true;
                }
                m_initInFlight.store(false, std::memory_order_release);
            }).detach();
        }

        // Drain WASAPI packets (non-blocking)
        if (m_initialized && !m_initInFlight.load(std::memory_order_acquire)) {
            drainPackets();
        }
    }

    // Run analysis if we have enough samples
    if (m_samplesAccumulated >= kFFTSize) {
        runFFT();
        computeBands();
        m_samplesAccumulated = 0;
    }

    conditionBands(dt);       // dB-domain AGC + gate + response curves
    detectBeat(dt);
    smoothBands(dt);
    computeTemperaments(dt);  // Hit / Presence / Time matrix + tempo feed
    computeStems(dt);         // tier-1 pseudo-stems (HPSS + band split)
    computeSpectralFeatures(dt);
    computeChroma(dt);      // chroma / dominant pitch / major-minor (feeds affect + palette)
    computeAffect(dt);      // reads previous-frame buildup (DAG)
    computeStructure(dt);   // computes this-frame buildup
    computeFlow(dt);
    computePalette(dt);     // Tier 5 Oklch ramp (uses brightness/arousal/warmth/beat/spread)
    m_prevBuildup = m_buildup;   // close the DAG
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
    initHannWindow();
    // Cached kiss_fft config — allocated once, reused every call
    // (previously alloc/freed per frame).
    kiss_fft_cfg cfg = (kiss_fft_cfg)m_fftCfg;
    if (!cfg) {
        cfg = kiss_fft_alloc(kFFTSize, 0, nullptr, nullptr);
        m_fftCfg = cfg;
    }
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
    m_specDirty = true;   // new spectrum for the HPSS/stem pass

    // Compute RMS from raw samples — kept LINEAR (pre-AGC) here; the 0-1
    // normalized value shaders see comes out of the dB-domain AGC in
    // conditionBands(). The legacy 2x factor is preserved so crest/punch
    // computations keep their historical scaling.
    float sumSq = 0;
    for (int i = 0; i < kFFTSize; i++) {
        sumSq += m_ringBuf[i] * m_ringBuf[i];
    }
    m_rmsLin = std::min(std::sqrt(sumSq / kFFTSize) * 2.0f * m_inputGain, 1.0f);
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

    // Store PRE-AGC linear band energies. The user gains (master + per-band
    // trim) apply here — they are the defeatable "manual gain" stage in
    // front of the automatic gain control. The old fixed magic gains
    // (60/100/200/400x) are gone: normalization now happens in the
    // dB-domain AGC in conditionBands() (EaselAudio spec §3).
    float masterG = m_inputGain;
    m_bandE[0] = bass    * masterG * m_bassGain;
    m_bandE[1] = lowMid  * masterG * m_lowMidGain;
    m_bandE[2] = highMid * masterG * m_highMidGain;
    m_bandE[3] = treble  * masterG * m_trebleGain;
}

// --- Band conditioning: dB AGC + gate + response curves (every frame) ---
// Per band: norm = clamp((dB - noiseFloor) / range, 0, 1) with the -60 dB
// noise floor doubling as the silence gate and `range` auto-tracked
// fast-grow (0.1s) / slow-shrink (60s) so there's no pumping. Runs every
// frame (not just on FFT frames) so the range tracking stays dt-derived.
void AudioAnalyzer::conditionBands(float dt) {
    float b[4];
    for (int i = 0; i < 4; i++) b[i] = m_bandAgc[i].norm(m_bandE[i], dt);
    m_rawRMS = m_rmsAgc.norm(m_rmsLin, dt);

    // Noise gate: values below threshold collapse to 0 (user control,
    // unchanged semantics — now applied to the AGC'd 0-1 values)
    if (m_noiseGate > 0.0f) {
        auto gate = [&](float v) {
            if (v < m_noiseGate) return 0.0f;
            // Rescale remaining range to 0-1 for smooth transition
            return (v - m_noiseGate) / std::max(0.001f, 1.0f - m_noiseGate);
        };
        for (int i = 0; i < 4; i++) b[i] = gate(b[i]);
    }

    // Response curves: capture the pre-curve input (for the live graph), then
    // shape each band by its own curve followed by the global master curve.
    // Defaults are identity, so this is a no-op until the user dials a curve.
    m_curveInput[CurveBass]    = b[0];
    m_curveInput[CurveLowMid]  = b[1];
    m_curveInput[CurveHighMid] = b[2];
    m_curveInput[CurveTreble]  = b[3];
    m_curveInput[CurveMaster]  = std::max(std::max(b[0], b[1]),
                                          std::max(b[2], b[3]));

    m_rawBass    = applyAudioCurve(applyAudioCurve(b[0], m_curves[CurveBass]),    m_curves[CurveMaster]);
    m_rawLowMid  = applyAudioCurve(applyAudioCurve(b[1], m_curves[CurveLowMid]),  m_curves[CurveMaster]);
    m_rawHighMid = applyAudioCurve(applyAudioCurve(b[2], m_curves[CurveHighMid]), m_curves[CurveMaster]);
    m_rawTreble  = applyAudioCurve(applyAudioCurve(b[3], m_curves[CurveTreble]),  m_curves[CurveMaster]);
}

// --- EaselAudio v1: temperament matrix + tempo feed ---------------------
// Hit  — dual-follower onset per band, fed PRE-AGC energy (self-normalizing).
// Presence — slow macro envelope (1.5s/4s) of the AGC'd band.
// Time — monotonic clock += dt * band (the value shaders see), unclamped.
void AudioAnalyzer::computeTemperaments(float dt) {
    float pre[3] = { m_bandE[0], (m_bandE[1] + m_bandE[2]) * 0.5f, m_bandE[3] };
    for (int i = 0; i < 3; i++) m_hitOut[i] = m_hitDet[i].update(pre[i], dt);

    float post[4] = { m_rawBass, (m_rawLowMid + m_rawHighMid) * 0.5f,
                      m_rawTreble, m_rawRMS };
    for (int i = 0; i < 4; i++) m_presOut[i] = m_presEnv[i].update(post[i], dt);

    m_timeClock[0] += dt * m_smoothBass;
    m_timeClock[1] += dt * (m_smoothLowMid + m_smoothHighMid) * 0.5f;
    m_timeClock[2] += dt * m_smoothTreble;
    m_timeClock[3] += dt * m_smoothRMS;

    // Tempo: autocorrelation over the onset-strength envelope (normalized
    // spectral flux from the previous frame's spectral pass). Gate the feed
    // on real signal: flux normalization amplifies the noise floor, so in
    // silence the tracker hears phantom rhythms and locks the beat clock
    // to junk tempos. Raw RMS is pre-AGC — digital silence reads ~0 there,
    // no real program material does.
    m_tempo.feed(m_rawRMS > 1e-4f ? m_fluxNorm : 0.0f, dt);
}

// --- EaselAudio v1: tier-1 pseudo-stems ----------------------------------
// Zero-ML instant stems (spec §1.4): band split + causal median-filter HPSS
// (past-only kernel, power 2.0, margin 2.5; one FFT-hop latency).
//   stemBass   — sub+low band level (kick body + bassline)
//   stemDrums  — HPSS percussive level (all-band transients)
//   stemMelody — harmonic minus sub (tonal body ~190Hz-2k)
//   stemAir    — >2k harmonic wash (pads / cymbals)
//   stemVocal  — 2-6kHz harmonic x spectral-flux gate (vocal approximation)
// Note: the "Linkwitz-Riley" split of the spec is realized spectrally (band
// sums on the magnitude spectrum) — equivalent for level extraction at this
// FFT size; crossover edges land on the nearest bins (~94Hz resolution).
// Per-stem conditioning: stem role preset = +50% release vs mix bands
// (mask-flutter guard); stemDrumsHit runs a lower threshold (bleed-free).
void AudioAnalyzer::computeStems(float dt) {
    if (!m_stemInit) {
        for (int s = 0; s < StemCount; s++) {
            m_stemCond[s].p = easelaudio::ConditionerParams::stem();
            m_stemHitDet[s].threshMul = (s == StemDrums) ? 1.08f : 1.15f;
        }
        m_stemInit = true;
    }

    if (m_specDirty) {
        m_specDirty = false;

        // Push current spectrum into the causal history ring
        std::memcpy(m_specHist[m_specHistPos], m_spectrum, sizeof(m_spectrum));
        int cur = m_specHistPos;
        m_specHistPos = (m_specHistPos + 1) % kHPSSFrames;
        if (m_specHistCount < kHPSSFrames) m_specHistCount++;

        const float binHz = (float)m_sampleRate / (float)kFFTSize;
        const int kHi = std::min(kBins, 220);
        int bBassHi   = std::max(2, (int)(200.0f / binHz) + 1);              // sub+low
        int bMelodyHi = std::min(kHi - 2, std::max(bBassHi + 1, (int)(2000.0f / binHz) + 1));
        int bVocalHi  = std::min(kHi, (int)(6000.0f / binHz) + 1);

        // Causal median HPSS (past-only): H = median across TIME per bin
        // (harmonic estimate), P = median across FREQUENCY per bin
        // (percussive estimate), then soft Wiener masks p=2 margin=2.5.
        float H[kBins], P[kBins];
        const int n = m_specHistCount;
        for (int i = 1; i < kHi; i++) {
            float tmp[kHPSSFrames];
            for (int f = 0; f < n; f++) tmp[f] = m_specHist[f][i];
            std::nth_element(tmp, tmp + n / 2, tmp + n);
            H[i] = tmp[n / 2];
        }
        const float* S = m_specHist[cur];
        for (int i = 1; i < kHi; i++) {
            float tmp[kHPSSKernel];
            int lo = std::max(1, i - kHPSSKernel / 2);
            int hi = std::min(kHi, lo + kHPSSKernel);
            lo = std::max(1, hi - kHPSSKernel);
            int m = hi - lo;
            for (int j = 0; j < m; j++) tmp[j] = S[lo + j];
            std::nth_element(tmp, tmp + m / 2, tmp + m);
            P[i] = tmp[m / 2];
        }

        const float margin2 = 2.5f * 2.5f;   // margin^2 for the power-2 masks
        float bassE = 0, drumsE = 0, melodyE = 0, airE = 0, vocalE = 0, vFlux = 0;
        int prev = (cur - 1 + kHPSSFrames) % kHPSSFrames;
        for (int i = 1; i < kHi; i++) {
            float h2 = H[i] * H[i], p2 = P[i] * P[i];
            float mh = h2 / (h2 + margin2 * p2 + 1e-12f);
            float mp = p2 / (p2 + margin2 * h2 + 1e-12f);
            float sh = S[i] * mh, sp = S[i] * mp;
            drumsE += sp;
            if (i < bBassHi)        bassE   += S[i];
            else if (i < bMelodyHi) melodyE += sh;
            else                    airE    += sh;
            if (i >= bMelodyHi && i < bVocalHi) {
                vocalE += sh;
                if (m_specHistCount > 1) {
                    float d = S[i] - m_specHist[prev][i];
                    if (d > 0.0f) vFlux += d;
                }
            }
        }
        // Mean-normalize per band width so the AGCs see comparable levels
        m_stemE[StemBass]   = bassE   / (float)std::max(1, bBassHi - 1);
        m_stemE[StemDrums]  = drumsE  / (float)std::max(1, kHi - 1);
        m_stemE[StemMelody] = melodyE / (float)std::max(1, bMelodyHi - bBassHi);
        m_stemE[StemAir]    = airE    / (float)std::max(1, kHi - bMelodyHi);
        m_stemE[StemVocal]  = vocalE  / (float)std::max(1, bVocalHi - bMelodyHi);
        m_lastVocalFlux     = vFlux   / (float)std::max(1, bVocalHi - bMelodyHi);
    }

    // Vocal presence gate: movement (flux) in the 2-6k band relative to its
    // own running average — a steady synth pad doesn't read as a vocal.
    m_vocalFluxAvg += (m_lastVocalFlux - m_vocalFluxAvg) * easelaudio::k1(dt, 2.0f);
    float g = m_lastVocalFlux / std::max(m_vocalFluxAvg, 1e-6f);
    float gateT = easelaudio::clamp01((g - 0.5f) / 1.2f);
    m_vocalFluxGate += (gateT - m_vocalFluxGate) * easelaudio::k1(dt, 0.25f);

    for (int s = 0; s < StemCount; s++) {
        float e = m_stemE[s] * (s == StemVocal ? m_vocalFluxGate : 1.0f);
        float norm = m_stemAgc[s].norm(e * m_inputGain, dt);
        m_stemOut[s]     = m_stemCond[s].process(norm, dt);
        m_stemHitOut[s]  = m_stemHitDet[s].update(e, dt);   // pre-AGC (self-norm)
        m_stemPresOut[s] = m_stemPresEnv[s].update(m_stemOut[s], dt);
    }
}

// --- Spectral character (Tier 2) + sub/punch (Tier 1 extras) ---
// One linear pass over the 256-bin magnitude spectrum + a few EMAs. All
// outputs are normalized 0-1 (tilt is -1..1) and lightly smoothed so they
// read as natural, not jittery.
void AudioAnalyzer::computeSpectralFeatures(float dt) {
    const int kLo = 2;                 // skip DC + bin1 (sub handled separately)
    const int kHi = std::min(kBins, 220);

    // --- sub-bass (coarse; spectrum resolution can't isolate <90Hz, so this
    // is an honest bin-1 proxy — dB-AGC'd instead of the old fixed 60x) ---
    float subRaw = m_subAgc.norm(m_spectrum[1] * m_inputGain, dt);
    m_smoothSub = expSmooth(m_smoothSub, subRaw, subRaw > m_smoothSub ? 30.0f : 8.0f, dt);

    // --- single pass: total energy, centroid numerator, flatness accumulators,
    //     spectral flux, peak magnitude ---
    float total = 0.0f, centNum = 0.0f, logSum = 0.0f, flux = 0.0f, peak = 0.0f;
    int   flatN = 0;
    for (int i = kLo; i < kHi; i++) {
        float s = m_spectrum[i];
        total  += s;
        centNum += s * (float)i;
        if (i >= 4 && i < 200) { logSum += std::log(s + 1e-6f); flatN++; }
        float d = s - m_prevSpec[i];
        if (d > 0.0f) flux += d;
        if (s > peak) peak = s;
    }
    float invTotal = 1.0f / std::max(total, 1e-6f);

    // --- centroid -> brightness (log2 map ~bin2..bin200) ---
    float centroidBin = centNum * invTotal;            // in [kLo,kHi]
    float cNorm = (std::log2(std::max(centroidBin, 1.0f)) - 1.0f) /
                  (std::log2((float)kHi) - 1.0f);
    cNorm = std::min(std::max(cNorm, 0.0f), 1.0f);
    m_brightness = expSmooth(m_brightness, m_agcBright.norm(cNorm, dt), 2.5f, dt);

    // --- spread (std dev around centroid, normalized) ---
    float var = 0.0f;
    for (int i = kLo; i < kHi; i++) {
        float dd = (float)i - centroidBin;
        var += m_spectrum[i] * dd * dd;
    }
    float sd = std::sqrt(var * invTotal) / ((float)kHi / 3.0f);
    m_spread = expSmooth(m_spread, std::min(sd, 1.0f), 3.0f, dt);

    // --- rolloff (85% cumulative energy bin, log-mapped) ---
    float cum = 0.0f, target = total * 0.85f; int rollBin = kLo;
    for (int i = kLo; i < kHi; i++) { cum += m_spectrum[i]; if (cum >= target) { rollBin = i; break; } }
    float rNorm = (std::log2(std::max((float)rollBin, 1.0f)) - 1.0f) /
                  (std::log2((float)kHi) - 1.0f);
    m_rolloff = expSmooth(m_rolloff, std::min(std::max(rNorm, 0.0f), 1.0f), 3.0f, dt);

    // --- flatness (geometric/arithmetic mean) -> noisiness ---
    float amean = (total > 0 ? (centNum * 0.0f + total) : 0.0f); // (reuse total)
    float arithMean = total / std::max(1, (kHi - kLo));
    float geoMean = (flatN > 0) ? std::exp(logSum / flatN) : 0.0f;
    float flatRaw = (arithMean > 1e-6f) ? std::min(geoMean / arithMean, 1.0f) : 0.0f;
    (void)amean;
    m_flatness = expSmooth(m_flatness, m_agcFlat.norm(flatRaw, dt), 4.0f, dt);

    // --- flux -> movement (normalized by slow running floor) ---
    m_fluxFloor += (flux - m_fluxFloor) * (1.0f - std::exp(-0.5f * dt));
    float fluxN = m_agcFlux.norm(flux, dt);
    m_fluxNorm = fluxN;   // unsmoothed onset strength for the tempo tracker
    m_flux = expSmooth(m_flux, fluxN, 6.0f, dt);

    // --- onset: peak-pick flux above an adaptive median, with refractory ---
    m_onsetMedian += (flux - m_onsetMedian) * (1.0f - std::exp(-1.0f * dt));
    if (m_beatCooldownOnset > 0.0f) m_beatCooldownOnset -= dt;
    float onsetTrig = 0.0f;
    bool aboveFloor = flux > (m_fluxFloor * 2.0f + 1e-4f);
    if (flux > m_onsetMedian * 1.5f && aboveFloor && m_beatCooldownOnset <= 0.0f) {
        onsetTrig = 1.0f; m_beatCooldownOnset = 0.06f;        // 60ms refractory
        m_onsetRate = std::min(m_onsetRate + 1.0f / 8.0f, 1.0f);
    }
    m_onset = m_onsetPH.update(onsetTrig, dt, 0.0f, 0.08f);
    m_onsetRate *= std::exp(-dt / 1.5f);

    // --- tilt: warm(-) vs harsh(+), from smoothed bands ---
    float lowE = m_smoothBass + m_smoothLowMid;
    float hiE  = m_smoothHighMid + m_smoothTreble;
    float tiltRaw = (hiE - lowE) / std::max(hiE + lowE, 1e-4f); // -1..1
    m_tilt = expSmooth(m_tilt, tiltRaw, 2.0f, dt);

    // --- zcr: sign changes over the time ring buffer ---
    int zc = 0;
    for (int i = 1; i < kFFTSize; i++)
        if ((m_ringBuf[i] >= 0.0f) != (m_ringBuf[i - 1] >= 0.0f)) zc++;
    float zcrRaw = (float)zc / (float)kFFTSize;           // 0..1 (rarely >0.5)
    m_zcr = expSmooth(m_zcr, std::min(zcrRaw * 2.0f, 1.0f), 5.0f, dt);

    // --- texture: sustained crispy(1) vs smooth(0) — tilt-shaped, noise-aware ---
    float texRaw = 0.6f * (0.5f + 0.5f * m_tilt) + 0.4f * m_flatness;
    m_texture = expSmooth(m_texture, std::min(std::max(texRaw, 0.0f), 1.0f), 4.0f, dt);

    // --- punch: crest factor peak/rms mapped 1..6 -> 0..1, peak-held ---
    // (uses the LINEAR rms — m_rawRMS is AGC-normalized now)
    float crest = peak / std::max(m_rmsLin, 1e-3f);
    float punchRaw = std::min(std::max((crest - 1.0f) / 5.0f, 0.0f), 1.0f);
    m_punch = m_punchPH.update(punchRaw, dt, 0.05f, 0.12f);

    // snapshot spectrum for next-frame flux
    std::memcpy(m_prevSpec, m_spectrum, sizeof(m_prevSpec));
}

// --- Tier 3: affect / mood ---
// Slow, session-stable scalars derived from Tier 1-2. valence/majorMinor use
// a chroma-free approximation for now (brightness + consonance proxy); the
// palette phase upgrades them with real chroma/Krumhansl-Kessler mode.
void AudioAnalyzer::computeAffect(float dt) {
    // roughness ~ dissonance proxy (broadband + change). Real Plomp-Levelt
    // over spectral peaks is a later refinement.
    float roughRaw = 0.6f * m_flatness + 0.4f * m_flux;
    m_roughness = expSmooth(m_roughness, std::min(roughRaw, 1.0f), 2.0f, dt);

    // arousal — calm vs energetic
    float arousalRaw = 0.35f * m_smoothRMS + 0.30f * m_flux + 0.20f * m_smoothTreble + 0.15f * m_punch;
    m_arousal = expSmooth(m_arousal, std::min(arousalRaw, 1.0f),
                          arousalRaw > m_arousal ? 8.0f : 1.6f, dt);

    // valence — bright/pleasant vs sad/dark (mode-aware now that chroma exists)
    float valRaw = 0.35f * m_brightness + 0.25f * (1.0f - m_roughness)
                 + 0.20f * (0.5f + 0.5f * m_tilt) + 0.20f * m_majorMinor;
    m_valence = expSmooth(m_valence, std::min(std::max(valRaw, 0.0f), 1.0f),
                          valRaw > m_valence ? 3.0f : 0.7f, dt);

    // tension — reads PREVIOUS-frame buildup (DAG)
    float tenRaw = 0.40f * m_roughness + 0.30f * m_prevBuildup + 0.30f * (1.0f - m_valence);
    m_tension = expSmooth(m_tension, std::min(tenRaw, 1.0f),
                          tenRaw > m_tension ? 5.0f : 1.25f, dt);
    m_tension *= (1.0f - 0.7f * m_drop);   // release on the drop

    // warmth — warm/intimate vs cold/airy
    float warmRaw = 0.50f * m_smoothBass + 0.25f * (1.0f - m_smoothTreble) + 0.25f * (1.0f - m_flatness);
    m_warmth = expSmooth(m_warmth, std::min(warmRaw, 1.0f), 2.0f, dt);

    // softness — gentle/blurred vs sharp (decorrelated from tension)
    float softRaw = 0.40f * (1.0f - m_punch) + 0.30f * (1.0f - m_flux) + 0.30f * (1.0f - m_onsetRate);
    m_softness = expSmooth(m_softness, std::min(std::max(softRaw, 0.0f), 1.0f), 2.5f, dt);

    // charm — "lovely groove" sweet spot (widened gaussian + floor)
    float g = std::exp(-(m_arousal - 0.5f) * (m_arousal - 0.5f) / (2.0f * 0.35f * 0.35f));
    m_charm = m_valence * (1.0f - m_tension) * g + 0.05f;
    if (m_charm > 1.0f) m_charm = 1.0f;
}

// --- Tier 4: structure / build-up ---
// Dual-timescale energy EMAs: slow = where the song's been, fast = where it
// is, fast-slow = where it's going. All causal (no lookahead).
void AudioAnalyzer::computeStructure(float dt) {
    // 12 log-spaced bands over the spectrum (assembly / novelty / layers).
    float total = 0.0f;
    for (int b = 0; b < 12; b++) {
        int lo = (int)(2.0f * std::pow((float)kBins / 2.0f, b / 12.0f));
        int hi = (int)(2.0f * std::pow((float)kBins / 2.0f, (b + 1) / 12.0f));
        lo = std::max(2, lo); hi = std::min(kBins, std::max(lo + 1, hi));
        float e = 0; for (int i = lo; i < hi; i++) e += m_spectrum[i];
        e /= (float)(hi - lo);
        m_band12[b] = e;
        total += e;
        m_band12Slow[b] += (e - m_band12Slow[b]) * (1.0f - std::exp(-0.25f * dt));
    }

    // energy altitude (normalized by a slowly-decaying running max)
    float energyRaw = std::log(1.0f + total * 12.0f);
    m_energyRunMax = std::max(energyRaw, m_energyRunMax * std::exp(-dt / 30.0f));
    float eNorm = energyRaw / std::max(m_energyRunMax, 1e-3f);
    m_energyFast += (eNorm - m_energyFast) * (1.0f - std::exp(-dt / 1.0f));
    m_energySlow += (eNorm - m_energySlow) * (1.0f - std::exp(-dt / 8.0f));
    m_energy = expSmooth(m_energy, m_energySlow, 0.25f, dt);

    // velocity / acceleration of the altitude (clamped, post-smoothed)
    float vel = (m_energy - m_prevEnergy) / std::max(dt, 1e-3f);
    vel = std::min(std::max(vel / 2.0f, -1.0f), 1.0f);
    m_energyVel = expSmooth(m_energyVel, vel, 2.0f, dt);
    float acc = (m_energyVel - m_prevEnergyVel) / std::max(dt, 1e-3f);
    acc = std::min(std::max(acc / 4.0f, -1.0f), 1.0f);
    m_energyAcc = expSmooth(m_energyAcc, acc, 2.0f, dt);
    m_prevEnergy = m_energy; m_prevEnergyVel = m_energyVel;

    // buildup — integrate the "rising above baseline" signal, hold, decay
    float rise = (m_energyFast - m_energySlow) / std::max(m_energySlow, 0.05f);
    rise = std::min(std::max(rise, 0.0f), 1.0f);
    rise = 0.6f * rise + 0.4f * m_onsetRate;   // accelerating onsets count too
    float prevB = m_buildup;
    m_buildup = std::min(m_buildup * std::exp(-dt / 1.6f) + rise * dt * 2.0f, 1.0f);
    m_buildupRate = expSmooth(m_buildupRate,
                              std::min(std::max((m_buildup - prevB) / std::max(dt, 1e-3f), -1.0f), 1.0f),
                              2.5f, dt);

    // drop — high prior buildup released by a broadband slam
    float dropTrig = 0.0f;
    if (m_prevBuildup > 0.5f && m_onset > 0.4f && m_smoothBass > 0.5f) {
        dropTrig = 1.0f;
        m_buildup *= 0.2f;   // release
    }
    m_drop = m_dropPH.update(dropTrig, dt, 0.0f, 0.8f);

    // novelty — cosine distance of band shape vs its slow average
    float dot = 0, na = 0, nb = 0;
    for (int b = 0; b < 12; b++) { dot += m_band12[b]*m_band12Slow[b]; na += m_band12[b]*m_band12[b]; nb += m_band12Slow[b]*m_band12Slow[b]; }
    float cosSim = (na > 1e-9f && nb > 1e-9f) ? dot / std::sqrt(na * nb) : 1.0f;
    float novRaw = std::min(std::max(1.0f - cosSim, 0.0f), 1.0f);
    m_novelty = expSmooth(m_novelty, novRaw, novRaw > m_novelty ? 8.0f : 0.5f, dt);

    // section tracking
    if (m_sectionCooldown > 0.0f) m_sectionCooldown -= dt;
    if (m_novelty > 0.4f && m_sectionCooldown <= 0.0f) {
        m_sectionCount++; m_sectionAge = 0.0f; m_sectionCooldown = 4.0f;
    }
    m_sectionPhase = (float)(m_sectionCount % 8) / 8.0f;
    m_sectionAge = std::min(m_sectionAge + dt / 60.0f, 1.0f);

    // layers — active element count; presence — per-band masks
    int active = 0;
    for (int b = 0; b < 12; b++) if (m_band12[b] > m_band12Slow[b] * 1.4f + 1e-4f) active++;
    float layersRaw = (float)active / 12.0f;
    m_layers = expSmooth(m_layers, layersRaw, layersRaw > m_layers ? 2.5f : 0.5f, dt);
    float pres[4] = { m_smoothBass, m_smoothLowMid, m_smoothHighMid, m_smoothTreble };
    for (int i = 0; i < 4; i++) {
        float on = pres[i] > 0.12f ? 1.0f : 0.0f;
        m_presence[i] = expSmooth(m_presence[i], on, on > m_presence[i] ? 2.5f : 0.5f, dt);
    }
    m_density = expSmooth(m_density, m_flatness, 2.0f, dt);
}

// --- Flow: a continuous low-frequency drift vector the whole field can ride ---
void AudioAnalyzer::computeFlow(float dt) {
    float tx = (m_smoothBass - m_smoothTreble);          // -1..1
    float ty = (m_smoothLowMid - m_smoothHighMid);
    m_flow[0] = expSmooth(m_flow[0], std::min(std::max(tx, -1.0f), 1.0f), 1.25f, dt);
    m_flow[1] = expSmooth(m_flow[1], std::min(std::max(ty, -1.0f), 1.0f), 1.25f, dt);
}

// --- Oklch (L, C, hue-radians) -> linear sRGB (Björn Ottosson's Oklab) ---
static void oklchToLinear(float L, float C, float h, float* out) {
    float a = C * std::cos(h), b = C * std::sin(h);
    float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
    float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
    float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    float r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float bl = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    out[0] = std::min(std::max(r, 0.0f), 1.0f);
    out[1] = std::min(std::max(g, 0.0f), 1.0f);
    out[2] = std::min(std::max(bl, 0.0f), 1.0f);
}

// --- Tier 5 chroma: fold the spectrum into 12 pitch classes ---
void AudioAnalyzer::computeChroma(float dt) {
    if (!m_binToPCInit) {
        for (int i = 0; i < kBins; i++) {
            float freq = (float)i * (float)m_sampleRate / (float)kFFTSize;
            if (freq < 27.5f) { m_binToPC[i] = -1; continue; }       // below A0
            float midi = 69.0f + 12.0f * std::log2(freq / 440.0f);
            int pc = ((int)std::lround(midi)) % 12; if (pc < 0) pc += 12;
            m_binToPC[i] = pc;
        }
        m_binToPCInit = true;
    }
    float c[12] = {0}; float maxc = 0; int dom = 0;
    for (int i = 2; i < kBins; i++) { int pc = m_binToPC[i]; if (pc >= 0) c[pc] += m_spectrum[i]; }
    for (int p = 0; p < 12; p++) { if (c[p] > maxc) { maxc = c[p]; dom = p; } }
    // L-inf normalize + smooth
    float inv = maxc > 1e-6f ? 1.0f / maxc : 0.0f;
    for (int p = 0; p < 12; p++)
        m_chroma[p] = expSmooth(m_chroma[p], c[p] * inv, 3.0f, dt);
    m_dominantPitch = (float)dom / 12.0f;

    // major/minor proxy: third above the dominant (major=+4 semis, minor=+3).
    float majFit = m_chroma[(dom + 4) % 12] + m_chroma[(dom + 7) % 12]; // major 3rd + 5th
    float minFit = m_chroma[(dom + 3) % 12] + m_chroma[(dom + 7) % 12]; // minor 3rd + 5th
    float mm = 0.5f + 0.5f * std::tanh(2.0f * (majFit - minFit));
    m_majorMinor = expSmooth(m_majorMinor, std::min(std::max(mm, 0.0f), 1.0f), 0.6f, dt);
}

// --- Tier 5 palette: harmonious-by-construction Oklch ramp + accent ---
void AudioAnalyzer::computePalette(float dt) {
    const float TWO_PI = 6.28318530718f;
    // Hue = energy-weighted circular mean of the 12 pitch classes.
    float sx = 0, sy = 0;
    for (int p = 0; p < 12; p++) {
        float ang = (float)p / 12.0f * TWO_PI;
        sx += m_chroma[p] * std::cos(ang);
        sy += m_chroma[p] * std::sin(ang);
    }
    if (sx == 0 && sy == 0) sx = 1; // default hue when silent
    // circular-smooth the hue (slerp the sin/cos pair) so it never flickers
    float k = 1.0f - std::exp(-dt / 1.5f);
    float mag = std::sqrt(sx * sx + sy * sy); if (mag < 1e-6f) mag = 1;
    m_hueCos += (sx / mag - m_hueCos) * k;
    m_hueSin += (sy / mag - m_hueSin) * k;
    float H = std::atan2(m_hueSin, m_hueCos);

    // chroma dispersion: narrow (analogous) vs wide (triad) palette spread
    float csum = 0; for (int p = 0; p < 12; p++) csum += m_chroma[p];
    float disp = (csum > 1e-6f) ? std::min(1.0f - (sx*sx+sy*sy) / (csum*csum), 1.0f) : 0.5f;
    float offset = (18.0f + (130.0f - 18.0f) * disp) * (TWO_PI / 360.0f);

    float V = m_brightness;
    float Cbase = 0.02f + (0.16f - 0.02f) * m_arousal;   // capped chroma (harmonious)

    oklchToLinear(0.18f + 0.14f * V, Cbase * 0.6f, H - offset * 0.5f, m_palShadow);
    oklchToLinear(0.45f + 0.17f * V, Cbase,        H,                 m_palMid);
    oklchToLinear(0.72f + 0.20f * V, Cbase * 0.8f, H + offset * 0.5f, m_palHigh);

    // accent: onset-pop color, hue circularly smoothed too
    float accHtarget = H + (60.0f * (TWO_PI / 360.0f)) * m_spread + (20.0f * (TWO_PI/360.0f)) * (m_warmth - 0.5f);
    float ka = 1.0f - std::exp(-dt / 0.6f);
    m_accHueCos += (std::cos(accHtarget) - m_accHueCos) * ka;
    m_accHueSin += (std::sin(accHtarget) - m_accHueSin) * ka;
    float accH = std::atan2(m_accHueSin, m_accHueCos);
    float beat = m_beatDecay;
    oklchToLinear(0.55f + 0.30f * beat, Cbase * (1.0f + 0.8f * beat), accH, m_palAccent);

    m_palTemp = (m_warmth - 0.5f) * 2.0f;                 // -1..1
    m_palSat  = std::min(std::max(0.2f + 0.6f * m_arousal, 0.0f), 1.0f);
}

// --- Beat detection ---

void AudioAnalyzer::detectBeat(float dt) {
    m_beatThisFrame = false;

    // Update cooldown
    if (m_beatCooldown > 0) {
        m_beatCooldown -= dt;
    }

    // Dual fast/slow follower crossing (Ableton AnalysisGrabber pattern) on
    // the PRE-AGC bass-weighted energy — self-normalizing, no absolute
    // threshold (replaced the naive energy-vs-1.4x-rolling-average trigger).
    float energy = m_bandE[0] * 0.7f + m_bandE[1] * 0.3f;
    m_beatFast += (energy - m_beatFast) * easelaudio::k1(dt, 0.025f);
    m_beatSlow += (energy - m_beatSlow) * easelaudio::k1(dt, 0.35f);

    if (m_beatFast > m_beatSlow * 1.30f + 1e-4f && m_beatCooldown <= 0) {
        m_beatThisFrame = true;
        m_beatDecay = 1.0f;
        m_beatCooldown = 0.15f; // 150ms minimum between beats (unchanged)
    }

    // Exponential decay (~85ms half-life)
    // half-life = ln(2) / rate -> rate = ln(2) / 0.085 ≈ 8.15
    m_beatDecay *= std::exp(-8.15f * dt);
    if (m_beatDecay < 0.001f) m_beatDecay = 0;
}

// --- Smoothing ---

void AudioAnalyzer::smoothBands(float dt) {
    // Asymmetric attack/release envelope (dt-independent exponential).
    // Rates are now user-adjustable via the Audio panel — lower values
    // glide more, higher values snap. Floor at 0.05 so a "0" slider
    // doesn't freeze the value at its current sample.
    const float aRate = std::max(0.05f, m_smoothAttackRate);
    const float rRate = std::max(0.05f, m_smoothReleaseRate);
    auto smooth = [&](float& current, float raw) {
        float rate = (raw > current) ? aRate : rRate;
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
