#include "app/Application.h"
#include "sources/ImageSource.h"
#ifdef HAS_FFMPEG
#include "sources/VideoSource.h"
#endif
#include "sources/CaptureSource.h"
#include "sources/WindowCaptureSource.h"
#include "sources/ShaderSource.h"
#ifdef HAS_NDI
#include "sources/NDIRuntime.h"
#include "sources/NDISource.h"
#endif
#include <nlohmann/json.hpp>
#include <imgui.h>
#include "stb_image_write.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#ifdef HAS_WEBVIEW2
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <objbase.h>
#endif

using json = nlohmann::json;

static void glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

static std::string openFileDialog(const char* filter) {
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        return filename;
    }
#endif
    return "";
}

static std::string saveFileDialog(const char* filter, const char* defaultExt) {
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        return filename;
    }
#endif
    return "";
}

bool Application::init() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, "Easel", nullptr, nullptr);
    if (!m_window) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD" << std::endl;
        return false;
    }

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;

    if (!m_ui.init(m_window)) return false;
    if (!m_compositor.init(1920, 1080)) return false;
    if (!m_cornerPin.init()) return false;
    if (!m_meshWarp.init(5, 5)) return false;
    if (!m_warpFBO.create(1920, 1080)) return false;

    m_quad.createQuad();
    if (!m_passthroughShader.loadFromFiles("shaders/passthrough.vert", "shaders/passthrough.frag")) {
        return false;
    }
    if (!m_maskRenderer.init()) return false;

#ifdef HAS_OPENCV
    m_scanner.init(1920, 1080);
#endif

    // Generate test pattern texture (visible when no layers are loaded)
    {
        const int tw = 512, th = 512;
        std::vector<uint8_t> pixels(tw * th * 4);
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                int idx = (y * tw + x) * 4;
                bool checker = (((x / 32) + (y / 32)) % 2 == 0);
                if (checker) {
                    pixels[idx + 0] = (uint8_t)(x * 255 / tw);
                    pixels[idx + 1] = (uint8_t)(y * 255 / th);
                    pixels[idx + 2] = 128;
                } else {
                    pixels[idx + 0] = 30;
                    pixels[idx + 1] = 30;
                    pixels[idx + 2] = 40;
                }
                pixels[idx + 3] = 255;
            }
        }
        m_testPattern.createEmpty(tw, th);
        m_testPattern.updateData(pixels.data(), tw, th);
    }

#ifdef HAS_WEBVIEW2
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_speechBridge.init(glfwGetWin32Window(m_window));
    m_speechBridge.setCallback([this](const std::string& text, bool isFinal) {
        if (m_speechState.targetSource && !m_speechState.targetParam.empty()) {
            m_speechState.targetSource->setText(m_speechState.targetParam, text);
        }
    });
    m_speechState.available = true;
#endif

    // Record initial monitor count and auto-connect if secondary exists
    m_lastMonitorCount = (int)ProjectorOutput::enumerateMonitors().size();
    if (m_projectorAutoConnect && m_lastMonitorCount > 1) {
        int sec = ProjectorOutput::findSecondaryMonitor(m_window);
        if (sec >= 0 && m_projector.create(m_window, sec)) {
            m_warpFBO.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
            m_compositor.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
        }
    }

#ifdef HAS_NDI
    NDIRuntime::instance().init();
    // Pre-populate NDI source list (non-blocking, short wait)
    if (NDIRuntime::instance().isAvailable()) {
        m_ndiSources = NDISource::findSources(500);
    }
#endif

    return true;
}

