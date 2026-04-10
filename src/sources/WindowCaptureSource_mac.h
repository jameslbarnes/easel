#pragma once
#ifdef __APPLE__
#include "sources/ContentSource.h"
#include "render/Texture.h"
#include <vector>
#include <string>
#include <cstdint>

struct WindowInfo {
    uint32_t windowID;
    std::string title;
    int width, height;
};

class WindowCaptureSource : public ContentSource {
public:
    ~WindowCaptureSource();

    static std::vector<WindowInfo> enumerateWindows();

    bool start(uint32_t windowID);
    void stop();

    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    std::string typeName() const override { return "Window Capture"; }
    std::string sourcePath() const override { return m_title; }

private:
    uint32_t m_windowID = 0;
    std::string m_title;
    int m_width = 0, m_height = 0;
    Texture m_texture;
    bool m_active = false;
    std::vector<uint8_t> m_pixelBuffer;
};
#endif
