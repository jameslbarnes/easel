#include "sources/WindowCaptureSource.h"
#include <iostream>

WindowCaptureSource::~WindowCaptureSource() {
    stop();
}

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = (std::vector<WindowInfo>*)lParam;

    if (!IsWindowVisible(hwnd)) return TRUE;

    // Skip windows with no title
    int len = GetWindowTextLengthA(hwnd);
    if (len == 0) return TRUE;

    // Skip tiny windows
    RECT rect;
    GetClientRect(hwnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w < 50 || h < 50) return TRUE;

    // Skip our own windows (Easel)
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    std::string titleStr(title);
    if (titleStr == "Easel" || titleStr == "Easel Projector") return TRUE;

    // Skip taskbar, shell, etc.
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = titleStr;
    info.width = w;
    info.height = h;
    windows->push_back(info);

    return TRUE;
}

std::vector<WindowInfo> WindowCaptureSource::enumerateWindows() {
    std::vector<WindowInfo> result;
    EnumWindows(enumWindowsProc, (LPARAM)&result);
    return result;
}

bool WindowCaptureSource::start(HWND hwnd) {
    stop();

    if (!IsWindow(hwnd)) {
        std::cerr << "Invalid window handle" << std::endl;
        return false;
    }

    m_hwnd = hwnd;

    // Get window title
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    m_title = title;

    // Get client area size
    RECT rect;
    GetClientRect(hwnd, &rect);
    m_width = rect.right - rect.left;
    m_height = rect.bottom - rect.top;

    if (m_width <= 0 || m_height <= 0) {
        std::cerr << "Window has no client area" << std::endl;
        return false;
    }

    // Create GDI resources for capture
    m_windowDC = GetDC(hwnd);
    if (!m_windowDC) {
        std::cerr << "Failed to get window DC" << std::endl;
        cleanup();
        return false;
    }

    m_memDC = CreateCompatibleDC(m_windowDC);
    m_bitmap = CreateCompatibleBitmap(m_windowDC, m_width, m_height);
    m_oldBitmap = (HBITMAP)SelectObject(m_memDC, m_bitmap);

    // Detect Chromium content child to auto-crop toolbar
    m_cropTop = 0;
    HWND contentChild = FindWindowExA(hwnd, nullptr, "Chrome_RenderWidgetHostHWND", nullptr);
    if (contentChild && IsWindowVisible(contentChild)) {
        RECT childRect, parentRect;
        GetWindowRect(contentChild, &childRect);
        GetWindowRect(hwnd, &parentRect);
        int crop = childRect.top - parentRect.top;
        if (crop > 0 && crop < m_height / 2)
            m_cropTop = crop;
    }

    m_pixelBuffer.resize(m_width * m_height * 4);
    m_texture.createEmpty(m_width, m_height - m_cropTop);

    m_active = true;
    std::cout << "Capturing window: " << m_title << " (" << m_width << "x" << (m_height - m_cropTop) << ")" << std::endl;
    return true;
}

void WindowCaptureSource::stop() {
    m_active = false;
    cleanup();
}

void WindowCaptureSource::cleanup() {
    if (m_memDC) {
        if (m_oldBitmap) SelectObject(m_memDC, m_oldBitmap);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
        m_oldBitmap = nullptr;
    }
    if (m_bitmap) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    if (m_windowDC && m_hwnd) {
        ReleaseDC(m_hwnd, m_windowDC);
        m_windowDC = nullptr;
    }
}

void WindowCaptureSource::update() {
    if (!m_active || !m_hwnd) return;

    // Check if window still exists
    if (!IsWindow(m_hwnd)) {
        std::cerr << "Captured window was closed" << std::endl;
        m_active = false;
        return;
    }

    // Recheck crop (toolbar can change, e.g. bookmarks bar toggle)
    int newCrop = 0;
    HWND contentChild = FindWindowExA(m_hwnd, nullptr, "Chrome_RenderWidgetHostHWND", nullptr);
    if (contentChild && IsWindowVisible(contentChild)) {
        RECT childRect, parentRect;
        GetWindowRect(contentChild, &childRect);
        GetWindowRect(m_hwnd, &parentRect);
        int crop = childRect.top - parentRect.top;
        if (crop > 0 && crop < m_height / 2)
            newCrop = crop;
    }

    // Check for resize or crop change
    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);
    int newW = clientRect.right - clientRect.left;
    int newH = clientRect.bottom - clientRect.top;

    if (newW != m_width || newH != m_height || newCrop != m_cropTop) {
        m_width = newW;
        m_height = newH;
        m_cropTop = newCrop;
        if (m_width <= 0 || m_height <= 0) return;

        SelectObject(m_memDC, m_oldBitmap);
        DeleteObject(m_bitmap);
        m_bitmap = CreateCompatibleBitmap(m_windowDC, m_width, m_height);
        m_oldBitmap = (HBITMAP)SelectObject(m_memDC, m_bitmap);

        m_pixelBuffer.resize(m_width * m_height * 4);
        m_texture.createEmpty(m_width, m_height - m_cropTop);
    }

    // Capture client area only (works even when occluded)
    if (!PrintWindow(m_hwnd, m_memDC, PW_RENDERFULLCONTENT | PW_CLIENTONLY)) {
        // Fallback to BitBlt from client DC
        BitBlt(m_memDC, 0, 0, m_width, m_height, m_windowDC, 0, 0, SRCCOPY);
    }

    // Read pixels from bitmap
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = m_width;
    bi.biHeight = m_height; // positive = bottom-up (matches OpenGL)
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(m_memDC, m_bitmap, 0, m_height, m_pixelBuffer.data(),
              (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    // BGRA to RGBA conversion
    // Bitmap is bottom-up: row 0 = bottom of image, last row = top.
    // To crop the top N pixels, we only process the first (height - cropTop) rows.
    int outH = m_height - m_cropTop;
    uint8_t* px = m_pixelBuffer.data();
    int total = m_width * outH;
    for (int i = 0; i < total; i++) {
        uint8_t tmp = px[i * 4 + 0];
        px[i * 4 + 0] = px[i * 4 + 2];
        px[i * 4 + 2] = tmp;
        px[i * 4 + 3] = 255;
    }

    m_texture.updateData(m_pixelBuffer.data(), m_width, outH);
}
