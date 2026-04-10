#ifdef HAS_FFMPEG
#include "app/VideoRecorder.h"
#include <iostream>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
}

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <propidl.h>
#include <ksmedia.h>
#endif

VideoRecorder::~VideoRecorder() {
    stop();
}

bool VideoRecorder::start(const std::string& path, int width, int height, int fps) {
    stop();

    // Ensure parent directory exists
    std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    if (!initEncoder(path, width, height, fps)) {
        cleanup();
        return false;
    }
    m_path = path;
    m_startTime = av_gettime_relative() / 1000000.0;
    m_stopRequested = false;
    m_active = true;
    m_videoFrameIndex = 0;
    m_audioSamplesWritten = 0;
    m_thread = std::thread(&VideoRecorder::encodeThread, this);
    std::cout << "[REC] Recording to: " << path << std::endl;
    return true;
}

void VideoRecorder::stop() {
    if (m_active) {
        m_stopRequested = true;
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_active = false;
        std::cout << "[REC] Recording saved: " << m_path << std::endl;
    }
    cleanup();
}

double VideoRecorder::uptimeSeconds() const {
    if (!m_active) return 0;
    return av_gettime_relative() / 1000000.0 - m_startTime;
}

// ─── WASAPI audio device enumeration ────────────────────────────────

#ifdef _WIN32
std::vector<RecAudioDevice> VideoRecorder::enumerateAudioDevices() {
    std::vector<RecAudioDevice> result;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&enumerator);
    if (FAILED(hr)) return result;

    // Enumerate render devices (output loopback)
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(collection->Item(i, &dev))) {
                RecAudioDevice info;
                info.isCapture = false;

                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid))) {
                    char buf[512];
                    wcstombs(buf, wid, sizeof(buf));
                    info.id = buf;
                    CoTaskMemFree(wid);
                }

                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
                    PROPERTYKEY key;
                    key.fmtid = {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
                    key.pid = 14;
                    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) {
                        char buf[512];
                        wcstombs(buf, pv.pwszVal, sizeof(buf));
                        info.name = buf;
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (info.name.empty()) info.name = "Output " + std::to_string(i);
                info.name += " (loopback)";
                result.push_back(info);
                dev->Release();
            }
        }
        collection->Release();
    }

    // Enumerate capture devices (microphones)
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(collection->Item(i, &dev))) {
                RecAudioDevice info;
                info.isCapture = true;

                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid))) {
                    char buf[512];
                    wcstombs(buf, wid, sizeof(buf));
                    info.id = buf;
                    CoTaskMemFree(wid);
                }

                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    PROPERTYKEY key;
                    key.fmtid = {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
                    key.pid = 14;
                    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) {
                        char buf[512];
                        wcstombs(buf, pv.pwszVal, sizeof(buf));
                        info.name = buf;
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (info.name.empty()) info.name = "Microphone " + std::to_string(i);
                result.push_back(info);
                dev->Release();
            }
        }
        collection->Release();
    }

    enumerator->Release();
    return result;
}
#else
std::vector<RecAudioDevice> VideoRecorder::enumerateAudioDevices() {
    return {};
}
#endif

// ─── WASAPI capture ─────────────────────────────────────────────────

