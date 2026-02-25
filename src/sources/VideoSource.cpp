#include "sources/VideoSource.h"
#include <iostream>
#include <GLFW/glfw3.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

VideoSource::~VideoSource() {
    close();
}

bool VideoSource::load(const std::string& path) {
    close();
    m_path = path;

    if (avformat_open_input(&m_formatCtx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open video: " << path << std::endl;
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        close();
        return false;
    }

    // Find video stream
    m_videoStreamIndex = -1;
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        std::cerr << "No video stream found" << std::endl;
        close();
        return false;
    }

    auto* codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec" << std::endl;
        close();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, codecpar);
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
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
    m_swsCtx = sws_getContext(
        m_width, m_height, m_codecCtx->pix_fmt,
        m_width, m_height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx) {
        std::cerr << "Failed to create sws context" << std::endl;
        close();
        return false;
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

    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); }
}

void VideoSource::play() {
    m_playbackStart = glfwGetTime();
    m_playbackOffset = m_currentTime;
    m_playing = true;
}

void VideoSource::pause() {
    m_playing = false;
}

void VideoSource::seek(double seconds) {
    m_seekTarget = seconds;
    m_seekRequested = true;
    m_playbackOffset = seconds;
    m_playbackStart = glfwGetTime();
}

void VideoSource::update() {
    if (!m_running) return;

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

void VideoSource::decodeLoop() {
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbaFrame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    if (!frame || !rgbaFrame || !pkt) {
        if (frame) av_frame_free(&frame);
        if (rgbaFrame) av_frame_free(&rgbaFrame);
        if (pkt) av_packet_free(&pkt);
        return;
    }

    int rgbaSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_width, m_height, 1);
    std::vector<uint8_t> rgbaBuf(rgbaSize);
    av_image_fill_arrays(rgbaFrame->data, rgbaFrame->linesize, rgbaBuf.data(),
                         AV_PIX_FMT_RGBA, m_width, m_height, 1);

    while (m_running) {
        // Handle seek
        if (m_seekRequested) {
            m_seekRequested = false;
            double target = m_seekTarget.load();
            int64_t ts = (int64_t)(target / m_timeBase);
            av_seek_frame(m_formatCtx, m_videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_codecCtx);
        }

        if (!m_playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Decode a frame
        bool gotFrame = false;
        while (!gotFrame && m_running) {
            int ret = av_read_frame(m_formatCtx, pkt);
            if (ret < 0) {
                // End of file - loop
                av_seek_frame(m_formatCtx, m_videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(m_codecCtx);
                m_playbackStart = glfwGetTime();
                m_playbackOffset = 0.0;
                continue;
            }

            if (pkt->stream_index == m_videoStreamIndex) {
                avcodec_send_packet(m_codecCtx, pkt);
                ret = avcodec_receive_frame(m_codecCtx, frame);
                if (ret == 0) {
                    gotFrame = true;
                }
            }
            av_packet_unref(pkt);
        }

        if (gotFrame) {
            // Convert to RGBA
            sws_scale(m_swsCtx, frame->data, frame->linesize, 0, m_height,
                      rgbaFrame->data, rgbaFrame->linesize);

            // Calculate PTS
            double pts = 0.0;
            if (frame->pts != AV_NOPTS_VALUE) {
                pts = frame->pts * m_timeBase;
            }

            // Frame timing - wait until it's time to display
            double elapsed = glfwGetTime() - m_playbackStart + m_playbackOffset;
            double waitTime = pts - elapsed;
            if (waitTime > 0.001) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds((int)(waitTime * 1000)));
            }

            // Write to triple buffer — find a free slot
            int wi = -1;
            for (int i = 0; i < 3; i++) {
                if (!m_buffers[i].ready.load()) {
                    wi = i;
                    break;
                }
            }
            if (wi < 0) {
                // All buffers full, overwrite oldest (slot 0 as fallback)
                wi = 0;
                m_buffers[wi].ready = false;
            }
            memcpy(m_buffers[wi].data.data(), rgbaBuf.data(), rgbaSize);
            m_buffers[wi].pts = pts;
            m_buffers[wi].ready = true;
        }
    }

    av_frame_free(&frame);
    av_frame_free(&rgbaFrame);
    av_packet_free(&pkt);
}
