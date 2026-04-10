#pragma once
#ifdef HAS_NDI

// Force static declaration so NDI functions are plain extern "C" (not dllimport).
#ifndef PROCESSINGNDILIB_STATIC
#define PROCESSINGNDILIB_STATIC
#endif

#include <cstddef>  // for NULL — NDI headers use it without including it
#include <Processing.NDI.Lib.h>

// NDI SDK 6 uses NDIlib_v6_3 as the API struct; earlier code used "NDIlib_api"
typedef NDIlib_v6_3 NDIlib_api;

// Singleton that manages the NDI runtime lifecycle.
// Dynamically loads the NDI DLL and provides access to function pointers.
class NDIRuntime {
public:
    static NDIRuntime& instance();

    bool isAvailable() const { return m_loaded; }
    const NDIlib_api* api() const { return m_loaded ? m_api : nullptr; }

    bool init();
    void shutdown();

private:
    NDIRuntime() = default;
    ~NDIRuntime() = default;
    NDIRuntime(const NDIRuntime&) = delete;
    NDIRuntime& operator=(const NDIRuntime&) = delete;

    const NDIlib_api* m_api = nullptr;
    bool m_loaded = false;
    bool m_initialized = false;
};

#endif // HAS_NDI