void Application::run() {
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        // Escape on main window closes projector
        if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS && m_projector.isActive()) {
            m_projector.destroy();
        }

        // Undo / Redo keybinds
        {
            static bool undoWasPressed = false;
            static bool redoWasPressed = false;
            bool ctrl = (glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                         glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
            bool shift = (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                          glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
            bool zNow = glfwGetKey(m_window, GLFW_KEY_Z) == GLFW_PRESS;
            bool yNow = glfwGetKey(m_window, GLFW_KEY_Y) == GLFW_PRESS;

            // Ctrl+Shift+Z or Ctrl+Y = redo
            bool redoNow = ctrl && ((zNow && shift) || yNow);
            // Ctrl+Z (without shift) = undo
            bool undoNow = ctrl && zNow && !shift;

            if (undoNow && !undoWasPressed) {
                m_undoStack.undo(m_layerStack, m_selectedLayer);
            }
            if (redoNow && !redoWasPressed) {
                m_undoStack.redo(m_layerStack, m_selectedLayer);
            }
            undoWasPressed = undoNow;
            redoWasPressed = redoNow;
        }

        // F12 = quick screenshot (auto-named to screenshots/ folder)
        {
            static bool f12WasPressed = false;
            bool f12Now = glfwGetKey(m_window, GLFW_KEY_F12) == GLFW_PRESS;
            if (f12Now && !f12WasPressed) {
                std::filesystem::create_directories("screenshots");
                auto t = std::time(nullptr);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
                captureScreenshot(std::string("screenshots/easel_") + buf + ".png");
            }
            f12WasPressed = f12Now;
        }

        int w, h;
        glfwGetFramebufferSize(m_window, &w, &h);
        if (w != m_windowWidth || h != m_windowHeight) {
            m_windowWidth = w;
            m_windowHeight = h;
        }

        // Auto-detect monitor hotplug
        if (m_projectorAutoConnect) {
            int monitorCount = (int)ProjectorOutput::enumerateMonitors().size();
            if (monitorCount != m_lastMonitorCount) {
                if (monitorCount > 1 && !m_projector.isActive()) {
                    int sec = ProjectorOutput::findSecondaryMonitor(m_window);
                    if (sec >= 0 && m_projector.create(m_window, sec)) {
                        m_warpFBO.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
                        m_compositor.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
                    }
                } else if (monitorCount <= 1 && m_projector.isActive()) {
                    // Secondary monitor disconnected - close projector
                    m_projector.destroy();
                }
                m_lastMonitorCount = monitorCount;
            }
        }

        updateSources();
        compositeAndWarp();

        if (m_projector.isActive()) {
            // Ensure main context finishes writing the texture before projector reads it
            glFinish();
            m_projector.present(m_warpFBO.textureId());
        }

#ifdef HAS_NDI
        if (m_ndiOutputEnabled && m_ndiOutput.isActive()) {
            m_ndiOutput.send(m_warpFBO.textureId(), m_warpFBO.width(), m_warpFBO.height());
        }
#endif

#ifdef HAS_FFMPEG
        if (m_rtmpOutput.isActive()) {
            m_rtmpOutput.sendFrame(m_warpFBO.textureId(), m_warpFBO.width(), m_warpFBO.height());
        }
        if (m_recorder.isActive()) {
            m_recorder.sendFrame(m_warpFBO.textureId(), m_warpFBO.width(), m_warpFBO.height());
        }
#endif

        glViewport(0, 0, m_windowWidth, m_windowHeight);
        glClearColor(0.055f, 0.063f, 0.082f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_ui.beginFrame();
        renderUI();
        m_ui.endFrame();

        // Check for agent screenshot trigger (after UI is rendered)
        pollScreenshotTrigger();

#ifdef HAS_WEBVIEW2
        // Sync speech bridge state with UI toggle
        if (m_speechState.listening && !m_speechBridge.isListening()) {
            m_speechBridge.startListening();
        } else if (!m_speechState.listening && m_speechBridge.isListening()) {
            m_speechBridge.stopListening();
        }
#endif

        glfwSwapBuffers(m_window);
    }
}

void Application::shutdown() {
#ifdef HAS_FFMPEG
    m_recorder.stop();
    m_rtmpOutput.stop();
#endif
#ifdef HAS_NDI
    // Destroy per-layer NDI senders before shutting down runtime
    for (int i = 0; i < m_layerStack.count(); i++) {
        m_layerStack[i]->ndiSender.destroy();
    }
    m_ndiOutput.destroy();
    NDIRuntime::instance().shutdown();
#endif
#ifdef HAS_WEBVIEW2
    m_speechBridge.shutdown();
    CoUninitialize();
#endif
#ifdef HAS_OPENCV
    m_scanner.cancelScan();
    m_webcam.close();
#endif
    m_projector.destroy();
    m_ui.shutdown();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Application::updateSources() {
    // Hot-reload any changed Shader-Claw shaders
    m_shaderClaw.update();

    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& layer = m_layerStack[i];
        if (layer->source) {
            layer->source->update();
        }

#ifdef HAS_NDI
        // Auto-manage per-layer NDI output
        if (NDIRuntime::instance().isAvailable()) {
            std::string expectedName = "Easel - " + layer->name;
            if (!layer->ndiSender.isActive() || layer->ndiName != expectedName) {
                layer->ndiSender.destroy();
                layer->ndiSender.create(expectedName);
                layer->ndiName = expectedName;
            }
            GLuint tex = layer->textureId();
            if (layer->ndiSender.isActive() && tex) {
                layer->ndiSender.send(tex, layer->width(), layer->height());
            }
        }
#endif
    }

#ifdef HAS_OPENCV
    if (m_webcam.isOpen()) {
        m_webcam.update();
        m_scanner.update(m_webcam);
    }
#endif
}

void Application::compositeAndWarp() {
#ifdef HAS_OPENCV
    // During scanning, display the scan pattern instead of the normal composite
    if (m_scanner.isScanning()) {
        GLuint patternTex = m_scanner.currentPatternTexture();
        if (patternTex) {
            m_warpFBO.bind();
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            // Render the pattern texture directly to warp FBO (no warp — patterns must be pixel-accurate)
            m_passthroughShader.use();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, patternTex);
            m_passthroughShader.setInt("uTexture", 0);
            m_quad.draw();
            Framebuffer::unbind();
            return;
        }
    }
#endif

    // Re-render any dirty masks before compositing
    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& layer = m_layerStack[i];
        if (layer->maskEnabled && layer->maskPath.isDirty() && layer->maskPath.count() >= 3) {
            if (!layer->mask) {
                layer->mask = std::make_shared<Texture>();
            }
            int mw = layer->width() > 0 ? layer->width() : 1024;
            int mh = layer->height() > 0 ? layer->height() : 1024;
            m_maskRenderer.render(layer->maskPath, mw, mh, *layer->mask);
            layer->maskPath.clearDirty();
        }
        if (!layer->maskEnabled) {
            layer->mask = nullptr;
        }
    }

    m_compositor.composite(m_layerStack);

    // Use composited result, or test pattern if no layers
    GLuint sourceTex = m_compositor.resultTexture();
    if (m_layerStack.empty()) {
        sourceTex = m_testPattern.id();
    }

    m_warpFBO.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (sourceTex) {
        if (m_warpMode == ViewportPanel::WarpMode::CornerPin) {
            m_cornerPin.render(sourceTex);
        } else {
            m_meshWarp.render(sourceTex);
        }
    }

    Framebuffer::unbind();
}

