#ifdef HAS_FFMPEG
#include "app/RTMPOutput.h"
#include <iostream>
#include <cstring>

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

// WASAPI loopback capture
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>

RTMPOutput::~RTMPOutput() {
    stop();
}

bool RTMPOutput::start(const std::string& streamKey, int width, int height,
                       int aspectNum, int aspectDen, int fps) {
    std::string url = "rtmp://a.rtmp.youtube.com/live2/" + streamKey;
    return startCustom(url, width, height, aspectNum, aspectDen, fps);
}

bool RTMPOutput::startCustom(const std::string& url, int width, int height,
                              int aspectNum, int aspectDen, int fps) {
    stop();
    if (!initEncoder(url, width, height, aspectNum, aspectDen, fps)) {
        cleanup();
        return false;
    }
    m_startTime = av_gettime_relative() / 1000000.0;
    m_stopRequested = false;
    m_active = true;
    m_droppedFrames = 0;
    m_videoFrameIndex = 0;
    m_audioSamplesWritten = 0;
    m_thread = std::thread(&RTMPOutput::encodeThread, this);
    std::cout << "[RTMP] Streaming to: " << url << std::endl;
    return true;
}

void RTMPOutput::stop() {
    if (m_active) {
        m_stopRequested = true;
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_active = false;
        std::cout << "[RTMP] Stream stopped. Dropped frames: " << m_droppedFrames << std::endl;
    }
    cleanup();
}

double RTMPOutput::uptimeSeconds() const {
    if (!m_active) return 0;
    return av_gettime_relative() / 1000000.0 - m_startTime;
}

// ─── WASAPI loopback init ───────────────────────────────────────────

bool RTMPOutput::initAudioCapture() {
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to create device enumerator" << std::endl;
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) {
        std::cerr << "[RTMP] No default audio output device" << std::endl;
        return false;
    }
    m_audioDevice = device;

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to activate audio client" << std::endl;
        return false;
    }
    m_audioClient = audioClient;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to get mix format" << std::endl;
        return false;
    }

    m_wasapiSampleRate = mixFmt->nSamplesPerSec;
    m_wasapiChannels = mixFmt->nChannels;
    std::cout << "[RTMP] Audio capture: " << m_wasapiSampleRate << " Hz, "
              << m_wasapiChannels << " ch" << std::endl;

    // Initialize in loopback mode (capture system audio output)
    // Use 50ms buffer — enough headroom, low latency
    REFERENCE_TIME bufferDuration = 500000; // 50ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to initialize loopback capture (0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to get capture client" << std::endl;
        return false;
    }
    m_captureClient = captureClient;

    hr = audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Failed to start audio capture" << std::endl;
        return false;
    }

    return true;
}

// ─── FFmpeg encoder init ────────────────────────────────────────────

