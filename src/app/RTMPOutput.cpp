#ifdef HAS_FFMPEG
#include "app/RTMPOutput.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <cctype>

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
// WASAPI loopback capture
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#endif

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
    m_stopRequested = false;
    m_packetsWritten = 0;
    m_videoPacketsWritten = 0;
    m_videoTime = 0;
    m_audioTime = 0;
    m_audioCaptureGaps = 0;
    m_audioSilenceFrames = 0;
    m_idleAudioTarget = 0;
    m_fps = fps;
    m_nextReadback = 0;
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
    m_audioClock.reset();
    if (m_captureClient && m_audioCodecCtx) {
        m_audioThread = std::thread(&RTMPOutput::captureAudioThread, this);
        m_audioEncodeThread = std::thread(&RTMPOutput::encodeAudioThread, this);
    }
    m_thread = std::thread(&RTMPOutput::encodeThread, this);
    std::cout << "[RTMP] Streaming started" << std::endl;
    return true;
}

void RTMPOutput::stop() {
    if (m_active) {
        m_stopRequested = true;
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        if (m_audioThread.joinable())
            m_audioThread.join();
        m_active = false;
        std::cout << "[RTMP] Stream stopped. Dropped frames: " << m_droppedFrames << std::endl;
    }
    cleanup();
}

double RTMPOutput::uptimeSeconds() const {
    if (!m_active) return 0;
    return av_gettime_relative() / 1000000.0 - m_startTime;
}

int RTMPOutput::interruptIO(void* opaque) {
    auto* output = static_cast<RTMPOutput*>(opaque);
    const auto deadline = output->m_ioDeadline.load();
    return deadline > 0 && av_gettime_relative() > deadline;
}

int RTMPOutput::writePacket(AVPacket* packet) {
    if (!packet) packet = m_packet;
    std::lock_guard<std::mutex> lock(m_muxMutex);
    const bool video = m_videoStream && packet->stream_index == m_videoStream->index;
    m_ioDeadline = av_gettime_relative() + 5000000;
    int result = av_interleaved_write_frame(m_fmtCtx, packet);
    m_ioDeadline = 0;
    if (result < 0) m_stopRequested = true;
    else {
        ++m_packetsWritten;
        if (video) ++m_videoPacketsWritten;
    }
    return result;
}

// ─── WASAPI loopback init ───────────────────────────────────────────

