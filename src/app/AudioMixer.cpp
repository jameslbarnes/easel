#include "AudioMixer.h"
#include "VideoRecorder.h" // for RecAudioDevice, enumerateAudioDevices

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h> // for KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#endif

#ifdef HAS_FFMPEG
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}
#endif

// ============================================================
// AudioMixerInput
// ============================================================

bool AudioMixerInput::init(int outputSampleRate, int outputChannels) {
#ifdef _WIN32
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        std::cerr << "[AudioMixer] Input '" << name << "': CoCreateInstance failed" << std::endl;
        return false;
    }

    IMMDevice* dev = nullptr;
    bool isLoopback = !isCapture;

    if (deviceId.empty()) {
        // Default system audio (render endpoint for loopback)
        if (isCapture) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
        } else {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        }
    } else {
        std::wstring wid(deviceId.begin(), deviceId.end());
        hr = enumerator->GetDevice(wid.c_str(), &dev);
    }
    enumerator->Release();

    if (FAILED(hr) || !dev) {
        std::cerr << "[AudioMixer] Input '" << name << "': no device found" << std::endl;
        return false;
    }
    device = dev;

    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr) || !ac) {
        std::cerr << "[AudioMixer] Input '" << name << "': IAudioClient activation failed" << std::endl;
        return false;
    }
    audioClient = ac;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = ac->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        std::cerr << "[AudioMixer] Input '" << name << "': GetMixFormat failed" << std::endl;
        return false;
    }

    sampleRate = mixFmt->nSamplesPerSec;
    channels = mixFmt->nChannels;

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    ac->GetDevicePeriod(&defaultPeriod, &minPeriod);
    // Use 2x default period for lower latency (typically ~20ms)
    REFERENCE_TIME bufferDuration = defaultPeriod > 0 ? defaultPeriod * 2 : 200000;

    DWORD streamFlags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                        bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[AudioMixer] Input '" << name << "': Initialize failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }

    IAudioCaptureClient* cc = nullptr;
    hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc);
    if (FAILED(hr) || !cc) {
        std::cerr << "[AudioMixer] Input '" << name << "': GetService(CaptureClient) failed" << std::endl;
        return false;
    }
    captureClient = cc;

    // Set up resampler if sample rate or channel count differs from output
#ifdef HAS_FFMPEG
    if (sampleRate != outputSampleRate || channels != outputChannels) {
        AVChannelLayout inLayout, outLayout;
        av_channel_layout_default(&inLayout, channels);
        av_channel_layout_default(&outLayout, outputChannels);

        int ret = swr_alloc_set_opts2(&swrCtx,
            &outLayout, AV_SAMPLE_FMT_FLT, outputSampleRate,
            &inLayout,  AV_SAMPLE_FMT_FLT, sampleRate,
            0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        if (ret < 0 || !swrCtx || swr_init(swrCtx) < 0) {
            std::cerr << "[AudioMixer] Input '" << name << "': resampler init failed" << std::endl;
            if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
            // Continue without resampling — audio may sound wrong but won't crash
        } else {
            std::cout << "[AudioMixer] Input '" << name << "': resampler "
                      << sampleRate << "Hz/" << channels << "ch -> "
                      << outputSampleRate << "Hz/" << outputChannels << "ch" << std::endl;
        }
    }
#else
    if (sampleRate != outputSampleRate) {
        std::cerr << "[AudioMixer] Input '" << name << "': sample rate mismatch ("
                  << sampleRate << " vs " << outputSampleRate
                  << ") but no FFmpeg resampler available" << std::endl;
    }
#endif

    // Allocate ring buffer: 1 second at output rate, stereo
    ringSize = outputSampleRate * outputChannels; // 1 second
    ringBuf.resize(ringSize, 0.0f);
    writePos = 0;
    readPos = 0;

    ac->Start();
    initialized = true;
    std::cout << "[AudioMixer] Input '" << name << "' started ("
              << sampleRate << "Hz, " << channels << "ch"
              << (isLoopback ? ", loopback" : ", capture") << ")" << std::endl;
    return true;
#else
    return false;
#endif
}

void AudioMixerInput::cleanup() {
#ifdef _WIN32
    if (audioClient) { audioClient->Stop(); audioClient->Release(); audioClient = nullptr; }
    if (captureClient) { captureClient->Release(); captureClient = nullptr; }
    if (device) { device->Release(); device = nullptr; }
#endif
#ifdef HAS_FFMPEG
    if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
#endif
    initialized = false;
    ringBuf.clear();
    writePos = readPos = ringSize = 0;
}