bool RTMPOutput::initEncoder(const std::string& url, int width, int height,
                              int aspectNum, int aspectDen, int fps) {
    m_width = width;
    m_height = height;

    // Compute center crop to target aspect ratio
    if (aspectNum > 0 && aspectDen > 0) {
        float targetAspect = (float)aspectNum / (float)aspectDen;
        float sourceAspect = (float)width / (float)height;

        if (sourceAspect > targetAspect) {
            // Source is wider — crop sides
            m_cropH = height;
            m_cropW = (int)(height * targetAspect);
            m_cropX = (width - m_cropW) / 2;
            m_cropY = 0;
        } else {
            // Source is taller — crop top/bottom
            m_cropW = width;
            m_cropH = (int)(width / targetAspect);
            m_cropX = 0;
            m_cropY = (height - m_cropH) / 2;
        }
        // Ensure even dimensions
        m_cropW &= ~1;
        m_cropH &= ~1;
        m_cropX &= ~1;
        m_cropY &= ~1;
    } else {
        m_cropX = 0;
        m_cropY = 0;
        m_cropW = width;
        m_cropH = height;
    }

    // Pick the nearest standard resolution that fits the cropped content
    struct StdRes { int w, h; };
    static const StdRes std16x9[]  = {{3840,2160},{2560,1440},{1920,1080},{1280,720},{854,480}};
    static const StdRes std4x3[]   = {{1600,1200},{1280,960},{1024,768},{800,600}};
    static const StdRes std16x10[] = {{1920,1200},{1680,1050},{1280,800}};

    const StdRes* table = std16x9;
    int tableSize = 5;
    if (aspectNum == 4 && aspectDen == 3)       { table = std4x3; tableSize = 4; }
    else if (aspectNum == 16 && aspectDen == 10) { table = std16x10; tableSize = 3; }

    m_encWidth = m_cropW;
    m_encHeight = m_cropH;
    for (int i = 0; i < tableSize; i++) {
        if (table[i].w <= m_cropW) {
            m_encWidth = table[i].w;
            m_encHeight = table[i].h;
            break;
        }
    }
    m_encWidth &= ~1;
    m_encHeight &= ~1;

    std::cout << "[RTMP] Source " << width << "x" << height
              << " -> crop " << m_cropW << "x" << m_cropH
              << " -> encode " << m_encWidth << "x" << m_encHeight << std::endl;

    // Initialize COM for WASAPI (this thread)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Allocate output format context for RTMP/FLV
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "flv", url.c_str());
    if (ret < 0 || !m_fmtCtx) {
        std::cerr << "[RTMP] Failed to create output context" << std::endl;
        return false;
    }

    // ── Video stream (H.264) ──
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) {
        std::cerr << "[RTMP] H.264 encoder not found" << std::endl;
        return false;
    }

    m_videoStream = avformat_new_stream(m_fmtCtx, videoCodec);
    if (!m_videoStream) return false;

    m_videoCodecCtx = avcodec_alloc_context3(videoCodec);
    if (!m_videoCodecCtx) return false;

    m_videoCodecCtx->width = m_encWidth;
    m_videoCodecCtx->height = m_encHeight;
    m_videoCodecCtx->time_base = {1, fps};
    m_videoCodecCtx->framerate = {fps, 1};
    m_videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_videoCodecCtx->gop_size = fps * 2;
    m_videoCodecCtx->max_b_frames = 2;
    m_videoCodecCtx->bit_rate = 4500000;

    av_opt_set(m_videoCodecCtx->priv_data, "preset", "veryfast", 0);
    av_opt_set(m_videoCodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_videoCodecCtx->priv_data, "profile", "high", 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_videoCodecCtx, videoCodec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[RTMP] Failed to open video encoder: " << err << std::endl;
        return false;
    }

    avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx);
    m_videoStream->time_base = m_videoCodecCtx->time_base;

    // Video frame + converter (handles both colorspace conversion and scaling)
    m_videoFrame = av_frame_alloc();
    m_videoFrame->format = AV_PIX_FMT_YUV420P;
    m_videoFrame->width = m_encWidth;
    m_videoFrame->height = m_encHeight;
    av_frame_get_buffer(m_videoFrame, 0);

    m_swsCtx = sws_getContext(m_cropW, m_cropH, AV_PIX_FMT_RGBA,
                               m_encWidth, m_encHeight, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) return false;

    // ── Audio stream (AAC) ──
    bool hasAudio = initAudioCapture();

    if (hasAudio) {
        const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audioCodec) {
            std::cerr << "[RTMP] AAC encoder not found, streaming without audio" << std::endl;
            hasAudio = false;
        }

        if (hasAudio) {
            m_audioStream = avformat_new_stream(m_fmtCtx, audioCodec);
            m_audioCodecCtx = avcodec_alloc_context3(audioCodec);

            m_audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AAC wants planar float
            m_audioCodecCtx->sample_rate = m_wasapiSampleRate;
            av_channel_layout_default(&m_audioCodecCtx->ch_layout, 2); // stereo output
            m_audioCodecCtx->bit_rate = 128000;

            if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                m_audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            ret = avcodec_open2(m_audioCodecCtx, audioCodec, nullptr);
            if (ret < 0) {
                char err[128]; av_strerror(ret, err, sizeof(err));
                std::cerr << "[RTMP] Failed to open AAC encoder: " << err
                          << " — streaming without audio" << std::endl;
                avcodec_free_context(&m_audioCodecCtx);
                m_audioCodecCtx = nullptr;
                m_audioStream = nullptr;
                hasAudio = false;
            }
        }

        if (hasAudio) {
            avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx);
            m_audioStream->time_base = {1, m_audioCodecCtx->sample_rate};

            m_audioFrameSize = m_audioCodecCtx->frame_size; // samples per AAC frame (1024)

            // Audio frame buffer
            m_audioFrame = av_frame_alloc();
            m_audioFrame->format = AV_SAMPLE_FMT_FLTP;
            m_audioFrame->sample_rate = m_audioCodecCtx->sample_rate;
            av_channel_layout_copy(&m_audioFrame->ch_layout, &m_audioCodecCtx->ch_layout);
            m_audioFrame->nb_samples = m_audioFrameSize;
            av_frame_get_buffer(m_audioFrame, 0);

            // Resampler: WASAPI float32 interleaved (N channels) -> AAC float32 planar stereo
            ret = swr_alloc_set_opts2(&m_swrCtx,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, m_wasapiSampleRate,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLT,  m_wasapiSampleRate,
                0, nullptr);

            // If WASAPI has more than 2 channels, we need to set the input layout properly
            if (m_wasapiChannels != 2) {
                AVChannelLayout inLayout;
                av_channel_layout_default(&inLayout, m_wasapiChannels);
                swr_alloc_set_opts2(&m_swrCtx,
                    &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, m_wasapiSampleRate,
                    &inLayout, AV_SAMPLE_FMT_FLT, m_wasapiSampleRate,
                    0, nullptr);
                av_channel_layout_uninit(&inLayout);
            }

            if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
                std::cerr << "[RTMP] Failed to init resampler — streaming without audio" << std::endl;
                cleanupAudio();
                hasAudio = false;
            }

            m_audioAccum.clear();
        }
    }

    // ── Open RTMP connection and write header ──
    m_packet = av_packet_alloc();

    ret = avio_open(&m_fmtCtx->pb, url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[RTMP] Failed to open URL: " << err << std::endl;
        return false;
    }

    ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[RTMP] Failed to write header: " << err << std::endl;
        return false;
    }

    // Allocate readback buffers
    m_readbackBuf.resize(width * height * 4);
    m_encodeBuf.resize(width * height * 4);

    return true;
}