#ifdef _WIN32
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
    bool loopback = true;
    // Use the installation's program-audio route. Virtual-cable installations
    // often have a silent default render endpoint; the cable capture endpoint
    // carries the actual show. A stream-specific override takes precedence.
    const char* configured = std::getenv("EASEL_STREAM_AUDIO");
    if (!configured || !configured[0]) configured = std::getenv("EASEL_RECORD_SYS");
    if (configured && configured[0]) {
        std::string needle = configured;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count && !device; ++i) {
                IMMDevice* candidate = nullptr;
                if (FAILED(collection->Item(i, &candidate))) continue;
                IPropertyStore* properties = nullptr;
                bool match = false;
                if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties))) {
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    PROPERTYKEY nameKey = {{0xa45c254e, 0xdf1c, 0x4efd,
                        {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
                    if (SUCCEEDED(properties->GetValue(nameKey, &value)) && value.vt == VT_LPWSTR) {
                        char name[1024] = {};
                        WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
                        std::string lower = name;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) { return (char)std::tolower(c); });
                        match = lower.find(needle) != std::string::npos;
                    }
                    PropVariantClear(&value);
                    properties->Release();
                }
                if (match) {
                    IMMEndpoint* endpoint = nullptr;
                    EDataFlow flow = eRender;
                    if (SUCCEEDED(candidate->QueryInterface(__uuidof(IMMEndpoint), (void**)&endpoint))) {
                        endpoint->GetDataFlow(&flow);
                        endpoint->Release();
                    }
                    loopback = flow == eRender;
                    device = candidate;
                } else candidate->Release();
            }
            collection->Release();
        }
        hr = device ? S_OK : E_FAIL;
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    enumerator->Release();
    if (FAILED(hr)) {
        std::cerr << "[RTMP] Program audio device unavailable" << std::endl;
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
    // Capacity for scheduling hiccups, not an added playback delay: a dedicated
    // capture thread drains available packets every 5 ms.
    REFERENCE_TIME bufferDuration = 2000000; // 200ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
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
#else
bool RTMPOutput::initAudioCapture() {
    std::cerr << "[RTMP] Audio capture not implemented on this platform" << std::endl;
    return false;
}
#endif

// ─── FFmpeg encoder init ────────────────────────────────────────────

bool RTMPOutput::initEncoder(const std::string& url, int width, int height,
                              int aspectNum, int aspectDen, int fps) {
    m_width = width;
    m_height = height;
    if (width < 2 || height < 2 || fps <= 0) return false;

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

#ifdef _WIN32
    // Initialize COM for WASAPI (this thread)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Allocate output format context for RTMP/FLV
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "flv", url.c_str());
    if (ret < 0 || !m_fmtCtx) {
        std::cerr << "[RTMP] Failed to create output context" << std::endl;
        return false;
    }

    // ── Video stream (H.264) ──
    const char* requested = std::getenv("EASEL_STREAM_ENCODER");
    const AVCodec* hardware = (!requested || std::string(requested) != "software")
        ? avcodec_find_encoder_by_name("h264_nvenc") : nullptr;
    const AVCodec* software = avcodec_find_encoder(AV_CODEC_ID_H264);
    m_videoStream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_videoStream) return false;

    // Use NVENC's dedicated engine when available. Fall back cleanly on hosts
    // without a usable NVIDIA device; no hardware requirement for the app.
    for (const AVCodec* codec : {hardware, software}) {
        if (!codec) continue;
        m_videoCodecCtx = avcodec_alloc_context3(codec);
        if (!m_videoCodecCtx) return false;
        m_videoCodecCtx->width = m_encWidth;
        m_videoCodecCtx->height = m_encHeight;
        m_videoCodecCtx->time_base = {1, fps};
        m_videoCodecCtx->framerate = {fps, 1};
        m_videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        m_videoCodecCtx->gop_size = fps * 2;
        m_videoCodecCtx->max_b_frames = 0;
        m_videoCodecCtx->bit_rate = m_encHeight >= 1080 ? 6000000 : 4500000;
        m_videoCodecCtx->rc_max_rate = m_videoCodecCtx->bit_rate;
        m_videoCodecCtx->rc_buffer_size = static_cast<int>(m_videoCodecCtx->bit_rate);
        if (codec == hardware) {
            av_opt_set(m_videoCodecCtx->priv_data, "preset", "p3", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "tune", "ull", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "rc", "cbr", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "zerolatency", "1", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "rc-lookahead", "0", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "delay", "0", 0);
            const char* gpu = std::getenv("EASEL_STREAM_GPU");
            av_opt_set(m_videoCodecCtx->priv_data, "gpu", gpu ? gpu : "0", 0);
        } else {
            av_opt_set(m_videoCodecCtx->priv_data, "preset", "veryfast", 0);
            av_opt_set(m_videoCodecCtx->priv_data, "tune", "zerolatency", 0);
        }
        av_opt_set(m_videoCodecCtx->priv_data, "profile", "high", 0);
        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ret = avcodec_open2(m_videoCodecCtx, codec, nullptr);
        if (ret >= 0) {
            m_videoEncoder = codec->name;
            std::cout << "[RTMP] Video encoder: " << m_videoEncoder << std::endl;
            break;
        }
        avcodec_free_context(&m_videoCodecCtx);
    }
    if (!m_videoCodecCtx) {
        std::cerr << "[RTMP] No usable H.264 encoder" << std::endl;
        return false;
    }

    avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx);
    m_videoStream->time_base = m_videoCodecCtx->time_base;
    m_videoStream->avg_frame_rate = {fps, 1};

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

#ifdef _WIN32
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
#else
    (void)hasAudio;
#endif

    // ── Open RTMP connection and write header ──
    m_packet = av_packet_alloc();
    m_audioPacket = av_packet_alloc();

    m_fmtCtx->interrupt_callback = {&RTMPOutput::interruptIO, this};
    // Do not let one stalled track accumulate seconds of the other in the muxer.
    m_fmtCtx->max_interleave_delta = 100000;
    m_fmtCtx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    m_ioDeadline = av_gettime_relative() + 5000000;
    AVDictionary* ioOptions = nullptr;
    av_dict_set(&ioOptions, "rw_timeout", "5000000", 0);
    ret = avio_open2(&m_fmtCtx->pb, url.c_str(), AVIO_FLAG_WRITE,
                    &m_fmtCtx->interrupt_callback, &ioOptions);
    av_dict_free(&ioOptions);
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
    m_ioDeadline = 0;

    // Allocate readback buffers
    m_readbackBuf.resize(width * height * 4);
    m_encodeBuf.resize(width * height * 4);

    return true;
}

