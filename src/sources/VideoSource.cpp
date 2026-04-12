#include "sources/VideoSource.h"
#include <iostream>
#include <GLFW/glfw3.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
}

#ifdef _WIN32
// WASAPI includes
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

VideoSource::~VideoSource() {
    close();
}

// ─── Audio output initialization ─────────────────────────────────────

#ifdef _WIN32
bool VideoSource::initAudioOutput() {
    HRESULT hr;

    // Get default audio output device
    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        std::cerr << "[Video] Failed to create device enumerator" << std::endl;
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) {
        std::cerr << "[Video] No audio output device" << std::endl;
        return false;
    }
    m_audioDevice = device;

    // Activate audio client
    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        std::cerr << "[Video] Failed to activate audio client" << std::endl;
        return false;
    }
    m_audioClient = audioClient;

    // Get the device's mix format
    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[Video] Failed to get mix format" << std::endl;
        return false;
    }

    m_audioOutSampleRate = mixFmt->nSamplesPerSec;
    m_audioOutChannels = mixFmt->nChannels;

    // Initialize in shared mode with 40ms buffer
    REFERENCE_TIME bufferDuration = 400000; // 40ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                  bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[Video] Failed to initialize audio client: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Get buffer size
    UINT32 bufFrames = 0;
    audioClient->GetBufferSize(&bufFrames);
    m_audioBufferFrames = bufFrames;

    // Get render client
    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) {
        std::cerr << "[Video] Failed to get render client" << std::endl;
        return false;
    }
    m_renderClient = renderClient;

    // Allocate ring buffer (2 seconds of audio)
    m_audioRing.resize(m_audioOutSampleRate * m_audioOutChannels * 2, 0.0f);
    m_audioWritePos = 0;
    m_audioReadPos = 0;

    std::cout << "[Video] Audio output: " << m_audioOutSampleRate << "Hz, "
              << m_audioOutChannels << "ch, buf=" << bufFrames << " frames" << std::endl;

    return true;
}
#else
bool VideoSource::initAudioOutput() {
    std::cerr << "[Video] Audio output not implemented on this platform" << std::endl;
    return false;
}
#endif

void VideoSource::cleanupAudio() {
#ifdef _WIN32
    if (m_audioClient) {
        ((IAudioClient*)m_audioClient)->Stop();
        ((IAudioClient*)m_audioClient)->Release();
        m_audioClient = nullptr;
    }
    if (m_renderClient) {
        ((IAudioRenderClient*)m_renderClient)->Release();
        m_renderClient = nullptr;
    }
    if (m_audioDevice) {
        ((IMMDevice*)m_audioDevice)->Release();
        m_audioDevice = nullptr;
    }
#else
    m_audioClient = nullptr;
    m_renderClient = nullptr;
    m_audioDevice = nullptr;
#endif
    if (m_audioCodecCtx) {
        avcodec_free_context(&m_audioCodecCtx);
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
    }
    m_audioStreamIndex = -1;
}

void VideoSource::feedAudioToWASAPI() {
    if (!m_audioClient || !m_renderClient) return;

#ifdef _WIN32
    auto* audioClient = (IAudioClient*)m_audioClient;
    auto* renderClient = (IAudioRenderClient*)m_renderClient;

    UINT32 padding = 0;
    audioClient->GetCurrentPadding(&padding);
    UINT32 available = m_audioBufferFrames - padding;
    if (available == 0) return;

    size_t ringSize = m_audioRing.size();
    size_t rp = m_audioReadPos.load();
    size_t wp = m_audioWritePos.load();

    // How many frames available in ring buffer
    size_t samplesInRing = (wp >= rp) ? (wp - rp) : (ringSize - rp + wp);
    size_t framesInRing = samplesInRing / m_audioOutChannels;

    UINT32 toWrite = (available < framesInRing) ? available : (UINT32)framesInRing;
    if (toWrite == 0) return;

    BYTE* buffer = nullptr;
    HRESULT hr = renderClient->GetBuffer(toWrite, &buffer);
    if (FAILED(hr)) return;

    float* dst = (float*)buffer;
    float vol = m_volume;
    for (UINT32 i = 0; i < toWrite * (UINT32)m_audioOutChannels; i++) {
        dst[i] = m_audioRing[rp] * vol;
        rp = (rp + 1) % ringSize;
    }
    m_audioReadPos.store(rp);

    renderClient->ReleaseBuffer(toWrite, 0);
    m_audioFramesPlayed += toWrite;
#endif
}

