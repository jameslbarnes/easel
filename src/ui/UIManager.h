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

private:
    void applyTheme(float dpiScale);

    GLFWwindow* m_window = nullptr;
    bool m_firstFrame = true;
    ImFont* m_smallFont = nullptr;
    ImFont* m_boldFont = nullptr;
};
