#pragma once

struct GLFWwindow;
struct ImFont;

class UIManager {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setupDockspace(float bottomBarHeight = 0);

    ImFont* smallFont() const { return m_smallFont; }
    ImFont* boldFont() const { return m_boldFont; }

    void handleZoom();  // call each frame to handle Cmd+/- zoom
    float uiScale() const { return m_uiZoom; }

private:
    void applyTheme(float dpiScale);

    GLFWwindow* m_window = nullptr;
    bool m_firstFrame = true;
    float m_lastDockW = 0;
    float m_lastDockH = 0;
    ImFont* m_smallFont = nullptr;
    ImFont* m_boldFont = nullptr;
    float m_uiZoom = 1.0f;
    float m_baseFontGlobalScale = 1.0f;
};
