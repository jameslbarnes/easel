#ifdef HAS_NDI
#include "app/NDIOutput.h"
#include <iostream>
#include <cstring>
#include <chrono>


NDIOutput::~NDIOutput() {
    destroy();
}

bool NDIOutput::create(const std::string& name) {
    destroy();

    auto& rt = NDIRuntime::instance();
    if (!rt.isAvailable()) return false;

    // The NDI runtime sometimes holds a sender name briefly after a previous
    // process crashed or was killed mid-shutdown; create() then returns null
    // for the duplicate name. Retry with "Name-2", "Name-3", … on failure so
    // a relaunch always finds an open slot.
    for (int suffix = 0; suffix < 8; ++suffix) {
        std::string tryName = (suffix == 0)
            ? name
            : (name + "-" + std::to_string(suffix + 1));

        NDIlib_send_create_t sendCreate = {};
        sendCreate.p_ndi_name = tryName.c_str();
        sendCreate.clock_video = false; // we pace frames ourselves
        sendCreate.clock_audio = false;

        m_send = rt.api()->send_create(&sendCreate);
        if (m_send) {
            m_publishedName = tryName;
            std::cout << "[NDI] Sender created: " << tryName << std::endl;
            return true;
        }
    }
    std::cerr << "[NDI] Failed to create sender (all 8 name slots taken — "
                 "is another Easel still running, or did NDI runtime fail to load?)"
              << std::endl;
    return false;
}

void NDIOutput::destroy() {
    if (m_send) {
        auto& rt = NDIRuntime::instance();
        if (rt.isAvailable()) {
            // Release the pixel buffer the async encoder may still be reading
            // before the sender (and then the buffer) goes away.
            if (m_asyncInFlight && rt.api()->send_send_video_async_v2) {
                rt.api()->send_send_video_async_v2(m_send, nullptr);
            }
            rt.api()->send_destroy(m_send);
        }
        m_send = nullptr;
    }
    m_asyncInFlight = false;
    for (int i = 0; i < kReadbackSlots; ++i) {
        if (m_fence[i]) {
            glDeleteSync(m_fence[i]);
            m_fence[i] = nullptr;
        }
    }
    if (m_pbo[0]) {
        glDeleteBuffers(kReadbackSlots, m_pbo);
        for (int i = 0; i < kReadbackSlots; ++i) m_pbo[i] = 0;
    }
    if (m_readFBO) {
        glDeleteFramebuffers(1, &m_readFBO);
        m_readFBO = 0;
    }
    m_pixelBuffer[0].clear();
    m_pixelBuffer[1].clear();
    m_sendBuf = 0;
    m_readIndex = 0;
    m_pendingReadbacks = 0;
    m_lastW = 0;
    m_lastH = 0;
    m_lastSendAt = {};
}

bool NDIOutput::hasReceivers() const {
    if (!m_send) return false;
    auto& rt = NDIRuntime::instance();
    return rt.api()->send_get_no_connections(m_send, 0) > 0;
}

