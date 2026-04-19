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
    bool hasReceivers() const;

    // Read back the warp FBO texture and send it over NDI.
    // Call this after compositeAndWarp() each frame.
    void send(GLuint texture, int w, int h);

private:
    NDIlib_send_instance_t m_send = nullptr;
    // Double-buffer for async send: one buffer is owned by NDI while we fill the other
    std::vector<uint8_t> m_pixelBuffer[2];
    int m_bufferIndex = 0;
    int m_lastW = 0, m_lastH = 0;

    // PBO for async GPU readback (avoids pipeline stall)
    GLuint m_pbo[2] = {0, 0};
    int m_pboIndex = 0;
    bool m_pboReady = false;  // first frame hasn't been read yet
};

#endif // HAS_NDI