#ifdef _WIN32
bool VideoRecorder::initAudioCapture() {
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr)) {
        std::cerr << "[REC] Audio: failed to create device enumerator" << std::endl;
        return false;
    }

    IMMDevice* device = nullptr;
    auto devices = enumerateAudioDevices();
    bool isCapture = false;

    if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)devices.size()) {
        auto& info = devices[m_selectedAudioDevice];
        isCapture = info.isCapture;
        int idLen = (int)info.id.size() + 1;
        std::vector<wchar_t> wid(idLen);
        mbstowcs(wid.data(), info.id.c_str(), idLen);
        hr = enumerator->GetDevice(wid.data(), &device);
        std::cout << "[REC] Audio: opening device '" << info.name << "'" << std::endl;
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        std::cout << "[REC] Audio: using default output (loopback)" << std::endl;
    }
    enumerator->Release();
    if (FAILED(hr) || !device) {
        std::cerr << "[REC] Audio: failed to get device (hr=0x" << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }
    m_audioDevice = device;

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        std::cerr << "[REC] Audio: failed to activate IAudioClient (hr=0x" << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }
    m_audioClient = audioClient;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[REC] Audio: GetMixFormat failed" << std::endl;
        return false;
    }

    m_wasapiSampleRate = mixFmt->nSamplesPerSec;
    m_wasapiChannels = mixFmt->nChannels;

    // Verify format is IEEE float
    bool isFloat = false;
    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFmt->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mixFmt;
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    std::cout << "[REC] Audio: format=" << mixFmt->wFormatTag
              << " rate=" << m_wasapiSampleRate
              << " ch=" << m_wasapiChannels
              << " bits=" << mixFmt->wBitsPerSample
              << " float=" << isFloat << std::endl;

    if (!isFloat) {
        std::cerr << "[REC] Audio: device format is not IEEE float — audio may not work correctly" << std::endl;
    }

    // For loopback, use the device's default period for minimum latency
    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    REFERENCE_TIME bufferDuration = defaultPeriod > 0 ? defaultPeriod * 4 : 2000000; // 4x period or 200ms

    DWORD streamFlags = isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  streamFlags,
                                  bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[REC] Audio: Initialize failed (hr=0x" << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) {
        std::cerr << "[REC] Audio: GetService(IAudioCaptureClient) failed" << std::endl;
        return false;
    }
    m_captureClient = captureClient;

    audioClient->Start();
    std::cout << "[REC] Audio capture started successfully" << std::endl;
    return true;
}
#else
bool VideoRecorder::initAudioCapture() {
    std::cerr << "[REC] Audio capture not implemented on this platform" << std::endl;
    return false;
}
#endif

// ─── FFmpeg encoder ─────────────────────────────────────────────────

bool VideoRecorder::initEncoder(const std::string& path, int width, int height, int fps) {
    m_width = width;
    m_height = height;

#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Auto-detect container from extension (mp4, mkv, etc.)
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, path.c_str());
    if (ret < 0 || !m_fmtCtx) {
        std::cerr << "[REC] Failed to create output context" << std::endl;
        return false;
    }

    // ── Video stream (H.264) ──
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) return false;

    m_videoStream = avformat_new_stream(m_fmtCtx, videoCodec);
    m_videoCodecCtx = avcodec_alloc_context3(videoCodec);
    if (!m_videoStream || !m_videoCodecCtx) return false;

    // Ensure even dimensions
    m_width = width & ~1;
    m_height = height & ~1;

    m_videoCodecCtx->width = m_width;
    m_videoCodecCtx->height = m_height;
    m_videoCodecCtx->time_base = {1, fps};
    m_videoCodecCtx->framerate = {fps, 1};
    m_videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_videoCodecCtx->gop_size = fps * 2;
    m_videoCodecCtx->max_b_frames = 2;
    m_videoCodecCtx->bit_rate = 8000000; // 8 Mbps for local recording

    av_opt_set(m_videoCodecCtx->priv_data, "preset", "medium", 0);
    av_opt_set(m_videoCodecCtx->priv_data, "profile", "high", 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_videoCodecCtx, videoCodec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to open video encoder: " << err << std::endl;
        return false;
    }

    avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx);
    m_videoStream->time_base = m_videoCodecCtx->time_base;

    m_videoFrame = av_frame_alloc();
    m_videoFrame->format = AV_PIX_FMT_YUV420P;
    m_videoFrame->width = m_width;
    m_videoFrame->height = m_height;
    av_frame_get_buffer(m_videoFrame, 0);

    m_swsCtx = sws_getContext(m_width, m_height, AV_PIX_FMT_RGBA,
                               m_width, m_height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) return false;

    // ── Audio stream (AAC) ──
    bool hasAudio = initAudioCapture();

    if (hasAudio) {
        const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (audioCodec) {
            m_audioStream = avformat_new_stream(m_fmtCtx, audioCodec);
            m_audioCodecCtx = avcodec_alloc_context3(audioCodec);

            m_audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            m_audioCodecCtx->sample_rate = m_wasapiSampleRate;
            av_channel_layout_default(&m_audioCodecCtx->ch_layout, 2);
            m_audioCodecCtx->bit_rate = 192000;

            if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                m_audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            ret = avcodec_open2(m_audioCodecCtx, audioCodec, nullptr);
            if (ret < 0) {
                avcodec_free_context(&m_audioCodecCtx);
                m_audioCodecCtx = nullptr;
                m_audioStream = nullptr;
                hasAudio = false;
            }
        } else {
            hasAudio = false;
        }

        if (hasAudio) {
            avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx);
            m_audioStream->time_base = {1, m_audioCodecCtx->sample_rate};

            m_audioFrameSize = m_audioCodecCtx->frame_size;

            m_audioFrame = av_frame_alloc();
            m_audioFrame->format = AV_SAMPLE_FMT_FLTP;
            m_audioFrame->sample_rate = m_audioCodecCtx->sample_rate;
            av_channel_layout_copy(&m_audioFrame->ch_layout, &m_audioCodecCtx->ch_layout);
            m_audioFrame->nb_samples = m_audioFrameSize;
            av_frame_get_buffer(m_audioFrame, 0);

            // Setup resampler: WASAPI interleaved float -> AAC planar float
            // Always use actual WASAPI channel count as input
            AVChannelLayout inLayout;
            av_channel_layout_default(&inLayout, m_wasapiChannels);
            ret = swr_alloc_set_opts2(&m_swrCtx,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, m_wasapiSampleRate,
                &inLayout, AV_SAMPLE_FMT_FLT, m_wasapiSampleRate,
                0, nullptr);
            av_channel_layout_uninit(&inLayout);

            if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
                std::cerr << "[REC] Audio: swr_init failed" << std::endl;
                cleanupAudio();
                hasAudio = false;
            } else {
                std::cout << "[REC] Audio encoder ready: AAC " << m_wasapiSampleRate << "Hz, "
                          << m_wasapiChannels << "ch -> stereo" << std::endl;
            }
            m_audioAccum.clear();
        }
    }

    if (!hasAudio) {
        std::cerr << "[REC] WARNING: Recording without audio!" << std::endl;
    }

    // ── Open file and write header ──
    m_packet = av_packet_alloc();

    ret = avio_open(&m_fmtCtx->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to open file: " << err << std::endl;
        return false;
    }

    ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to write header: " << err << std::endl;
        return false;
    }

    m_readbackBuf.resize(m_width * m_height * 4);
    m_encodeBuf.resize(m_width * m_height * 4);

    return true;
}

