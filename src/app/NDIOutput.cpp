#ifdef HAS_NDI
#include "app/NDIOutput.h"
#include <iostream>
#include <cstring>

NDIOutput::~NDIOutput() {
    destroy();
}

bool NDIOutput::create(const std::string& name) {
    destroy();

    auto& rt = NDIRuntime::instance();
    if (!rt.isAvailable()) return false;

    NDIlib_send_create_t sendCreate = {};
    sendCreate.p_ndi_name = name.c_str();
    sendCreate.clock_video = false;  // We drive timing from our render loop
    sendCreate.clock_audio = false;

    m_send = rt.api()->send_create(&sendCreate);
    if (!m_send) {
        std::cerr << "[NDI] Failed to create sender" << std::endl;
        return false;
    }

    std::cout << "[NDI] Sender created: " << name << std::endl;
    return true;
}

void NDIOutput::destroy() {
    if (m_send) {
        auto& rt = NDIRuntime::instance();
        if (rt.isAvailable()) {
            // Flush async frame
            rt.api()->send_send_video_async_v2(m_send, nullptr);
            rt.api()->send_destroy(m_send);
        }
        m_send = nullptr;
    }
    if (m_pbo[0]) { glDeleteBuffers(2, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
    m_pixelBuffer[0].clear();
    m_pixelBuffer[1].clear();
    m_lastW = 0;
    m_lastH = 0;
    m_pboReady = false;
    m_pboIndex = 0;
    m_bufferIndex = 0;
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

    // Resize buffers if dimensions changed
    if (w != m_lastW || h != m_lastH) {
        m_pixelBuffer[0].resize(bytes);
        m_pixelBuffer[1].resize(bytes);
        m_lastW = w;
        m_lastH = h;
        m_pboReady = false;

        // (Re)create PBOs
        if (m_pbo[0]) glDeleteBuffers(2, m_pbo);
        glGenBuffers(2, m_pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pboIndex = 0;
    }

    int readPBO = m_pboIndex;
    int mapPBO = 1 - m_pboIndex;

    // Step 1: Kick off async readback of current frame into readPBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readPBO]);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Step 2: Map the OTHER PBO (previous frame) — this one is already done
    if (m_pboReady) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[mapPBO]);
        void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (ptr) {
            // Straight copy — readback FBO is already vertically flipped
            auto& buf = m_pixelBuffer[m_bufferIndex];
            std::memcpy(buf.data(), ptr, bytes);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            // Send asynchronously — NDI holds the buffer until next async call
            NDIlib_video_frame_v2_t frame = {};
            frame.xres = w;
            frame.yres = h;
            frame.FourCC = NDIlib_FourCC_video_type_BGRA;
            frame.frame_rate_N = 60000;
            frame.frame_rate_D = 1001;
            frame.picture_aspect_ratio = (float)w / (float)h;
            frame.frame_format_type = NDIlib_frame_format_type_progressive;
            frame.p_data = buf.data();
            frame.line_stride_in_bytes = w * 4;

            rt.api()->send_send_video_async_v2(m_send, &frame);

            // Swap send buffer so next frame writes to the other one
            m_bufferIndex = 1 - m_bufferIndex;
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Swap PBO index for next frame
    m_pboIndex = 1 - m_pboIndex;
    m_pboReady = true;
}

#endif // HAS_NDI