// ─── GL readback (main thread) ──────────────────────────────────────

void RTMPOutput::sendFrame(GLuint texture, int w, int h) {
    if (!isActive()) return;
    if (w != m_width || h != m_height) {
        m_stopRequested = true; // supervisor can reopen with the new geometry
        return;
    }
    double now = av_gettime_relative() / 1000000.0;
    if (now < m_nextReadback) return;
    m_nextReadback = std::max(m_nextReadback + 1.0 / m_fps, now);

    size_t bytes = (size_t)w * h * 4;

    // Create PBOs on first use or size change
    if (!m_pbo[0] || (int)m_readbackBuf.size() != (int)bytes) {
        if (m_pbo[0]) glDeleteBuffers(2, m_pbo);
        glGenBuffers(2, m_pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pboIndex = 0;
        m_pboReady = false;
        m_readbackBuf.resize(bytes);
    }

    int readPBO = m_pboIndex;
    int mapPBO = 1 - m_pboIndex;

    // Kick off async readback into current PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readPBO]);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Map previous PBO (already complete) and hand off to encoder
    if (m_pboReady) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[mapPBO]);
        void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (ptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_frameReady) m_droppedFrames++;

            int srcStride = w * 4;
            int dstStride = m_cropW * 4;
            m_encodeBuf.resize(m_cropW * m_cropH * 4);
            const uint8_t* src = (const uint8_t*)ptr;
            for (int row = 0; row < m_cropH; row++) {
                std::memcpy(m_encodeBuf.data() + row * dstStride,
                            src + (m_cropY + row) * srcStride + m_cropX * 4,
                            dstStride);
            }
            m_frameReady = true;
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        m_cv.notify_one();
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_pboIndex = 1 - m_pboIndex;
    m_pboReady = true;
}

// ─── Audio drain (called from encode thread) ────────────────────────

void RTMPOutput::captureAudioThread() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    auto* capture = static_cast<IAudioCaptureClient*>(m_captureClient);
    bool first = true;
    double lastCapture = uptimeSeconds();
    while (!m_stopRequested) {
        UINT32 packetLength = 0;
        if (FAILED(capture->GetNextPacketSize(&packetLength))) {
            m_stopRequested = true;
            break;
        }
        while (packetLength > 0 && !m_stopRequested) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0, qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) {
                m_stopRequested = true;
                break;
            }
            CapturedAudio packet;
            packet.frames = frames;
            packet.position = position;
            packet.timestampValid = !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
            double elapsed = uptimeSeconds();
            double age = double(frames) / m_wasapiSampleRate;
            if (packet.timestampValid) {
                LARGE_INTEGER counter, frequency;
                QueryPerformanceCounter(&counter);
                QueryPerformanceFrequency(&frequency);
                age = double(counter.QuadPart) / frequency.QuadPart - double(qpc) / 10000000.0;
            }
            packet.wallSample = static_cast<int64_t>((elapsed - std::max(0.0, age)) * m_wasapiSampleRate);
            packet.samples.resize(size_t(frames) * m_wasapiChannels, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                std::memcpy(packet.samples.data(), data, packet.samples.size() * sizeof(float));
            // Release WASAPI before taking any queue lock, encoding or network IO.
            capture->ReleaseBuffer(frames);
            if (!first && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
                ++m_audioCaptureGaps;
            first = false;
            {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                m_queuedAudioFrames += frames;
                m_audioQueue.push_back(std::move(packet));
                lastCapture = uptimeSeconds();
                m_idleAudioTarget = 0;
                // Bounded storage even if the network blocks until its timeout.
                while (m_queuedAudioFrames > size_t(m_wasapiSampleRate) && m_audioQueue.size() > 1) {
                    m_queuedAudioFrames -= m_audioQueue.front().frames;
                    m_audioQueue.pop_front();
                    ++m_audioCaptureGaps;
                }
            }
            if (FAILED(capture->GetNextPacketSize(&packetLength))) {
                m_stopRequested = true;
                break;
            }
        }
        // Only the capture thread can distinguish an idle device from samples
        // waiting in WASAPI while this thread was descheduled during a scene cut.
        // Drain all available packets first. An encoder-side wall-clock deadline
        // used to insert silence and then discard those valid, delayed samples.
        if (uptimeSeconds() - lastCapture >= 0.250) {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            m_idleAudioTarget = static_cast<int64_t>((uptimeSeconds() - 0.060) * m_wasapiSampleRate);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CoUninitialize();
#endif
}

void RTMPOutput::drainAudio() {
    if (!m_captureClient || !m_audioCodecCtx) return;
    std::deque<CapturedAudio> packets;
    int64_t idleTarget = 0;
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        packets.swap(m_audioQueue);
        m_queuedAudioFrames = 0;
        idleTarget = m_idleAudioTarget;
    }
    for (const auto& packet : packets) {
        if (m_stopRequested) break;
        int64_t begin = m_audioClock.sampleAt(packet.position, packet.wallSample, packet.timestampValid);
        m_audioClock.advance(begin, packet.frames);
        int64_t end = m_audioSamplesWritten + m_audioAccum.size() / m_wasapiChannels;
        if (begin > end) {
            encodeAudioSamples(nullptr, static_cast<int>(begin - end), m_wasapiChannels);
            end = begin;
        }
        int skip = static_cast<int>(std::min<int64_t>(packet.frames, std::max<int64_t>(0, end - begin)));
        if (skip < static_cast<int>(packet.frames))
            encodeAudioSamples(packet.samples.data() + skip * m_wasapiChannels,
                packet.frames - skip, m_wasapiChannels);
    }
    // Fill only silence confirmed by the capture thread, never a scheduling gap.
    int64_t end = m_audioSamplesWritten + m_audioAccum.size() / m_wasapiChannels;
    if (idleTarget > end && !m_stopRequested)
        encodeAudioSamples(nullptr, static_cast<int>(idleTarget - end), m_wasapiChannels);
}

