#pragma once
#include "render/Mesh.h"
#include "render/ShaderProgram.h"
#include <GLFW/glfw3.h>
#include <string>
#include <vector>

struct MonitorInfo {
    GLFWmonitor* monitor;
    std::string name;
    int x, y, width, height;
};

class ProjectorOutput {
public:
    ~ProjectorOutput();

    static std::vector<MonitorInfo> enumerateMonitors();

    // Find a monitor that isn't the one the main window is on. Returns -1 if none.
    static int findSecondaryMonitor(GLFWwindow* mainWindow);

    // Create fullscreen window on specified monitor, sharing context with mainWindow.
    // Will refuse to open on the same monitor as mainWindow.
    bool create(GLFWwindow* mainWindow, int monitorIndex);
    void destroy();

    void present(GLuint texture);

    void requestClose() { m_closeRequested = true; }

    bool isActive() const { return m_window != nullptr; }
    GLFWwindow* window() const { return m_window; }
    int monitorIndex() const { return m_monitorIndex; }
    int projectorWidth() const { return m_width; }
    int projectorHeight() const { return m_height; }
    float aspectRatio() const { return m_height > 0 ? (float)m_width / m_height : 16.0f / 9.0f; }

private:
    GLFWwindow* m_window = nullptr;
    GLFWwindow* m_mainWindow = nullptr;
    int m_monitorIndex = -1;
    int m_width = 0, m_height = 0;
    bool m_closeRequested = false;

    Mesh m_quad;
    ShaderProgram m_shader;
};