// ─── GL readback ────────────────────────────────────────────────────

void VideoRecorder::sendFrame(GLuint texture, int w, int h) {
    if (!m_active || w != m_width || h != m_height) return;

    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_readbackBuf.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Flip vertically
    int stride = w * 4;
    for (int y = 0; y < h / 2; y++) {
        uint8_t* top = m_readbackBuf.data() + y * stride;
        uint8_t* bot = m_readbackBuf.data() + (h - 1 - y) * stride;
        for (int x = 0; x < stride; x++) {
            uint8_t tmp = top[x];
            top[x] = bot[x];
            bot[x] = tmp;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::swap(m_readbackBuf, m_encodeBuf);
        m_frameReady = true;
    }
    m_cv.notify_one();
}

// ─── Audio drain ────────────────────────────────────────────────────

void VideoRecorder::drainAudio() {
    if (!m_captureClient || !m_audioCodecCtx) return;

#ifdef _WIN32
    IAudioCaptureClient* capture = (IAudioCaptureClient*)m_captureClient;
    UINT32 packetLength = 0;
    HRESULT hr = capture->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) return;

    while (packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (m_audioSamplesWritten == 0 && numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            std::cout << "[REC] Audio: first non-silent packet (" << numFrames << " frames)" << std::endl;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::vector<float> silence(numFrames * m_wasapiChannels, 0.0f);
            encodeAudioSamples(silence.data(), numFrames, m_wasapiChannels);
        } else if (data) {
            encodeAudioSamples((const float*)data, numFrames, m_wasapiChannels);
        }

        capture->ReleaseBuffer(numFrames);
        hr = capture->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;
    }
#endif
}

