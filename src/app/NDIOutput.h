#pragma once
#ifdef HAS_NDI

#include "sources/NDIRuntime.h"
#include <glad/glad.h>
#include <string>
#include <vector>

class NDIOutput {
public:
    ~NDIOutput();

    bool create(const std::string& name = "Easel");
    void destroy();
    bool isActive() const { return m_send != nullptr; }

    // Read back the warp FBO texture and send it over NDI.
    // Call this after compositeAndWarp() each frame.
    void send(GLuint texture, int w, int h);

private:
    NDIlib_send_instance_t m_send = nullptr;
    std::vector<uint8_t> m_pixelBuffer;
    int m_lastW = 0, m_lastH = 0;
};

#endif // HAS_NDI