void RTMPOutput::encodeAudioSamples(const float* data, int numSamples, int channels) {
    if (!data) m_audioSilenceFrames += numSamples;
    // Accumulate interleaved float samples, encode when we have a full AAC frame
    int totalFloats = numSamples * channels;
    if (data) m_audioAccum.insert(m_audioAccum.end(), data, data + totalFloats);
    else m_audioAccum.insert(m_audioAccum.end(), totalFloats, 0.0f);

    // Process complete frames (m_audioFrameSize samples * channels)
    int samplesPerFrame = m_audioFrameSize; // in sample-sets (not individual floats)
    int floatsPerFrame = samplesPerFrame * channels;

    while (!m_stopRequested && (int)m_audioAccum.size() >= floatsPerFrame) {
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
            m_audioTime = double(m_audioSamplesWritten) / m_wasapiSampleRate;

            int ret = avcodec_send_frame(m_audioCodecCtx, m_audioFrame);
            while (ret >= 0) {
                ret = avcodec_receive_packet(m_audioCodecCtx, m_audioPacket);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                av_packet_rescale_ts(m_audioPacket,
                    m_audioCodecCtx->time_base, m_audioStream->time_base);
                m_audioPacket->stream_index = m_audioStream->index;
                int written = writePacket(m_audioPacket);
                av_packet_unref(m_audioPacket);
                if (written < 0) break;
            }
        }

        // Remove consumed samples
        m_audioAccum.erase(m_audioAccum.begin(), m_audioAccum.begin() + floatsPerFrame);
    }
}

// ─── Encode thread ──────────────────────────────────────────────────

void RTMPOutput::encodeAudioThread() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    while (!m_stopRequested) {
        drainAudio();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Flush audio encoder
    if (m_audioCodecCtx) {
        avcodec_send_frame(m_audioCodecCtx, nullptr);
        while (avcodec_receive_packet(m_audioCodecCtx, m_audioPacket) == 0) {
            av_packet_rescale_ts(m_audioPacket, m_audioCodecCtx->time_base, m_audioStream->time_base);
            m_audioPacket->stream_index = m_audioStream->index;
            int written = writePacket(m_audioPacket);
            av_packet_unref(m_audioPacket);
            if (written < 0) break;
        }
    }

}

