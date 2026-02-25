#include "app/ProjectorOutput.h"
#include <iostream>

ProjectorOutput::~ProjectorOutput() {
    destroy();
}

std::vector<MonitorInfo> ProjectorOutput::enumerateMonitors() {
    std::vector<MonitorInfo> result;
    int count;
    GLFWmonitor** monitors = glfwGetMonitors(&count);

    for (int i = 0; i < count; i++) {
        MonitorInfo info;
        info.monitor = monitors[i];
        info.name = glfwGetMonitorName(monitors[i]);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        glfwGetMonitorPos(monitors[i], &info.x, &info.y);
        info.width = mode->width;
        info.height = mode->height;
        result.push_back(info);
    }

    return result;
}

int ProjectorOutput::findSecondaryMonitor(GLFWwindow* mainWindow) {
    auto monitors = enumerateMonitors();
    if (monitors.size() < 2) return -1;

    // Figure out which monitor the main window is on
    int mainX, mainY;
    glfwGetWindowPos(mainWindow, &mainX, &mainY);

    int primaryIndex = 0;
    for (int i = 0; i < (int)monitors.size(); i++) {
        const auto& m = monitors[i];
        if (mainX >= m.x && mainX < m.x + m.width &&
            mainY >= m.y && mainY < m.y + m.height) {
            primaryIndex = i;
            break;
        }
    }

    // Return the first monitor that ISN'T the one the main window is on
    for (int i = 0; i < (int)monitors.size(); i++) {
        if (i != primaryIndex) return i;
    }

    return -1;
}

bool ProjectorOutput::create(GLFWwindow* mainWindow, int monitorIndex) {
    destroy();

    auto monitors = enumerateMonitors();
    if (monitorIndex < 0 || monitorIndex >= (int)monitors.size()) {
        std::cerr << "Invalid monitor index " << monitorIndex << std::endl;
        return false;
    }

    // Safety: refuse to open on the same monitor as the main window
    int mainX, mainY;
    glfwGetWindowPos(mainWindow, &mainX, &mainY);
    const auto& target = monitors[monitorIndex];
    if (mainX >= target.x && mainX < target.x + target.width &&
        mainY >= target.y && mainY < target.y + target.height) {
        std::cerr << "Refusing to open projector on the same monitor as the editor" << std::endl;

        // Try to find a different monitor instead
        int alt = findSecondaryMonitor(mainWindow);
        if (alt < 0) {
            std::cerr << "No secondary monitor available" << std::endl;
            return false;
        }
        monitorIndex = alt;
    }

    const auto& mi = monitors[monitorIndex];
    m_mainWindow = mainWindow;
    m_monitorIndex = monitorIndex;
    m_width = mi.width;
    m_height = mi.height;

    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    // NOT floating/always-on-top — that traps users if it opens on the wrong screen

    m_window = glfwCreateWindow(mi.width, mi.height, "Easel Projector", nullptr, mainWindow);

    // Reset hints immediately
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);

    if (!m_window) {
        std::cerr << "Failed to create projector window" << std::endl;
        return false;
    }

    // Position on the target monitor
    glfwSetWindowPos(m_window, mi.x, mi.y);
    glfwSetWindowSize(m_window, mi.width, mi.height);

    // Escape key closes the projector window
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, [](GLFWwindow* win, int key, int, int action, int) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            auto* self = (ProjectorOutput*)glfwGetWindowUserPointer(win);
            if (self) self->requestClose();
        }
    });

    // Create GL resources in the projector context (VAOs aren't shared)
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(0);

    m_quad.createQuad();
    if (!m_shader.loadFromFiles("shaders/passthrough.vert", "shaders/passthrough.frag")) {
        std::cerr << "Failed to load projector shader" << std::endl;
        glfwMakeContextCurrent(mainWindow);
        destroy();
        return false;
    }

    glfwMakeContextCurrent(mainWindow);

    std::cout << "Projector opened on monitor " << monitorIndex
              << " (" << mi.width << "x" << mi.height << ")" << std::endl;

    return true;
}

void ProjectorOutput::destroy() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        std::cout << "Projector closed" << std::endl;
    }
    m_monitorIndex = -1;
    m_closeRequested = false;
}

void ProjectorOutput::present(GLuint texture) {
    if (!m_window || !texture) return;

    // Check if close was requested (e.g. Escape pressed on projector window)
    if (m_closeRequested) {
        destroy();
        return;
    }

    glfwMakeContextCurrent(m_window);

    int fbW, fbH;
    glfwGetFramebufferSize(m_window, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    m_shader.use();
    m_shader.setInt("uTexture", 0);
    m_shader.setFloat("uOpacity", 1.0f);
    m_shader.setMat3("uTransform", glm::mat3(1.0f));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    m_quad.draw();

    glfwSwapBuffers(m_window);
    glfwMakeContextCurrent(m_mainWindow);
}
