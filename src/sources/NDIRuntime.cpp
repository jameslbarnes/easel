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

// Manually load NDI runtime DLL and get the v6 API struct
static const NDIlib_v6* loadNDIRuntime() {
#ifdef _WIN32
    // Try to find NDI runtime DLL
    const char* paths[] = {
        "Processing.NDI.Lib.x64.dll",
        "C:\\Program Files\\NDI\\NDI 6 Runtime\\Processing.NDI.Lib.x64.dll",
        "C:\\Program Files\\NDI\\NDI 5 Runtime\\Processing.NDI.Lib.x64.dll",
    };

    // Also check NDI_RUNTIME_DIR environment variable
    HMODULE hNDI = nullptr;
    char envBuf[512] = {};
    if (GetEnvironmentVariableA("NDI_RUNTIME_DIR_V6", envBuf, sizeof(envBuf)) > 0 ||
        GetEnvironmentVariableA("NDI_RUNTIME_DIR_V5", envBuf, sizeof(envBuf)) > 0) {
        std::string dllPath = std::string(envBuf) + "\\Processing.NDI.Lib.x64.dll";
        hNDI = LoadLibraryA(dllPath.c_str());
    }

    if (!hNDI) {
        for (const char* path : paths) {
            hNDI = LoadLibraryA(path);
            if (hNDI) break;
        }
    }

    if (!hNDI) {
        std::cout << "[NDI] Could not load Processing.NDI.Lib.x64.dll" << std::endl;
        return nullptr;
    }

    // Get the load function
    typedef const NDIlib_v6* (*NDIlib_v6_load_fn)(void);
    auto loadFn = (NDIlib_v6_load_fn)GetProcAddress(hNDI, "NDIlib_v6_load");
    if (!loadFn) {
        // Try v5
        typedef const NDIlib_v6* (*NDIlib_v5_load_fn)(void);
        loadFn = (NDIlib_v6_load_fn)GetProcAddress(hNDI, "NDIlib_v5_load");
    }

    if (!loadFn) {
        std::cout << "[NDI] Could not find NDIlib_v6_load in DLL" << std::endl;
        return nullptr;
    }

    return loadFn();
#else
    return nullptr;
#endif
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
    if (m_loaded && m_pApi) {
        m_pApi->destroy();
    }
    m_loaded = false;
    m_pApi = nullptr;
}

#endif // HAS_NDI