void AudioMixerInput::drain(int outputSampleRate, int outputChannels) {
#ifdef _WIN32
    if (!captureClient) return;

    UINT32 packetLength = 0;
    while (SUCCEEDED(captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        HRESULT hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        const float* samples = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : (const float*)data;

        // Prepare source buffer (silence if flagged)
        std::vector<float> silenceBuf;
        if (!samples) {
            silenceBuf.resize(numFrames * channels, 0.0f);
            samples = silenceBuf.data();
        }

#ifdef HAS_FFMPEG
        if (swrCtx) {
            // Resample to output format
            int outSamples = swr_get_out_samples(swrCtx, numFrames);
            std::vector<float> resampled(outSamples * outputChannels);
            uint8_t* outPtr = (uint8_t*)resampled.data();
            const uint8_t* inPtr = (const uint8_t*)samples;
            int converted = swr_convert(swrCtx, &outPtr, outSamples, &inPtr, numFrames);
            if (converted > 0) {
                int totalSamples = converted * outputChannels;
                for (int i = 0; i < totalSamples; i++) {
                    ringBuf[writePos] = resampled[i];
                    writePos = (writePos + 1) % ringSize;
                }
            }
        } else
#endif
        {
            // No resampling needed — copy directly
            // If channel count differs but no resampler, do simple mono→stereo or average→mono
            if (channels == outputChannels) {
                for (UINT32 i = 0; i < numFrames * (UINT32)channels; i++) {
                    ringBuf[writePos] = samples[i];
                    writePos = (writePos + 1) % ringSize;
                }
            } else {
                // Fallback: average all input channels to mono, duplicate to output channels
                for (UINT32 f = 0; f < numFrames; f++) {
                    float mono = 0;
                    for (int ch = 0; ch < channels; ch++)
                        mono += samples[f * channels + ch];
                    mono /= channels;
                    for (int ch = 0; ch < outputChannels; ch++) {
                        ringBuf[writePos] = mono;
                        writePos = (writePos + 1) % ringSize;
                    }
                }
            }
        }

        captureClient->ReleaseBuffer(numFrames);
    }
#endif
}

size_t AudioMixerInput::available() const {
    if (ringSize == 0) return 0;
    return (writePos >= readPos) ? (writePos - readPos) : (ringSize - readPos + writePos);
}

// ============================================================
// AudioMixerOutput
// ============================================================

bool AudioMixerOutput::init() {
#ifdef _WIN32
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        std::cerr << "[AudioMixer] Output: CoCreateInstance failed" << std::endl;
        return false;
    }

    IMMDevice* dev = nullptr;
    if (deviceId.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    } else {
        std::wstring wid(deviceId.begin(), deviceId.end());
        hr = enumerator->GetDevice(wid.c_str(), &dev);
    }
    enumerator->Release();

    if (FAILED(hr) || !dev) {
        std::cerr << "[AudioMixer] Output '" << name << "': no device found" << std::endl;
        return false;
    }
    device = dev;

    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr) || !ac) {
        std::cerr << "[AudioMixer] Output: IAudioClient activation failed" << std::endl;
        return false;
    }
    audioClient = ac;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = ac->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        std::cerr << "[AudioMixer] Output: GetMixFormat failed" << std::endl;
        return false;
    }

    sampleRate = mixFmt->nSamplesPerSec;
    channels = mixFmt->nChannels;

    // Validate format is float32 (WASAPI shared mode should always be float)
    bool isFloat = false;
    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFmt->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mixFmt;
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    if (!isFloat) {
        std::cerr << "[AudioMixer] Output: device format is not IEEE float (tag=0x"
                  << std::hex << mixFmt->wFormatTag << std::dec
                  << ") — audio may not work" << std::endl;
    }

    // Initialize in shared mode — use minimal buffer for low latency
    REFERENCE_TIME bufferDuration = 200000; // 20ms in 100ns units
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                        bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[AudioMixer] Output: Initialize failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }

    UINT32 bufFrames = 0;
    ac->GetBufferSize(&bufFrames);
    bufferFrames = bufFrames;

    IAudioRenderClient* rc = nullptr;
    hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    if (FAILED(hr) || !rc) {
        std::cerr << "[AudioMixer] Output: GetService(RenderClient) failed" << std::endl;
        return false;
    }
    renderClient = rc;

    // Pre-fill a small amount of silence (one period) to avoid startup glitch
    {
        REFERENCE_TIME defPeriod = 0, minP = 0;
        ac->GetDevicePeriod(&defPeriod, &minP);
        UINT32 silenceFrames = (UINT32)(sampleRate * defPeriod / 10000000.0);
        if (silenceFrames > bufFrames) silenceFrames = bufFrames;
        if (silenceFrames > 0) {
            BYTE* buf = nullptr;
            hr = rc->GetBuffer(silenceFrames, &buf);
            if (SUCCEEDED(hr) && buf) {
                std::memset(buf, 0, silenceFrames * channels * sizeof(float));
                rc->ReleaseBuffer(silenceFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
    }

    ac->Start();
    initialized = true;
    std::cout << "[AudioMixer] Output '" << name << "' started ("
              << sampleRate << "Hz, " << channels << "ch, buf=" << bufFrames << " frames)" << std::endl;
    return true;
#else
    return false;
#endif
}

void AudioMixerOutput::cleanup() {
#ifdef _WIN32
    if (audioClient) { audioClient->Stop(); audioClient->Release(); audioClient = nullptr; }
    if (renderClient) { renderClient->Release(); renderClient = nullptr; }
    if (device) { device->Release(); device = nullptr; }
#endif
    initialized = false;
}

void AudioMixerOutput::feed(const float* mixedData, size_t frames) {
#ifdef _WIN32
    if (!renderClient || frames == 0) return;

    BYTE* buffer = nullptr;
    HRESULT hr = renderClient->GetBuffer((UINT32)frames, &buffer);
    if (FAILED(hr) || !buffer) return;

    std::memcpy(buffer, mixedData, frames * channels * sizeof(float));
    renderClient->ReleaseBuffer((UINT32)frames, 0);
#endif
}

// ============================================================
// AudioMixer
// ============================================================

AudioMixer::~AudioMixer() {
    stop();
}

int AudioMixer::addInput(const std::string& deviceId, const std::string& name, bool isCapture) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingAdds.push_back({deviceId, name, isCapture});
    return m_nextInputId++;
}

void AudioMixer::removeInput(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingRemoves.push_back(index);
}

int AudioMixer::inputCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_inputs.size();
}