void RTMPOutput::encodeThread() {
#ifdef _WIN32
    // COM init for this thread (WASAPI calls happen here via drainAudio)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    std::vector<uint8_t> frame;
    while (!m_stopRequested) {
        // CFR belongs to the encoder clock, not the renderer's arrival rate.
        // Repeat the latest image through render stalls. If video encoding itself
        // stalls, skip overdue slots instead of restarting uninterrupted audio or
        // sending a burst of stale video. Both tracks retain wall-clock time.
        double due = double(m_videoFrameIndex) / m_fps;
        double elapsed = uptimeSeconds();
        if (!frame.empty() && elapsed - due > 2.0 / m_fps) {
            auto current = static_cast<int64_t>(elapsed * m_fps);
            m_droppedFrames += static_cast<int>(current - m_videoFrameIndex);
            m_videoFrameIndex = current;
            due = double(current) / m_fps;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (elapsed < due || frame.empty()) {
            auto wait = std::chrono::duration<double>(frame.empty() ? 0.005 : std::min(0.005, due - elapsed));
            m_cv.wait_for(lock, wait, [this] { return m_stopRequested.load(); });
        }

        if (m_stopRequested) break;

        bool hasVideo = m_frameReady;
        if (hasVideo) {
            frame.swap(m_encodeBuf); // producer cannot overwrite a frame being encoded
            m_frameReady = false;
        }
        lock.unlock();

        if (!frame.empty() && !m_stopRequested && uptimeSeconds() >= due) {
            if (m_videoFrameIndex == 0)
                m_videoFrameIndex = static_cast<int64_t>(uptimeSeconds() * m_fps);
            encodeVideoFrame(frame.data(), m_width, m_height);
        }
    }

    // Audio owns its codec and packets; finish it before writing the trailer.
    if (m_audioEncodeThread.joinable()) m_audioEncodeThread.join();

    // Flush video encoder
    if (m_videoCodecCtx) {
        avcodec_send_frame(m_videoCodecCtx, nullptr);
        while (avcodec_receive_packet(m_videoCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_packet->stream_index = m_videoStream->index;
            int written = writePacket();
            av_packet_unref(m_packet);
            if (written < 0) break;
        }
    }

    if (m_fmtCtx && m_fmtCtx->pb) {
        m_ioDeadline = av_gettime_relative() + 5000000;
        av_write_trailer(m_fmtCtx);
    }

#ifdef _WIN32
    CoUninitialize();
#endif
}

void RTMPOutput::encodeVideoFrame(const uint8_t* rgbaData, int /*w*/, int /*h*/) {
    av_frame_make_writable(m_videoFrame);

    const uint8_t* srcSlice[1] = { rgbaData };
    int srcStride[1] = { m_cropW * 4 };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_cropH,
              m_videoFrame->data, m_videoFrame->linesize);

    m_videoFrame->pts = m_videoFrameIndex++;
    m_videoTime = double(m_videoFrame->pts) / m_fps;

    int ret = avcodec_send_frame(m_videoCodecCtx, m_videoFrame);
    if (ret < 0) return;

    while (ret >= 0) {
        ret = avcodec_receive_packet(m_videoCodecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;

        ret = writePacket();
        av_packet_unref(m_packet);
        if (ret < 0) {
            std::cerr << "[RTMP] Write error — connection lost?" << std::endl;
            m_stopRequested = true;
            break;
        }
    }
}

// ─── Cleanup ────────────────────────────────────────────────────────

void RTMPOutput::cleanupAudio() {
    // Stop and release WASAPI
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
    m_audioQueue.clear();
    m_queuedAudioFrames = 0;
}

void RTMPOutput::cleanup() {
    cleanupAudio();

    if (m_pbo[0]) { glDeleteBuffers(2, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
    m_pboReady = false;
    m_pboIndex = 0;

    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_videoFrame) { av_frame_free(&m_videoFrame); }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_audioPacket) { av_packet_free(&m_audioPacket); }
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); }
    if (m_fmtCtx) {
        m_ioDeadline = av_gettime_relative() + 5000000;
        if (m_fmtCtx->pb) avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStream = nullptr;
    m_readbackBuf.clear();
    m_encodeBuf.clear();
    m_frameReady = false;
    m_ioDeadline = 0;
}

#endif // HAS_FFMPEG