void Application::renderUI() {
    m_ui.setupDockspace();
    renderMenuBar();

    float projAspect = m_projector.isActive()
        ? m_projector.aspectRatio()
        : 16.0f / 9.0f;
    m_viewportPanel.render(m_warpFBO.textureId(), m_cornerPin, m_meshWarp, m_warpMode, projAspect);
    m_layerPanel.render(m_layerStack, m_selectedLayer);

    std::shared_ptr<Layer> selectedLayer;
    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
        selectedLayer = m_layerStack[m_selectedLayer];
    }
    m_propertyPanel.render(selectedLayer, m_maskEditMode, &m_speechState);

    // Push undo state if a property widget was just activated (before the edit takes effect next frame)
    if (m_propertyPanel.undoNeeded) {
        m_undoStack.pushState(m_layerStack, m_selectedLayer);
        m_propertyPanel.undoNeeded = false;
    }

    // Set viewport edit mode based on current editing state
    if (m_maskEditMode && selectedLayer && selectedLayer->maskEnabled) {
        m_viewportPanel.setEditMode(ViewportPanel::EditMode::Mask);
        m_viewportPanel.renderMaskOverlay(selectedLayer->maskPath);
    } else {
        m_viewportPanel.setEditMode(ViewportPanel::EditMode::Normal);
        m_viewportPanel.renderLayerOverlay(m_layerStack, m_selectedLayer);
        m_maskEditMode = false;
    }

    m_warpEditor.render(m_cornerPin, m_meshWarp, m_warpMode);

    // Projector panel
    ImGui::Begin("Projector");
    {
        auto monitors = ProjectorOutput::enumerateMonitors();

        ImGui::Checkbox("Auto-detect", &m_projectorAutoConnect);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("(?)");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically open projector when a\nsecond display is connected");
        }

        // Section line
        ImGui::Dummy(ImVec2(0, 4));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
        }
        ImGui::Dummy(ImVec2(0, 6));

        for (int i = 0; i < (int)monitors.size(); i++) {
            ImGui::PushID(i);
            bool isProjector = m_projector.isActive() && m_projector.monitorIndex() == i;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 rowPos = ImGui::GetCursorScreenPos();

            if (isProjector) {
                dl->AddCircleFilled(ImVec2(rowPos.x + 6, rowPos.y + ImGui::GetTextLineHeight() * 0.5f),
                                    3.5f, IM_COL32(34, 210, 130, 255));
                ImGui::Dummy(ImVec2(16, 0));
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                ImGui::Text("%s (%dx%d)", monitors[i].name.c_str(),
                            monitors[i].width, monitors[i].height);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                if (ImGui::SmallButton("Close")) {
                    m_projector.destroy();
                }
                ImGui::PopStyleColor(4);
            } else {
                dl->AddCircle(ImVec2(rowPos.x + 6, rowPos.y + ImGui::GetTextLineHeight() * 0.5f),
                              3.0f, IM_COL32(100, 110, 130, 160), 0, 1.2f);
                ImGui::Dummy(ImVec2(16, 0));
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::Text("%s (%dx%d)", monitors[i].name.c_str(),
                            monitors[i].width, monitors[i].height);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::SmallButton("Open")) {
                    if (m_projector.create(m_window, i)) {
                        m_warpFBO.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
                        m_compositor.resize(m_projector.projectorWidth(), m_projector.projectorHeight());
                    }
                }
                ImGui::PopStyleColor(4);
            }
            ImGui::PopID();
        }
    }
    ImGui::End();

    // Capture panel
    ImGui::Begin("Capture");
    {
        if (ImGui::CollapsingHeader("Screen Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto capMonitors = CaptureSource::enumerateMonitors();
            for (int i = 0; i < (int)capMonitors.size(); i++) {
                ImGui::PushID(i);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::Text("%s  %dx%d", capMonitors[i].name.c_str(),
                            capMonitors[i].width, capMonitors[i].height);
                ImGui::PopStyleColor();
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::Button("Add")) {
                    addScreenCapture(i);
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Window Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("Refresh Windows", ImVec2(-1, 0))) {
                m_windowList = WindowCaptureSource::enumerateWindows();
            }
            ImGui::PopStyleColor(4);

            if (m_windowList.empty()) {
                m_windowList = WindowCaptureSource::enumerateWindows();
            }

            ImGui::Dummy(ImVec2(0, 2));
            for (int i = 0; i < (int)m_windowList.size(); i++) {
                ImGui::PushID(1000 + i);

                // Truncate long window titles
                std::string title = m_windowList[i].title;
                if (title.length() > 40) title = title.substr(0, 37) + "...";

                ImGui::Text("%s", title.c_str());
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::Button("Add")) {
                    addWindowCapture(m_windowList[i].hwnd, m_windowList[i].title);
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }
    }
    ImGui::End();

    // ShaderClaw panel — shader browser + connection
    ImGui::Begin("ShaderClaw");
    {
        if (!m_shaderClaw.isConnected()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextWrapped("Connect to a Shader-Claw shaders directory to browse and hot-reload ISF shaders.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));

            // Auto-detect common locations
            static char scPath[512] = "";
            if (scPath[0] == '\0') {
                // Try common paths
                const char* home = getenv("USERPROFILE");
                if (home) {
                    std::string tryPath = std::string(home) + "\\Shader-Claw\\shaders";
                    if (std::filesystem::exists(tryPath)) {
                        strncpy(scPath, tryPath.c_str(), sizeof(scPath) - 1);
                    }
                }
            }

            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##SCPath", scPath, sizeof(scPath));

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("Connect", ImVec2(-1, 0))) {
                m_shaderClaw.connect(scPath);
            }
            ImGui::PopStyleColor(4);
        } else {
            // Connected — show shader browser
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            ImGui::Text("Connected");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("(%d shaders)", (int)m_shaderClaw.shaders().size());
            ImGui::PopStyleColor();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::SmallButton("Disconnect")) {
                m_shaderClaw.disconnect();
            }
            ImGui::PopStyleColor(4);

            // Refresh button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
                m_shaderClaw.refreshManifest();
            }
            ImGui::PopStyleColor(4);

            ImGui::Dummy(ImVec2(0, 4));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
            ImGui::Dummy(ImVec2(0, 6));

            // Shader list
            for (int i = 0; i < (int)m_shaderClaw.shaders().size(); i++) {
                const auto& shader = m_shaderClaw.shaders()[i];
                ImGui::PushID(2000 + i);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                ImGui::Text("%s", shader.title.c_str());
                ImGui::PopStyleColor();

                if (!shader.description.empty()) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.8f));
                    // Truncate long descriptions
                    std::string desc = shader.description;
                    if (desc.length() > 50) desc = desc.substr(0, 47) + "...";
                    ImGui::Text("- %s", desc.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::SmallButton("+")) {
                    loadShader(shader.fullPath);
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }
    }
    ImGui::End();

