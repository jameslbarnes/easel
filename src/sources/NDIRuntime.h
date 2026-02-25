#pragma once
#ifdef HAS_NDI

#include <Processing.NDI.Lib.h>

// Singleton that manages the NDI runtime lifecycle.
// Dynamically loads the NDI DLL and provides access to function pointers.
class NDIRuntime {
public:
    static NDIRuntime& instance();

    bool isAvailable() const { return m_loaded; }
    const NDIlib_api* api() const { return m_loaded ? &m_api : nullptr; }

    bool init();
    void shutdown();

private:
    NDIRuntime() = default;
    ~NDIRuntime() = default;
    NDIRuntime(const NDIRuntime&) = delete;
    NDIRuntime& operator=(const NDIRuntime&) = delete;

    NDIlib_api m_api = {};
    bool m_loaded = false;
    bool m_initialized = false;
};

#endif // HAS_NDI
