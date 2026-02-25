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

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>

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

// ─── WASAPI loopback ────────────────────────────────────────────────

bool VideoRecorder::initAudioCapture() {
    HRESULT hr;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr)) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) return false;
    m_audioDevice = device;

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) return false;
    m_audioClient = audioClient;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) return false;

    m_wasapiSampleRate = mixFmt->nSamplesPerSec;
    m_wasapiChannels = mixFmt->nChannels;

    REFERENCE_TIME bufferDuration = 500000;
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) return false;

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr)) return false;
    m_captureClient = captureClient;

    audioClient->Start();
    return true;
}

// ─── FFmpeg encoder ─────────────────────────────────────────────────

bool VideoRecorder::initEncoder(const std::string& path, int width, int height, int fps) {
    m_width = width;
    m_height = height;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

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

            ret = swr_alloc_set_opts2(&m_swrCtx,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, m_wasapiSampleRate,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLT,  m_wasapiSampleRate,
                0, nullptr);

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
                cleanupAudio();
                hasAudio = false;
            }
            m_audioAccum.clear();
        }
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
            std::vector<float> silence(numFrames * m_wasapiChannels, 0.0f);
            encodeAudioSamples(silence.data(), numFrames, m_wasapiChannels);
        } else {
            encodeAudioSamples((const float*)data, numFrames, m_wasapiChannels);
        }

        capture->ReleaseBuffer(numFrames);
        capture->GetNextPacketSize(&packetLength);
    }
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

        drainAudio();
        if (hasVideo) encodeVideoFrame(m_encodeBuf.data());
    }

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

    CoUninitialize();
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
