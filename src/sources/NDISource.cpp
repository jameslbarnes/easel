#ifdef HAS_NDI
#include "sources/NDISource.h"
#include <iostream>
#include <cstring>

NDISource::~NDISource() {
    disconnect();
}

std::vector<NDISenderInfo> NDISource::findSources(uint32_t waitMs) {
    std::vector<NDISenderInfo> result;

    auto& rt = NDIRuntime::instance();
    if (!rt.isAvailable()) return result;

    NDIlib_find_create_t findCreate = {};
    findCreate.show_local_sources = true;

    NDIlib_find_instance_t finder = rt.api()->find_create_v2(&findCreate);
    if (!finder) return result;

    // Wait for sources to appear
    rt.api()->find_wait_for_sources(finder, waitMs);

    uint32_t count = 0;
    const NDIlib_source_t* sources = rt.api()->find_get_current_sources(finder, &count);

    for (uint32_t i = 0; i < count; i++) {
        NDISenderInfo info;
        info.name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
        info.url = sources[i].p_url_address ? sources[i].p_url_address : "";
        result.push_back(info);
    }

    rt.api()->find_destroy(finder);
    return result;
}

bool NDISource::connect(const std::string& senderName) {
    disconnect();

    auto& rt = NDIRuntime::instance();
    if (!rt.isAvailable()) return false;

    // Create receiver requesting RGBA format
    NDIlib_recv_create_v3_t recvCreate = {};
    recvCreate.color_format = NDIlib_recv_color_format_RGBX_RGBA;
    recvCreate.bandwidth = NDIlib_recv_bandwidth_highest;
    recvCreate.allow_video_fields = true;
    recvCreate.p_ndi_recv_name = "Easel";

    m_recv = rt.api()->recv_create_v3(&recvCreate);
    if (!m_recv) {
        std::cerr << "[NDI] Failed to create receiver" << std::endl;
        return false;
    }

    // Connect to the named source
    NDIlib_source_t source = {};
    source.p_ndi_name = senderName.c_str();
    rt.api()->recv_connect(m_recv, &source);

    m_senderName = senderName;
    std::cout << "[NDI] Connected to: " << senderName << std::endl;
    return true;
}

void NDISource::disconnect() {
    if (m_recv) {
        auto& rt = NDIRuntime::instance();
        if (rt.isAvailable()) {
            rt.api()->recv_destroy(m_recv);
        }
        m_recv = nullptr;
    }
    m_senderName.clear();
    m_width = 0;
    m_height = 0;
}

void NDISource::update() {
    if (!m_recv) return;

    auto& rt = NDIRuntime::instance();
    if (!rt.isAvailable()) return;

    NDIlib_video_frame_v2_t video = {};
    NDIlib_audio_frame_v3_t audio = {};
    NDIlib_metadata_frame_t meta = {};

    // Non-blocking capture
    NDIlib_frame_type_e type = rt.api()->recv_capture_v3(m_recv, &video, &audio, &meta, 0);

    if (type == NDIlib_frame_type_video && video.p_data) {
        // Resize texture if dimensions changed
        if (video.xres != m_width || video.yres != m_height) {
            m_width = video.xres;
            m_height = video.yres;
            m_texture.createEmpty(m_width, m_height);
            m_pixelBuffer.resize(m_width * m_height * 4);
        }

        int stride = video.line_stride_in_bytes;
        if (stride <= 0) stride = m_width * 4;

        // NDI gives us top-down, OpenGL wants bottom-up → flip vertically
        // Format is RGBA (we requested RGBX_RGBA)
        for (int y = 0; y < m_height; y++) {
            const uint8_t* srcRow = video.p_data + y * stride;
            uint8_t* dstRow = m_pixelBuffer.data() + (m_height - 1 - y) * m_width * 4;
            std::memcpy(dstRow, srcRow, m_width * 4);
        }

        m_texture.updateData(m_pixelBuffer.data(), m_width, m_height);
        rt.api()->recv_free_video_v2(m_recv, &video);
    }

    // Free any audio/metadata we received (we don't use them)
    if (type == NDIlib_frame_type_audio) {
        rt.api()->recv_free_audio_v3(m_recv, &audio);
    }
    if (type == NDIlib_frame_type_metadata) {
        rt.api()->recv_free_metadata(m_recv, &meta);
    }
}

#endif // HAS_NDI
