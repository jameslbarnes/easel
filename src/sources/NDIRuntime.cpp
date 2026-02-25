#ifdef HAS_NDI
#include "sources/NDIRuntime.h"
#include <iostream>

NDIRuntime& NDIRuntime::instance() {
    static NDIRuntime s;
    return s;
}

bool NDIRuntime::init() {
    if (m_initialized) return m_loaded;
    m_initialized = true;

    if (!NDIlib_load(&m_api)) {
        std::cout << "[NDI] Runtime not found (NDI Tools not installed?)" << std::endl;
        return false;
    }

    if (!m_api.initialize()) {
        std::cerr << "[NDI] Failed to initialize" << std::endl;
        memset(&m_api, 0, sizeof(m_api));
        return false;
    }

    m_loaded = true;
    std::cout << "[NDI] Runtime loaded successfully" << std::endl;
    return true;
}

void NDIRuntime::shutdown() {
    if (m_loaded && m_api.destroy) {
        m_api.destroy();
    }
    memset(&m_api, 0, sizeof(m_api));
    m_loaded = false;
    m_initialized = false;
}

#endif // HAS_NDI
