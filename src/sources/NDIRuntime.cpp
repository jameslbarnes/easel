#ifdef HAS_NDI

// Force static declaration so NDI functions are plain extern "C" (not dllimport).
// We load the NDI DLL manually at runtime via LoadLibrary/GetProcAddress.
#define PROCESSINGNDILIB_STATIC

#include "sources/NDIRuntime.h"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

NDIRuntime& NDIRuntime::instance() {
    static NDIRuntime s;
    return s;
}

// Load NDI runtime via our custom dynamic loader (Processing.NDI.DynamicLoad.h)
static NDIlib_api s_ndiApi;

static const NDIlib_api* loadNDIRuntime() {
    if (NDIlib_load(&s_ndiApi)) {
        return &s_ndiApi;
    }
    return nullptr;
}

bool NDIRuntime::init() {
    if (m_initialized) return m_loaded;
    m_initialized = true;

    m_pApi = loadNDIRuntime();
    if (!m_pApi) {
        std::cout << "[NDI] Runtime not found (NDI Tools not installed?)" << std::endl;
        return false;
    }

    if (!m_pApi->initialize()) {
        std::cerr << "[NDI] Failed to initialize" << std::endl;
        m_pApi = nullptr;
        return false;
    }

    m_loaded = true;
    std::cout << "[NDI] Runtime loaded successfully" << std::endl;
    return true;
}

void NDIRuntime::shutdown() {
    if (m_loaded && m_pApi && m_pApi->destroy) {
        m_pApi->destroy();
    }
    m_pApi = nullptr;
    m_loaded = false;
    m_initialized = false;
}

#endif // HAS_NDI