void AudioMixer::setInputVolume(int index, float vol) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 && index < (int)m_inputs.size())
        m_inputs[index]->volume = vol;
}

float AudioMixer::inputVolume(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 && index < (int)m_inputs.size())
        return m_inputs[index]->volume;
    return 0;
}

void AudioMixer::setInputMuted(int index, bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 && index < (int)m_inputs.size())
        m_inputs[index]->muted = muted;
}

bool AudioMixer::isInputMuted(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 && index < (int)m_inputs.size())
        return m_inputs[index]->muted;
    return false;
}

std::string AudioMixer::inputName(int index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 && index < (int)m_inputs.size())
        return m_inputs[index]->name;
    return "";
}

void AudioMixer::setOutputDevice(const std::string& deviceId, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingOutputDeviceId = deviceId;
    m_pendingOutputDeviceName = name;
    m_outputChanged = true;
}

std::string AudioMixer::outputDeviceId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_output.deviceId;
}

std::string AudioMixer::outputDeviceName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_output.name;
}

void AudioMixer::setMasterVolume(float vol) { m_masterVolume.store(vol); }
float AudioMixer::masterVolume() const { return m_masterVolume.load(); }

void AudioMixer::setNDIAudioEnabled(bool enabled) { m_ndiAudioEnabled.store(enabled); }

bool AudioMixer::hasLocalOutput() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_output.deviceId != "__none__";
}

void AudioMixer::start() {
    if (m_running.load()) return;
    m_stopRequested.store(false);
    m_thread = std::thread(&AudioMixer::mixerThread, this);
}

void AudioMixer::stop() {
    if (!m_running.load() && !m_thread.joinable()) return;
    m_stopRequested.store(true);
    if (m_thread.joinable()) m_thread.join();
}

int AudioMixer::drainMixedMono(float* buffer, int maxSamples) {
    size_t rp = m_analyzerReadPos.load(std::memory_order_relaxed);
    size_t wp = m_analyzerWritePos.load(std::memory_order_acquire);

    size_t avail = (wp >= rp) ? (wp - rp) : (kAnalyzerRingSize - rp + wp);
    int toCopy = (int)std::min(avail, (size_t)maxSamples);

    for (int i = 0; i < toCopy; i++) {
        buffer[i] = m_analyzerRing[rp];
        rp = (rp + 1) % kAnalyzerRingSize;
    }
    m_analyzerReadPos.store(rp, std::memory_order_release);
    return toCopy;
}