void NDIOutput::send(GLuint texture, int w, int h) {
    if (!m_send || w <= 0 || h <= 0 || texture == 0) return;

    // Skip frames when no receivers are connected (avoid expensive readback)
    auto& rt = NDIRuntime::instance();
    if (rt.api()->send_get_no_connections(m_send, 0) == 0) return;

    size_t bytes = (size_t)w * h * 4;

    if (w != m_lastW || h != m_lastH || !m_pbo[0]) {
        // Resizing may reallocate the pixel buffers; make sure the async
        // encoder is not still reading the old storage.
        if (m_asyncInFlight && rt.api()->send_send_video_async_v2) {
            rt.api()->send_send_video_async_v2(m_send, nullptr);
        }
        m_asyncInFlight = false;
        for (int i = 0; i < kReadbackSlots; ++i) {
            if (m_fence[i]) {
                glDeleteSync(m_fence[i]);
                m_fence[i] = nullptr;
            }
        }
        if (m_pbo[0]) glDeleteBuffers(kReadbackSlots, m_pbo);
        glGenBuffers(kReadbackSlots, m_pbo);
        for (int i = 0; i < kReadbackSlots; ++i) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pixelBuffer[0].resize(bytes);
        m_pixelBuffer[1].resize(bytes);
        m_sendBuf = 0;
        m_readIndex = 0;
        m_pendingReadbacks = 0;
        m_lastW = w;
        m_lastH = h;
    }

    bool hasFrame = false;
    if (m_pendingReadbacks > 0) {
        const int mapPBO = m_readIndex;
        GLenum status = GL_ALREADY_SIGNALED;
        if (m_fence[mapPBO]) {
            status = glClientWaitSync(m_fence[mapPBO], 0, 0);
        }
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
            if (m_fence[mapPBO]) {
                glDeleteSync(m_fence[mapPBO]);
                m_fence[mapPBO] = nullptr;
            }

            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[mapPBO]);
            void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
            if (ptr) {
                std::memcpy(m_pixelBuffer[m_sendBuf].data(), ptr, bytes);
                hasFrame = true;
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_readIndex = (m_readIndex + 1) % kReadbackSlots;
            --m_pendingReadbacks;
        }
    }

    if (hasFrame) {
        NDIlib_video_frame_v2_t frame = {};
        frame.xres = w;
        frame.yres = h;
        frame.FourCC = NDIlib_FourCC_video_type_BGRA;
        // Stamp the actual pacing rather than a fixed 60.
        const float fps = m_settings.targetFps > 0.0f ? m_settings.targetFps : 60.0f;
        frame.frame_rate_N = (int)(fps * 1000.0f);
        frame.frame_rate_D = 1000;
        frame.picture_aspect_ratio = (float)w / (float)h;
        frame.frame_format_type = NDIlib_frame_format_type_progressive;
        frame.p_data = m_pixelBuffer[m_sendBuf].data();
        frame.line_stride_in_bytes = w * 4;

        // Async: SpeedHQ encode happens on the NDI SDK's thread instead of
        // stalling the render loop (sync sends were costing ~3-5ms per
        // active sender per frame — enough to vsync-halve to 30fps with
        // several zones subscribed). Guard the func ptrs — older/partially-
        // loaded NDI runtimes can leave them null (the audio path already
        // guards send_send_audio_v2).
        if (rt.api()->send_send_video_async_v2) {
            rt.api()->send_send_video_async_v2(m_send, &frame);
            m_asyncInFlight = true;
            m_sendBuf ^= 1;
        } else if (rt.api()->send_send_video_v2) {
            rt.api()->send_send_video_v2(m_send, &frame);
        }
    }

    // Settings-driven wall-clock throttle (default 60fps; <= 0 = uncapped,
    // live-settable via OSC /easel/ndi/fps). Readiness mapping above runs even
    // while throttled so completed readbacks do not sit behind the fps gate.
    const auto now = std::chrono::steady_clock::now();
    if (m_settings.targetFps > 0.0f &&
        m_lastSendAt.time_since_epoch().count() > 0) {
        const double elapsed = std::chrono::duration<double>(now - m_lastSendAt).count();
        if (elapsed < (1.0 / m_settings.targetFps) * 0.90) return;
    }
    if (m_pendingReadbacks >= kReadbackSlots) return;

    if (!m_readFBO) glGenFramebuffers(1, &m_readFBO);
    const int writePBO = (m_readIndex + m_pendingReadbacks) % kReadbackSlots;

    GLint prevReadFBO = 0;
    GLint prevPackBuffer = 0;
    GLint prevReadBuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackBuffer);
    glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[writePBO]);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        if (m_fence[writePBO]) glDeleteSync(m_fence[writePBO]);
        m_fence[writePBO] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        ++m_pendingReadbacks;
        m_lastSendAt = now;
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prevPackBuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFBO);
    glReadBuffer((GLenum)prevReadBuffer);

}

#endif // HAS_NDI