// ─── GL readback (main thread) ──────────────────────────────────────

void RTMPOutput::sendFrame(GLuint texture, int w, int h) {
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

    // Extract cropped region into encode buffer
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frameReady) m_droppedFrames++;

        int srcStride = w * 4;
        int dstStride = m_cropW * 4;
        m_encodeBuf.resize(m_cropW * m_cropH * 4);
        for (int row = 0; row < m_cropH; row++) {
            const uint8_t* src = m_readbackBuf.data() + (m_cropY + row) * srcStride + m_cropX * 4;
            uint8_t* dst = m_encodeBuf.data() + row * dstStride;
            std::memcpy(dst, src, dstStride);
        }
        m_frameReady = true;
    }
    m_cv.notify_one();
}

// ─── Audio drain (called from encode thread) ────────────────────────

void RTMPOutput::drainAudio() {
    if (!m_captureClient || !m_audioCodecCtx) return;

    IAudioCaptureClient* capture = (IAudioCaptureClient*)m_captureClient;

    UINT32 packetLength = 0;
    capture->GetNextPacketSize(&packetLength);

    while (packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        HRESULT hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // Insert silence
            std::vector<float> silence(numFrames * m_wasapiChannels, 0.0f);
            encodeAudioSamples(silence.data(), numFrames, m_wasapiChannels);
        } else {
            encodeAudioSamples((const float*)data, numFrames, m_wasapiChannels);
        }

        capture->ReleaseBuffer(numFrames);
        capture->GetNextPacketSize(&packetLength);
    }
}