// --- Mixer thread ---

void AudioMixer::mixerThread() {
#ifdef _WIN32
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioMixer] Thread COM init failed" << std::endl;
        m_running.store(false);
        return;
    }
#endif

    m_running.store(true);

    try {
        // Initialize output device (unless "none" selected)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_output.deviceId.empty() && !m_outputChanged) {
                m_output.name = "Default Output";
            }
        }
        if (m_output.deviceId == "__none__") {
            m_mixSampleRate = 48000;
            m_mixChannels = 2;
            std::cout << "[AudioMixer] No local output (NDI only mode)" << std::endl;
        } else if (m_output.init()) {
            m_mixSampleRate = m_output.sampleRate;
            m_mixChannels = m_output.channels;
        } else {
            std::cerr << "[AudioMixer] Failed to initialize output — using defaults" << std::endl;
            m_mixSampleRate = 48000;
            m_mixChannels = 2;
        }

        // Apply any inputs that were added before start()
        applyPendingChanges();

        while (!m_stopRequested.load()) {
            applyPendingChanges();

            // NDI audio send lifecycle
#ifdef HAS_NDI
            if (m_ndiAudioEnabled.load() && !m_ndiSend) {
                auto& rt = NDIRuntime::instance();
                if (rt.isAvailable() && rt.api()->send_create) {
                    NDIlib_send_create_t sc = {};
                    sc.p_ndi_name = "Easel Audio";
                    sc.clock_video = false;
                    sc.clock_audio = false;
                    m_ndiSend = rt.api()->send_create(&sc);
                    if (m_ndiSend)
                        std::cout << "[AudioMixer] NDI audio sender created" << std::endl;
                }
            } else if (!m_ndiAudioEnabled.load() && m_ndiSend) {
                auto& rt = NDIRuntime::instance();
                if (rt.isAvailable()) rt.api()->send_destroy(m_ndiSend);
                m_ndiSend = nullptr;
                std::cout << "[AudioMixer] NDI audio sender destroyed" << std::endl;
            }
#endif

            // Drain all inputs
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& input : m_inputs) {
                    if (input->initialized) {
                        input->drain(m_mixSampleRate, m_mixChannels);
                    }
                }
            }

            // Mix and output
            mixAndOutput();

            // Sleep ~3ms for low-latency polling
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    } catch (const std::exception& e) {
        std::cerr << "[AudioMixer] Thread exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[AudioMixer] Thread unknown exception" << std::endl;
    }

    // Cleanup
#ifdef HAS_NDI
    if (m_ndiSend) {
        auto& rt = NDIRuntime::instance();
        if (rt.isAvailable()) rt.api()->send_destroy(m_ndiSend);
        m_ndiSend = nullptr;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& input : m_inputs) input->cleanup();
        m_inputs.clear();
    }
    m_output.cleanup();

#ifdef _WIN32
    CoUninitialize();
#endif
    m_running.store(false);
}

void AudioMixer::applyPendingChanges() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Handle output device change
    if (m_outputChanged) {
        m_output.cleanup();
        m_output.deviceId = m_pendingOutputDeviceId;
        m_output.name = m_pendingOutputDeviceName;

        if (m_output.deviceId == "__none__") {
            m_mixSampleRate = 48000;
            m_mixChannels = 2;
        } else if (m_output.init()) {
            m_mixSampleRate = m_output.sampleRate;
            m_mixChannels = m_output.channels;
        }

        // Reinit all inputs (resamplers depend on output rate)
        for (auto& input : m_inputs) {
            std::string savedId = input->deviceId;
            std::string savedName = input->name;
            bool savedIsCapture = input->isCapture;
            float savedVol = input->volume;
            bool savedMuted = input->muted;
            input->cleanup();
            input->deviceId = savedId;
            input->name = savedName;
            input->isCapture = savedIsCapture;
            input->volume = savedVol;
            input->muted = savedMuted;
            input->init(m_mixSampleRate, m_mixChannels);
        }
        m_outputChanged = false;
    }

    // Remove inputs (process in reverse order to keep indices stable)
    if (!m_pendingRemoves.empty()) {
        std::sort(m_pendingRemoves.begin(), m_pendingRemoves.end(), std::greater<int>());
        for (int idx : m_pendingRemoves) {
            if (idx >= 0 && idx < (int)m_inputs.size()) {
                m_inputs[idx]->cleanup();
                m_inputs.erase(m_inputs.begin() + idx);
            }
        }
        m_pendingRemoves.clear();
    }

    // Add new inputs
    for (auto& pending : m_pendingAdds) {
        auto input = std::make_unique<AudioMixerInput>();
        input->deviceId = pending.deviceId;
        input->name = pending.name;
        input->isCapture = pending.isCapture;

        if (m_output.initialized || m_output.deviceId == "__none__") {
            input->init(m_mixSampleRate, m_mixChannels);
        }
        m_inputs.push_back(std::move(input));
    }
    m_pendingAdds.clear();
}