void VideoRecorder::encodeAudioSamples(const float* data, int numSamples, int channels) {
    int totalFloats = numSamples * channels;
    m_audioAccum.insert(m_audioAccum.end(), data, data + totalFloats);

    int samplesPerFrame = m_audioFrameSize;
    int floatsPerFrame = samplesPerFrame * channels;

    while ((int)m_audioAccum.size() >= floatsPerFrame) {
        av_frame_make_writable(m_audioFrame);
        m_audioFrame->nb_samples = samplesPerFrame;

        const uint8_t* inData[1] = { (const uint8_t*)m_audioAccum.data() };
        int converted = swr_convert(m_swrCtx,
            m_audioFrame->data, samplesPerFrame,
            inData, samplesPerFrame);

        if (converted > 0) {
            m_audioFrame->pts = m_audioSamplesWritten;
            m_audioSamplesWritten += converted;

            int ret = avcodec_send_frame(m_audioCodecCtx, m_audioFrame);
            while (ret >= 0) {
                ret = avcodec_receive_packet(m_audioCodecCtx, m_packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;
                av_packet_rescale_ts(m_packet,
                    m_audioCodecCtx->time_base, m_audioStream->time_base);
                m_packet->stream_index = m_audioStream->index;
                av_interleaved_write_frame(m_fmtCtx, m_packet);
                av_packet_unref(m_packet);
            }
        }
        m_audioAccum.erase(m_audioAccum.begin(), m_audioAccum.begin() + floatsPerFrame);
    }
}

// ─── Encode thread ──────────────────────────────────────────────────

void VideoRecorder::encodeThread() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    while (!m_stopRequested) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return m_frameReady || m_stopRequested.load();
        });

        bool hasVideo = m_frameReady;
        if (hasVideo) m_frameReady = false;
        lock.unlock();

        // Always drain audio, even on the stop iteration
        drainAudio();
        if (hasVideo) encodeVideoFrame(m_encodeBuf.data());
    }

    // Final audio drain
    drainAudio();

    std::cout << "[REC] Flushing encoders (audio samples written: " << m_audioSamplesWritten << ")" << std::endl;

    // Flush encoders
    if (m_videoCodecCtx) {
        avcodec_send_frame(m_videoCodecCtx, nullptr);
        while (avcodec_receive_packet(m_videoCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_packet->stream_index = m_videoStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }
    if (m_audioCodecCtx) {
        avcodec_send_frame(m_audioCodecCtx, nullptr);
        while (avcodec_receive_packet(m_audioCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_audioCodecCtx->time_base, m_audioStream->time_base);
            m_packet->stream_index = m_audioStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }

    if (m_fmtCtx && m_fmtCtx->pb)
        av_write_trailer(m_fmtCtx);

#ifdef _WIN32
    CoUninitialize();
#endif
}

void VideoRecorder::encodeVideoFrame(const uint8_t* rgbaData) {
    av_frame_make_writable(m_videoFrame);

    const uint8_t* srcSlice[1] = { rgbaData };
    int srcStride[1] = { m_width * 4 };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_height,
              m_videoFrame->data, m_videoFrame->linesize);

    m_videoFrame->pts = m_videoFrameIndex++;

    int ret = avcodec_send_frame(m_videoCodecCtx, m_videoFrame);
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_videoCodecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;
        av_interleaved_write_frame(m_fmtCtx, m_packet);
        av_packet_unref(m_packet);
    }
}

// ─── Cleanup ────────────────────────────────────────────────────────

void VideoRecorder::cleanupAudio() {
#ifdef _WIN32
    if (m_audioClient) {
        ((IAudioClient*)m_audioClient)->Stop();
        ((IAudioClient*)m_audioClient)->Release();
        m_audioClient = nullptr;
    }
    if (m_captureClient) {
        ((IAudioCaptureClient*)m_captureClient)->Release();
        m_captureClient = nullptr;
    }
    if (m_audioDevice) {
        ((IMMDevice*)m_audioDevice)->Release();
        m_audioDevice = nullptr;
    }
#else
    m_audioClient = nullptr;
    m_captureClient = nullptr;
    m_audioDevice = nullptr;
#endif
    if (m_swrCtx) { swr_free(&m_swrCtx); m_swrCtx = nullptr; }
    if (m_audioFrame) { av_frame_free(&m_audioFrame); }
    if (m_audioCodecCtx) { avcodec_free_context(&m_audioCodecCtx); }
    m_audioStream = nullptr;
    m_audioAccum.clear();
}

void VideoRecorder::cleanup() {
    cleanupAudio();
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_videoFrame) { av_frame_free(&m_videoFrame); }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); }
    if (m_fmtCtx) {
        if (m_fmtCtx->pb) avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStream = nullptr;
    m_readbackBuf.clear();
    m_encodeBuf.clear();
    m_frameReady = false;
}

#endif // HAS_FFMPEG