void RTMPOutput::encodeAudioSamples(const float* data, int numSamples, int channels) {
    // Accumulate interleaved float samples, encode when we have a full AAC frame
    int totalFloats = numSamples * channels;
    m_audioAccum.insert(m_audioAccum.end(), data, data + totalFloats);

    // Process complete frames (m_audioFrameSize samples * channels)
    int samplesPerFrame = m_audioFrameSize; // in sample-sets (not individual floats)
    int floatsPerFrame = samplesPerFrame * channels;

    while ((int)m_audioAccum.size() >= floatsPerFrame) {
        av_frame_make_writable(m_audioFrame);
        m_audioFrame->nb_samples = samplesPerFrame;

        // Convert interleaved float -> planar float via swresample
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

        // Remove consumed samples
        m_audioAccum.erase(m_audioAccum.begin(), m_audioAccum.begin() + floatsPerFrame);
    }
}

// ─── Encode thread ──────────────────────────────────────────────────

void RTMPOutput::encodeThread() {
    // COM init for this thread (WASAPI calls happen here via drainAudio)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!m_stopRequested) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(20), [this] {
            return m_frameReady || m_stopRequested.load();
        });

        if (m_stopRequested) break;

        bool hasVideo = m_frameReady;
        if (hasVideo) m_frameReady = false;
        lock.unlock();

        // Always drain audio, even if no video frame this tick
        drainAudio();

        if (hasVideo) {
            encodeVideoFrame(m_encodeBuf.data(), m_width, m_height);
        }
    }

    // Flush video encoder
    if (m_videoCodecCtx) {
        avcodec_send_frame(m_videoCodecCtx, nullptr);
        while (avcodec_receive_packet(m_videoCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_packet->stream_index = m_videoStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }

    // Flush audio encoder
    if (m_audioCodecCtx) {
        avcodec_send_frame(m_audioCodecCtx, nullptr);
        while (avcodec_receive_packet(m_audioCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_audioCodecCtx->time_base, m_audioStream->time_base);
            m_packet->stream_index = m_audioStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }

    if (m_fmtCtx && m_fmtCtx->pb) {
        av_write_trailer(m_fmtCtx);
    }

    CoUninitialize();
}

void RTMPOutput::encodeVideoFrame(const uint8_t* rgbaData, int /*w*/, int /*h*/) {
    av_frame_make_writable(m_videoFrame);

    const uint8_t* srcSlice[1] = { rgbaData };
    int srcStride[1] = { m_cropW * 4 };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_cropH,
              m_videoFrame->data, m_videoFrame->linesize);

    m_videoFrame->pts = m_videoFrameIndex++;

    int ret = avcodec_send_frame(m_videoCodecCtx, m_videoFrame);
    if (ret < 0) return;

    while (ret >= 0) {
        ret = avcodec_receive_packet(m_videoCodecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;

        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            std::cerr << "[RTMP] Write error — connection lost?" << std::endl;
            m_stopRequested = true;
            break;
        }
        av_packet_unref(m_packet);
    }
}

// ─── Cleanup ────────────────────────────────────────────────────────

void RTMPOutput::cleanupAudio() {
    // Stop and release WASAPI
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

    if (m_swrCtx) { swr_free(&m_swrCtx); m_swrCtx = nullptr; }
    if (m_audioFrame) { av_frame_free(&m_audioFrame); }
    if (m_audioCodecCtx) { avcodec_free_context(&m_audioCodecCtx); }
    m_audioStream = nullptr;
    m_audioAccum.clear();
}

void RTMPOutput::cleanup() {
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