void AudioMixer::mixAndOutput() {
    int available = 0;
    int outChannels = m_mixChannels;

    if (m_output.initialized) {
        // Use WASAPI output buffer for timing
        uint32_t padding = 0;
#ifdef _WIN32
        if (m_output.audioClient)
            m_output.audioClient->GetCurrentPadding(&padding);
#endif
        available = m_output.bufferFrames - padding;
    } else {
        // No local output — use clock-based frame counting
        auto now = std::chrono::steady_clock::now();
        if (m_lastMixTime.time_since_epoch().count() == 0) {
            m_lastMixTime = now;
            return;
        }
        double elapsed = std::chrono::duration<double>(now - m_lastMixTime).count();
        m_lastMixTime = now;
        available = (int)(elapsed * m_mixSampleRate);
    }

    if (available <= 0) return;

    int totalSamples = available * outChannels;

    // Resize scratch buffer if needed
    if ((int)m_mixBuf.size() < totalSamples)
        m_mixBuf.resize(totalSamples, 0.0f);

    // Zero the mix buffer
    std::memset(m_mixBuf.data(), 0, totalSamples * sizeof(float));

    // Sum all inputs
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& input : m_inputs) {
            if (!input->initialized || input->muted) continue;

            float vol = input->volume;
            size_t inputAvail = input->available();
            size_t samplesToRead = std::min(inputAvail, (size_t)totalSamples);

            for (size_t i = 0; i < samplesToRead; i++) {
                m_mixBuf[i] += input->ringBuf[input->readPos] * vol;
                input->readPos = (input->readPos + 1) % input->ringSize;
            }
        }
    }

    // Apply master volume and clamp
    float master = m_masterVolume.load();
    for (int i = 0; i < totalSamples; i++) {
        m_mixBuf[i] *= master;
        m_mixBuf[i] = std::max(-1.0f, std::min(1.0f, m_mixBuf[i]));
    }

    // Feed to local output device (if active)
    if (m_output.initialized) {
        m_output.feed(m_mixBuf.data(), available);
    }

    // Send via NDI if enabled (deinterleave to planar float)
#ifdef HAS_NDI
    if (m_ndiSend && available > 0 && m_mixSampleRate > 0) {
        auto& rt = NDIRuntime::instance();
        if (rt.isAvailable() && rt.api()->send_send_audio_v2) {
            // Deinterleave: interleaved [L0,R0,L1,R1,...] → planar [L0,L1,...,R0,R1,...]
            m_ndiPlanarBuf.resize(available * outChannels);
            for (int ch = 0; ch < outChannels; ch++) {
                for (int f = 0; f < available; f++) {
                    m_ndiPlanarBuf[ch * available + f] = m_mixBuf[f * outChannels + ch];
                }
            }

            NDIlib_audio_frame_v2_t audioFrame = {};
            audioFrame.sample_rate = m_mixSampleRate;
            audioFrame.no_channels = outChannels;
            audioFrame.no_samples = available;
            audioFrame.p_data = m_ndiPlanarBuf.data();
            audioFrame.channel_stride_in_bytes = available * (int)sizeof(float);

            rt.api()->send_send_audio_v2(m_ndiSend, &audioFrame);
        }
    }
#endif

    // Downmix to mono and write to analyzer ring (SPSC: we are the only writer)
    size_t wp = m_analyzerWritePos.load(std::memory_order_relaxed);
    for (int f = 0; f < available; f++) {
        float mono = 0;
        for (int ch = 0; ch < outChannels; ch++)
            mono += m_mixBuf[f * outChannels + ch];
        mono /= outChannels;

        m_analyzerRing[wp] = mono;
        wp = (wp + 1) % kAnalyzerRingSize;
    }
    m_analyzerWritePos.store(wp, std::memory_order_release);
}