void VideoSource::decodeAudioPacket(AVFrame* frame, AVPacket* pkt) {
    if (!m_audioCodecCtx || !m_swrCtx) return;

    int ret = avcodec_send_packet(m_audioCodecCtx, pkt);
    if (ret < 0) return;

    while (avcodec_receive_frame(m_audioCodecCtx, frame) == 0) {
        // Resample to output format (float32, stereo/device channels, device sample rate)
        int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
        std::vector<float> outBuf(outSamples * m_audioOutChannels);
        uint8_t* outPtr = (uint8_t*)outBuf.data();

        int converted = swr_convert(m_swrCtx, &outPtr, outSamples,
                                     (const uint8_t**)frame->extended_data, frame->nb_samples);
        if (converted <= 0) continue;

        // Write to ring buffer
        size_t ringSize = m_audioRing.size();
        size_t wp = m_audioWritePos.load();
        int totalSamples = converted * m_audioOutChannels;
        for (int i = 0; i < totalSamples; i++) {
            m_audioRing[wp] = outBuf[i];
            wp = (wp + 1) % ringSize;
        }
        m_audioWritePos.store(wp);
    }
}

// ─── Load / Close ────────────────────────────────────────────────────

bool VideoSource::load(const std::string& path) {
    close();
    m_path = path;

    // Detect live streams (RTMP, SRT, etc.)
    m_isLive = (path.rfind("rtmp://", 0) == 0 || path.rfind("srt://", 0) == 0 ||
                path.rfind("rtsp://", 0) == 0);

    if (avformat_open_input(&m_formatCtx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open video: " << path << std::endl;
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        close();
        return false;
    }

    // Find video and audio streams
    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        auto type = m_formatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && m_videoStreamIndex < 0) {
            m_videoStreamIndex = i;
        } else if (type == AVMEDIA_TYPE_AUDIO && m_audioStreamIndex < 0) {
            m_audioStreamIndex = i;
        }
    }

    if (m_videoStreamIndex < 0) {
        std::cerr << "No video stream found" << std::endl;
        close();
        return false;
    }

    // Open video decoder
    auto* codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported video codec" << std::endl;
        close();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, codecpar);

#ifdef __APPLE__
    // Try VideoToolbox hardware acceleration on macOS
    AVBufferRef* hwDeviceCtx = nullptr;
    if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) == 0) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        av_buffer_unref(&hwDeviceCtx);
        std::cout << "[Video] VideoToolbox hardware decode enabled" << std::endl;
    } else {
        std::cout << "[Video] VideoToolbox not available, using software decode" << std::endl;
    }