#ifdef HAS_NDI
    if (NDIRuntime::instance().isAvailable()) {
        ImGui::Begin("NDI");
        {
            // Output section
            if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!m_ndiOutput.isActive()) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                    if (ImGui::Button("Start NDI Output", ImVec2(-1, 0))) {
                        if (m_ndiOutput.create("Easel")) {
                            m_ndiOutputEnabled = true;
                        }
                    }
                    ImGui::PopStyleColor(4);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    ImGui::Text("Sending as \"Easel\"");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    if (ImGui::SmallButton("Stop")) {
                        m_ndiOutput.destroy();
                        m_ndiOutputEnabled = false;
                    }
                    ImGui::PopStyleColor(4);
                }
            }

            // Sources section
            if (ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
                    m_ndiSources = NDISource::findSources(1000);
                }
                ImGui::PopStyleColor(4);

                ImGui::Dummy(ImVec2(0, 2));
                if (m_ndiSources.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::TextWrapped("No NDI sources found. Click Refresh to scan the network.");
                    ImGui::PopStyleColor();
                }

                for (int i = 0; i < (int)m_ndiSources.size(); i++) {
                    ImGui::PushID(3000 + i);

                    std::string name = m_ndiSources[i].name;
                    if (name.length() > 40) name = name.substr(0, 37) + "...";

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                    ImGui::Text("%s", name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                    if (ImGui::SmallButton("Add")) {
                        addNDISource(m_ndiSources[i].name);
                    }
                    ImGui::PopStyleColor(4);

                    ImGui::PopID();
                }
            }
        }
        ImGui::End();
    }
