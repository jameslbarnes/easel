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
            // Send nullptr to flush any async frame
            rt.api()->send_send_video_async_v2(m_send, nullptr);
            rt.api()->send_destroy(m_send);
        }
        m_send = nullptr;
    }
    m_pixelBuffer.clear();
    m_lastW = 0;
    m_lastH = 0;
}

void NDIOutput::send(GLuint texture, int w, int h) {
    if (!m_send || w <= 0 || h <= 0) return;

    // Resize pixel buffer if needed
    if (w != m_lastW || h != m_lastH) {
        m_pixelBuffer.resize(w * h * 4);
        m_lastW = w;
        m_lastH = h;
    }

    // Read back texture as BGRA (NDI native format, avoids swizzle on their side)
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, m_pixelBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Flip vertically (OpenGL bottom-up → NDI top-down)
    int stride = w * 4;
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < h / 2; y++) {
        uint8_t* top = m_pixelBuffer.data() + y * stride;
        uint8_t* bot = m_pixelBuffer.data() + (h - 1 - y) * stride;
        std::memcpy(row.data(), top, stride);
        std::memcpy(top, bot, stride);
        std::memcpy(bot, row.data(), stride);
    }

    // Build and send the frame
    NDIlib_video_frame_v2_t frame = {};
    frame.xres = w;
    frame.yres = h;
    frame.FourCC = NDIlib_FourCC_video_type_BGRA;
    frame.frame_rate_N = 60000;
    frame.frame_rate_D = 1001;
    frame.picture_aspect_ratio = (float)w / (float)h;
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.p_data = m_pixelBuffer.data();
    frame.line_stride_in_bytes = stride;

    auto& rt = NDIRuntime::instance();
    rt.api()->send_send_video_v2(m_send, &frame);
}

#endif // HAS_NDI
