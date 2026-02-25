#pragma once
#include "sources/ContentSource.h"
#include "render/Texture.h"

#include <windows.h>
#include <vector>
#include <string>

struct WindowInfo {
    HWND hwnd;
    std::string title;
    int width, height;
};

class WindowCaptureSource : public ContentSource {
public:
    ~WindowCaptureSource();

    static std::vector<WindowInfo> enumerateWindows();

    bool start(HWND hwnd);
    void stop();

    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_width; }
    int height() const override { return m_height - m_cropTop; }
    std::string typeName() const override { return "Window Capture"; }
    std::string sourcePath() const override { return m_title; }

private:
    HWND m_hwnd = nullptr;
    std::string m_title;
    int m_width = 0, m_height = 0;
    int m_cropTop = 0; // auto-detected content offset (for Chromium windows)
    Texture m_texture;
    bool m_active = false;

    HDC m_windowDC = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_bitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;
    std::vector<uint8_t> m_pixelBuffer;

    void cleanup();
};