#endif

#ifdef HAS_FFMPEG
    ImGui::Begin("Stream");
    {
        if (!m_rtmpOutput.isActive()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("YouTube Stream Key");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##StreamKey", m_streamKeyBuf, sizeof(m_streamKeyBuf),
                             ImGuiInputTextFlags_Password);

            // Aspect ratio selector
            static const char* aspectNames[] = { "16:9", "4:3", "16:10", "Source" };
            static const int aspectNums[]    = { 16, 4, 16, 0 };
            static const int aspectDens[]    = { 9,  3, 10, 0 };
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Aspect");
            ImGui::PopStyleColor();
            ImGui::SameLine(58);
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##Aspect", &m_streamAspect, aspectNames, 4);

            bool hasKey = m_streamKeyBuf[0] != '\0';
            if (!hasKey) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.05f, 0.05f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.15f, 0.15f, 0.65f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Go Live", ImVec2(-1, 0))) {
                m_rtmpOutput.start(m_streamKeyBuf,
                                   m_warpFBO.width(), m_warpFBO.height(),
                                   aspectNums[m_streamAspect],
                                   aspectDens[m_streamAspect], 30);
            }
            ImGui::PopStyleColor(4);
            if (!hasKey) ImGui::EndDisabled();
        } else {
            // Live indicator
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
            ImGui::Text("LIVE");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            int secs = (int)m_rtmpOutput.uptimeSeconds();
            ImGui::Text("%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);

            if (m_rtmpOutput.droppedFrames() > 0) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                ImGui::Text("(%d dropped)", m_rtmpOutput.droppedFrames());
                ImGui::PopStyleColor();
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::Button("End Stream", ImVec2(-1, 0))) {
                m_rtmpOutput.stop();
            }
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::Separator();

    // ── Record section ──
    if (!m_recorder.isActive()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.05f, 0.05f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.15f, 0.15f, 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Record", ImVec2(-1, 0))) {
            // Generate timestamped filename
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
            localtime_s(&tm_buf, &t);
            char fname[128];
            strftime(fname, sizeof(fname), "recordings/%Y%m%d_%H%M%S.mp4", &tm_buf);
            m_recorder.start(fname, m_warpFBO.width(), m_warpFBO.height(), 30);
        }
        ImGui::PopStyleColor(4);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
        ImGui::Text("REC");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        int secs = (int)m_recorder.uptimeSeconds();
        ImGui::Text("%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
        if (ImGui::Button("Stop Recording", ImVec2(-1, 0))) {
            m_recorder.stop();
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::End();
#endif

#ifdef HAS_OPENCV
    m_scanPanel.render(m_scanner, m_webcam);
#endif
}

void Application::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_undoStack.canUndo())) {
                m_undoStack.undo(m_layerStack, m_selectedLayer);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_undoStack.canRedo())) {
                m_undoStack.redo(m_layerStack, m_selectedLayer);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add Image Layer...")) {
                std::string path = openFileDialog(
                    "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0");
                if (!path.empty()) loadImage(path);
            }
#ifdef HAS_FFMPEG
            if (ImGui::MenuItem("Add Video Layer...")) {
                std::string path = openFileDialog(
                    "Videos\0*.mp4;*.avi;*.mkv;*.mov;*.webm\0All Files\0*.*\0");
                if (!path.empty()) loadVideo(path);
            }