#endif

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "Failed to open video codec" << std::endl;
        close();
        return false;
    }

    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;

    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    m_timeBase = av_q2d(stream->time_base);
    m_duration = (stream->duration != AV_NOPTS_VALUE)
        ? stream->duration * m_timeBase
        : m_formatCtx->duration / (double)AV_TIME_BASE;

    // Create sws context for conversion to RGBA
    // Note: with VideoToolbox, pix_fmt may be VIDEOTOOLBOX at this point;
    // the actual software format is determined after the first frame transfer.
    // We'll create/recreate sws in the decode loop as needed.
    m_swsCtx = sws_getContext(
        m_width, m_height, m_codecCtx->pix_fmt,
        m_width, m_height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    // sws may fail for hw formats — that's OK, we'll create it lazily
    if (!m_swsCtx && !m_codecCtx->hw_device_ctx) {
        std::cerr << "Failed to create sws context" << std::endl;
        close();
        return false;
    }

    // Open audio decoder and set up resampler
    if (m_audioStreamIndex >= 0) {
        auto* audioCodecpar = m_formatCtx->streams[m_audioStreamIndex]->codecpar;
        const AVCodec* audioCodec = avcodec_find_decoder(audioCodecpar->codec_id);
        if (audioCodec) {
            m_audioCodecCtx = avcodec_alloc_context3(audioCodec);
            avcodec_parameters_to_context(m_audioCodecCtx, audioCodecpar);
            if (avcodec_open2(m_audioCodecCtx, audioCodec, nullptr) == 0) {
                m_audioTimeBase = av_q2d(m_formatCtx->streams[m_audioStreamIndex]->time_base);

                // Initialize WASAPI output first to get device format
                if (initAudioOutput()) {
                    // Set up resampler: source format -> device format (float32)
                    AVChannelLayout outLayout = {};
                    av_channel_layout_default(&outLayout, m_audioOutChannels);

                    swr_alloc_set_opts2(&m_swrCtx,
                        &outLayout, AV_SAMPLE_FMT_FLT, m_audioOutSampleRate,
                        &m_audioCodecCtx->ch_layout, m_audioCodecCtx->sample_fmt,
                        m_audioCodecCtx->sample_rate, 0, nullptr);

                    if (swr_init(m_swrCtx) < 0) {
                        std::cerr << "[Video] Failed to init SWR" << std::endl;
                        swr_free(&m_swrCtx);
                        m_swrCtx = nullptr;
                    }

                    av_channel_layout_uninit(&outLayout);
                } else {
                    std::cerr << "[Video] Audio output init failed, video will play without audio" << std::endl;
                }
            } else {
                avcodec_free_context(&m_audioCodecCtx);
                m_audioStreamIndex = -1;
            }
        }
    }

    // Allocate triple buffers
    int frameSize = m_width * m_height * 4;
    for (auto& buf : m_buffers) {
        buf.data.resize(frameSize);
        buf.ready = false;
    }

    // Create GL texture
    m_texture.createEmpty(m_width, m_height);

    // Start decode thread
    m_running = true;
    m_playing = false;
    m_decodeThread = std::thread(&VideoSource::decodeLoop, this);

    return true;
}

void VideoSource::close() {
    m_running = false;
    m_playing = false;
    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    cleanupAudio();
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); }
}

void VideoSource::play() {
    m_playbackStart = glfwGetTime();
    m_playbackOffset = m_currentTime;
    m_playing = true;
#ifdef _WIN32
    if (m_audioClient) {
        ((IAudioClient*)m_audioClient)->Start();
    }
#endif
}

void VideoSource::pause() {
    m_playing = false;
#ifdef _WIN32
    if (m_audioClient) {
        ((IAudioClient*)m_audioClient)->Stop();
    }
#endif
}

void VideoSource::seek(double seconds) {
    m_seekTarget = seconds;
    m_seekRequested = true;
    m_playbackOffset = seconds;
    m_playbackStart = glfwGetTime();
}

// ─── Update (main thread) ───────────────────────────────────────────

void VideoSource::update() {
    if (!m_running) return;

    // Feed decoded audio to WASAPI
    if (m_playing) {
        feedAudioToWASAPI();
    }

    // Find the latest ready frame (skip stale ones)
    int displayed = -1;
    for (int i = 0; i < 3; i++) {
        if (m_buffers[i].ready.load()) {
            if (displayed >= 0) {
                m_buffers[displayed].ready = false; // skip older frame
            }
            displayed = i;
        }
    }

    if (displayed >= 0) {
        m_texture.updateData(m_buffers[displayed].data.data(), m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE);
        m_currentTime = m_buffers[displayed].pts;
        m_buffers[displayed].ready = false;
    }
}

// ─── Decode loop (background thread) ────────────────────────────────