#endif
            if (ImGui::MenuItem("Add Shader Layer...")) {
                std::string path = openFileDialog(
                    "ISF Shaders\0*.fs;*.frag;*.glsl\0All Files\0*.*\0");
                if (!path.empty()) loadShader(path);
            }
#ifdef HAS_NDI
            if (NDIRuntime::instance().isAvailable() && ImGui::BeginMenu("Add NDI Source")) {
                if (ImGui::MenuItem("Refresh")) {
                    m_ndiSources = NDISource::findSources(1000);
                }
                ImGui::Separator();
                if (m_ndiSources.empty()) {
                    ImGui::MenuItem("(no sources found)", nullptr, false, false);
                }
                for (int i = 0; i < (int)m_ndiSources.size(); i++) {
                    if (ImGui::MenuItem(m_ndiSources[i].name.c_str())) {
                        addNDISource(m_ndiSources[i].name);
                    }
                }
                ImGui::EndMenu();
            }
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Save Project...")) {
                std::string path = saveFileDialog("Easel Project\0*.easel\0", "easel");
                if (!path.empty()) saveProject(path);
            }
            if (ImGui::MenuItem("Load Project...")) {
                std::string path = openFileDialog("Easel Project\0*.easel\0All Files\0*.*\0");
                if (!path.empty()) loadProject(path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Screenshot...", "F12")) {
                std::string path = saveFileDialog("PNG Image\0*.png\0", "png");
                if (!path.empty()) captureScreenshot(path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Layer")) {
            if (ImGui::MenuItem("Remove Selected") && m_selectedLayer >= 0) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                m_layerStack.removeLayer(m_selectedLayer);
                m_selectedLayer = std::min(m_selectedLayer, m_layerStack.count() - 1);
            }
            if (ImGui::MenuItem("Move Up") && m_selectedLayer < m_layerStack.count() - 1) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                m_layerStack.moveLayer(m_selectedLayer, m_selectedLayer + 1);
                m_selectedLayer++;
            }
            if (ImGui::MenuItem("Move Down") && m_selectedLayer > 0) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                m_layerStack.moveLayer(m_selectedLayer, m_selectedLayer - 1);
                m_selectedLayer--;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void Application::loadImage(const std::string& path) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<ImageSource>();
    if (!source->load(path)) {
        std::cerr << "Failed to load image: " << path << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}

void Application::loadVideo(const std::string& path) {
#ifdef HAS_FFMPEG
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<VideoSource>();
    if (!source->load(path)) {
        std::cerr << "Failed to load video: " << path << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    source->play();
#else
    std::cerr << "Video support not available (FFmpeg not found)" << std::endl;
#endif
}

void Application::addScreenCapture(int monitorIndex) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<CaptureSource>();
    if (!source->start(monitorIndex)) {
        std::cerr << "Failed to start screen capture" << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    layer->name = "Screen Capture " + std::to_string(monitorIndex);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}

void Application::addWindowCapture(HWND hwnd, const std::string& title) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<WindowCaptureSource>();
    if (!source->start(hwnd)) {
        std::cerr << "Failed to capture window: " << title << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    layer->name = "Win: " + title;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}

void Application::loadShader(const std::string& path) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<ShaderSource>();
    if (!source->loadFromFile(path)) {
        std::cerr << "Failed to load shader: " << path << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;

    // Register with ShaderClaw bridge for hot-reload
    if (m_shaderClaw.isConnected()) {
        m_shaderClaw.watchSource(path, source);
    }
}

#ifdef HAS_NDI
void Application::addNDISource(const std::string& senderName) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<NDISource>();
    if (!source->connect(senderName)) {
        std::cerr << "Failed to connect to NDI source: " << senderName << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->source = source;
    // Extract just the sender name part (after "MACHINE/")
    size_t slash = senderName.find('/');
    layer->name = "NDI: " + ((slash != std::string::npos) ? senderName.substr(slash + 1) : senderName);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}
#endif

// --- Save/Load ---

void Application::saveProject(const std::string& path) {
    json j;

    // Save warp mode
    j["warpMode"] = (int)m_warpMode;

    // Save corner pin
    json corners = json::array();
    for (const auto& c : m_cornerPin.corners()) {
        corners.push_back({c.x, c.y});
    }
    j["cornerPin"] = corners;

    // Save mesh warp
    j["meshWarp"]["cols"] = m_meshWarp.cols();
    j["meshWarp"]["rows"] = m_meshWarp.rows();
    json meshPoints = json::array();
    for (const auto& p : m_meshWarp.points()) {
        meshPoints.push_back({p.x, p.y});
    }
    j["meshWarp"]["points"] = meshPoints;

    // Save layers
    json layers = json::array();
    for (int i = 0; i < m_layerStack.count(); i++) {
        const auto& layer = m_layerStack[i];
        json layerJson;
        layerJson["name"] = layer->name;
        layerJson["visible"] = layer->visible;
        layerJson["opacity"] = layer->opacity;
        layerJson["blendMode"] = (int)layer->blendMode;
        layerJson["position"] = {layer->position.x, layer->position.y};
        layerJson["scale"] = {layer->scale.x, layer->scale.y};
        layerJson["rotation"] = layer->rotation;
        layerJson["flipH"] = layer->flipH;
        layerJson["flipV"] = layer->flipV;

        if (layer->source) {
            layerJson["sourceType"] = layer->source->typeName();
            layerJson["sourcePath"] = layer->source->sourcePath();

            // Save shader parameters
            if (layer->source->isShader()) {
                auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
                json params = json::array();
                for (const auto& input : shaderSrc->inputs()) {
                    json p;
                    p["name"] = input.name;
                    p["type"] = input.type;
                    if (input.type == "float" || input.type == "long") {
                        p["value"] = std::get<float>(input.value);
                    } else if (input.type == "color") {
                        auto c = std::get<glm::vec4>(input.value);
                        p["value"] = {c.r, c.g, c.b, c.a};
                    } else if (input.type == "bool") {
                        p["value"] = std::get<bool>(input.value);
                    } else if (input.type == "point2D") {
                        auto v = std::get<glm::vec2>(input.value);
                        p["value"] = {v.x, v.y};
                    } else if (input.type == "text") {
                        p["value"] = std::get<std::string>(input.value);
                    }
                    params.push_back(p);
                }
                layerJson["shaderParams"] = params;
            }
        }

        layers.push_back(layerJson);
    }
    j["layers"] = layers;

    // Write to file
    std::ofstream file(path);
    if (file.is_open()) {
        file << j.dump(2);
        std::cout << "Project saved: " << path << std::endl;
    } else {
        std::cerr << "Failed to save project: " << path << std::endl;
    }
}

void Application::loadProject(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open project: " << path << std::endl;
        return;
    }

    json j;
    try {
        file >> j;
    } catch (const json::exception& e) {
        std::cerr << "Failed to parse project: " << e.what() << std::endl;
        return;
    }

    // Clear current state
    while (m_layerStack.count() > 0) {
        m_layerStack.removeLayer(0);
    }
    m_selectedLayer = -1;

    // Load warp mode
    if (j.contains("warpMode")) {
        m_warpMode = (ViewportPanel::WarpMode)j["warpMode"].get<int>();
    }

    // Load corner pin
    if (j.contains("cornerPin")) {
        auto& corners = m_cornerPin.corners();
        const auto& cj = j["cornerPin"];
        for (int i = 0; i < 4 && i < (int)cj.size(); i++) {
            corners[i] = {cj[i][0].get<float>(), cj[i][1].get<float>()};
        }
    }

    // Load mesh warp
    if (j.contains("meshWarp")) {
        int cols = j["meshWarp"]["cols"].get<int>();
        int rows = j["meshWarp"]["rows"].get<int>();
        m_meshWarp.setGridSize(cols, rows);

        if (j["meshWarp"].contains("points")) {
            auto& points = m_meshWarp.points();
            const auto& pj = j["meshWarp"]["points"];
            for (int i = 0; i < (int)pj.size() && i < (int)points.size(); i++) {
                points[i] = {pj[i][0].get<float>(), pj[i][1].get<float>()};
            }
        }
    }

    // Load layers
    if (j.contains("layers")) {
        for (const auto& layerJson : j["layers"]) {
            auto layer = std::make_shared<Layer>();
            layer->name = layerJson.value("name", "Layer");
            layer->visible = layerJson.value("visible", true);
            layer->opacity = layerJson.value("opacity", 1.0f);
            layer->blendMode = (BlendMode)layerJson.value("blendMode", 0);
            layer->rotation = layerJson.value("rotation", 0.0f);
            layer->flipH = layerJson.value("flipH", false);
            layer->flipV = layerJson.value("flipV", false);

            if (layerJson.contains("position")) {
                layer->position = {layerJson["position"][0].get<float>(),
                                   layerJson["position"][1].get<float>()};
            }
            if (layerJson.contains("scale")) {
                layer->scale = {layerJson["scale"][0].get<float>(),
                                layerJson["scale"][1].get<float>()};
            }

            std::string sourceType = layerJson.value("sourceType", "");
            std::string sourcePath = layerJson.value("sourcePath", "");

            if (sourceType == "Image" && !sourcePath.empty()) {
                auto src = std::make_shared<ImageSource>();
                if (src->load(sourcePath)) {
                    layer->source = src;
                }
#ifdef HAS_FFMPEG
            } else if (sourceType == "Video" && !sourcePath.empty()) {
                auto src = std::make_shared<VideoSource>();
                if (src->load(sourcePath)) {
                    layer->source = src;
                }
#endif
            } else if (sourceType == "Shader" && !sourcePath.empty()) {
                auto src = std::make_shared<ShaderSource>();
                if (src->loadFromFile(sourcePath)) {
                    // Restore saved parameter values
                    if (layerJson.contains("shaderParams")) {
                        for (const auto& p : layerJson["shaderParams"]) {
                            std::string pName = p.value("name", "");
                            std::string pType = p.value("type", "");
                            if (pType == "float" && p.contains("value")) {
                                src->setFloat(pName, p["value"].get<float>());
                            } else if (pType == "color" && p.contains("value") && p["value"].is_array()) {
                                auto& v = p["value"];
                                if (v.size() >= 4) {
                                    src->setColor(pName, {v[0].get<float>(), v[1].get<float>(),
                                                          v[2].get<float>(), v[3].get<float>()});
                                }
                            } else if (pType == "bool" && p.contains("value")) {
                                src->setBool(pName, p["value"].get<bool>());
                            } else if (pType == "point2D" && p.contains("value") && p["value"].is_array()) {
                                auto& v = p["value"];
                                if (v.size() >= 2) {
                                    src->setPoint2D(pName, {v[0].get<float>(), v[1].get<float>()});
                                }
                            } else if (pType == "text" && p.contains("value") && p["value"].is_string()) {
                                src->setText(pName, p["value"].get<std::string>());
                            }
                        }
                    }
                    layer->source = src;
                }
#ifdef HAS_NDI
            } else if (sourceType == "NDI" && !sourcePath.empty()) {
                auto src = std::make_shared<NDISource>();
                if (src->connect(sourcePath)) {
                    layer->source = src;
                }
#endif
            }

            if (layer->source) {
                m_layerStack.addLayer(layer);
            }
        }
    }

    if (m_layerStack.count() > 0) {
        m_selectedLayer = 0;
    }

    std::cout << "Project loaded: " << path << std::endl;
}

void Application::captureScreenshot(const std::string& path) {
    int w = m_warpFBO.width();
    int h = m_warpFBO.height();
    if (w <= 0 || h <= 0) {
        std::cerr << "Screenshot failed: no framebuffer" << std::endl;
        return;
    }

    std::vector<uint8_t> pixels(w * h * 4);
    glBindTexture(GL_TEXTURE_2D, m_warpFBO.textureId());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Flip vertically (OpenGL has origin at bottom-left)
    int stride = w * 4;
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < h / 2; y++) {
        uint8_t* top = pixels.data() + y * stride;
        uint8_t* bot = pixels.data() + (h - 1 - y) * stride;
        std::memcpy(row.data(), top, stride);
        std::memcpy(top, bot, stride);
        std::memcpy(bot, row.data(), stride);
    }

    if (stbi_write_png(path.c_str(), w, h, 4, pixels.data(), stride)) {
        std::cout << "Screenshot saved: " << path << std::endl;
    } else {
        std::cerr << "Screenshot failed to write: " << path << std::endl;
    }
}

void Application::captureWindow(const std::string& path) {
    int w = m_windowWidth;
    int h = m_windowHeight;
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> pixels(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Flip vertically
    int stride = w * 4;
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < h / 2; y++) {
        uint8_t* top = pixels.data() + y * stride;
        uint8_t* bot = pixels.data() + (h - 1 - y) * stride;
        std::memcpy(row.data(), top, stride);
        std::memcpy(top, bot, stride);
        std::memcpy(bot, row.data(), stride);
    }

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    if (stbi_write_png(path.c_str(), w, h, 4, pixels.data(), stride)) {
        std::cout << "Window screenshot saved: " << path << std::endl;
    } else {
        std::cerr << "Window screenshot failed: " << path << std::endl;
    }
}

void Application::pollScreenshotTrigger() {
    const std::string trigger = "screenshots/.capture";
    if (std::filesystem::exists(trigger)) {
        std::filesystem::remove(trigger);
        std::filesystem::create_directories("screenshots");
        captureWindow("screenshots/claude_capture.png");
    }
}