void VideoSource::decodeLoop() {
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbaFrame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    if (!frame || !rgbaFrame || !audioFrame || !pkt) {
        if (frame) av_frame_free(&frame);
        if (rgbaFrame) av_frame_free(&rgbaFrame);
        if (audioFrame) av_frame_free(&audioFrame);
        if (pkt) av_packet_free(&pkt);
        return;
    }

    int rgbaSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_width, m_height, 1);
    std::vector<uint8_t> rgbaBuf(rgbaSize);
    av_image_fill_arrays(rgbaFrame->data, rgbaFrame->linesize, rgbaBuf.data(),
                         AV_PIX_FMT_RGBA, m_width, m_height, 1);

    // Pending video frame waiting for display time
    bool hasPendingVideo = false;
    double pendingPts = 0.0;

    while (m_running) {
        // Handle seek
        if (m_seekRequested) {
            m_seekRequested = false;
            double target = m_seekTarget.load();
            int64_t ts = (int64_t)(target / m_timeBase);
            av_seek_frame(m_formatCtx, m_videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_codecCtx);
            if (m_audioCodecCtx) {
                avcodec_flush_buffers(m_audioCodecCtx);
            }
            // Reset audio ring buffer and clock
            m_audioWritePos = 0;
            m_audioReadPos = 0;
            m_audioFramesPlayed = 0;
            hasPendingVideo = false;
        }

        if (!m_playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // If we have a pending video frame, check if it's time to display
        if (hasPendingVideo) {
            bool shouldDisplay = m_isLive; // Live streams: display immediately
            if (!shouldDisplay) {
                double elapsed = glfwGetTime() - m_playbackStart + m_playbackOffset;
                double waitTime = pendingPts - elapsed;
                shouldDisplay = (waitTime <= 0.001);
                if (!shouldDisplay && waitTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
            if (shouldDisplay) {
                int wi = -1;
                for (int i = 0; i < 3; i++) {
                    if (!m_buffers[i].ready.load()) { wi = i; break; }
                }
                if (wi < 0) { wi = 0; m_buffers[wi].ready = false; }
                memcpy(m_buffers[wi].data.data(), rgbaBuf.data(), rgbaSize);
                m_buffers[wi].pts = pendingPts;
                m_buffers[wi].ready = true;
                hasPendingVideo = false;
            }
        }

        // Read next packet
        int ret = av_read_frame(m_formatCtx, pkt);
        if (ret < 0) {
            if (m_isLive) {
                // Live stream: brief pause then retry (network hiccup)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // End of file - loop
            av_seek_frame(m_formatCtx, m_videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_codecCtx);
            if (m_audioCodecCtx) avcodec_flush_buffers(m_audioCodecCtx);
            m_playbackStart = glfwGetTime();
            m_playbackOffset = 0.0;
            m_audioWritePos = 0;
            m_audioReadPos = 0;
            m_audioFramesPlayed = 0;
            hasPendingVideo = false;
            continue;
        }

        if (pkt->stream_index == m_videoStreamIndex) {
            avcodec_send_packet(m_codecCtx, pkt);
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == 0 && !hasPendingVideo) {
                // Transfer hardware frame to software if needed (VideoToolbox)
                AVFrame* swFrame = nullptr;
                AVFrame* srcFrame = frame;
                if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                    swFrame = av_frame_alloc();
                    if (av_hwframe_transfer_data(swFrame, frame, 0) == 0) {
                        srcFrame = swFrame;
                    } else {
                        av_frame_free(&swFrame);
                        av_packet_unref(pkt);
                        continue;
                    }
                }

                // Recreate sws context if pixel format changed (e.g. hw transfer output)
                if (!m_swsCtx || srcFrame->format != m_lastSwsFmt) {
                    if (m_swsCtx) sws_freeContext(m_swsCtx);
                    m_swsCtx = sws_getContext(
                        m_width, m_height, (AVPixelFormat)srcFrame->format,
                        m_width, m_height, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    m_lastSwsFmt = (AVPixelFormat)srcFrame->format;
                }

                // Convert to RGBA
                if (m_swsCtx) {
                    sws_scale(m_swsCtx, srcFrame->data, srcFrame->linesize, 0, m_height,
                              rgbaFrame->data, rgbaFrame->linesize);
                }

                if (swFrame) av_frame_free(&swFrame);

                pendingPts = 0.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    pendingPts = frame->pts * m_timeBase;
                }
                hasPendingVideo = true;
            }
        } else if (pkt->stream_index == m_audioStreamIndex) {
            decodeAudioPacket(audioFrame, pkt);
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_frame_free(&rgbaFrame);
    av_frame_free(&audioFrame);
    av_packet_free(&pkt);
}
