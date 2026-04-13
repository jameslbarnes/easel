#include "app/Application.h"
#include "sources/ImageSource.h"
#ifdef HAS_FFMPEG
#include "sources/VideoSource.h"
#endif
#ifdef _WIN32
#include "sources/CaptureSource.h"
#include "sources/WindowCaptureSource.h"
#elif defined(__APPLE__)
#include "sources/CaptureSource_mac.h"
#include "sources/WindowCaptureSource_mac.h"
#endif
#include "sources/ShaderSource.h"
#ifdef HAS_NDI
#include "sources/NDIRuntime.h"
#include "sources/NDISource.h"
#endif
#ifdef HAS_WHEP
#include "sources/WHEPSource.h"
#endif
#include <nlohmann/json.hpp>
#include <imgui.h>
#include "stb_image.h"
#include "stb_image_write.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

// WhisperSpeech uses <filesystem> (already included above)

using json = nlohmann::json;

static void glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

static std::string defaultProjectPath() {
#ifdef __linux__
    if (std::filesystem::exists("default.jetson.easel")) {
        return "default.jetson.easel";
    }
#endif
    return "default.easel";
}

#ifdef __APPLE__
// Implemented in FileDialog_mac.mm
extern std::string openFileDialog_mac(const char* filter);
extern std::string saveFileDialog_mac(const char* filter, const char* defaultExt);
#endif

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
#elif defined(__APPLE__)
    return openFileDialog_mac(filter);
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
#elif defined(__APPLE__)
    return saveFileDialog_mac(filter, defaultExt);
#endif
    return "";
}

MappingProfile* Application::mappingForZone(OutputZone& z) {
    if (z.mappingIndex >= 0 && z.mappingIndex < (int)m_mappings.size())
        return m_mappings[z.mappingIndex].get();
    return nullptr;
}


bool Application::init() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW" << std::endl;
        return false;
    }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, "Easel", nullptr, nullptr);
    if (!m_window) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // Set window icon (search multiple paths since exe may be in build/Release/).
    // The bundled icon is too large for X11 window-manager properties on Linux.
#ifndef __linux__
    {
        int iw, ih, ic;
        const char* iconPaths[] = {
            "resources/icon.png",
            "../../resources/icon.png",   // from build/Release/
            "../resources/icon.png",      // from build/
        };
        unsigned char* iconData = nullptr;
        for (const char* path : iconPaths) {
            iconData = stbi_load(path, &iw, &ih, &ic, 4);
            if (iconData) break;
        }
        if (iconData) {
            GLFWimage icon = { iw, ih, iconData };
            glfwSetWindowIcon(m_window, 1, &icon);
            stbi_image_free(iconData);
        }
    }
#endif

    glfwSetWindowUserPointer(m_window, this);
    glfwSetDropCallback(m_window, Application::dropCallback);

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // vsync — saves CPU, NDI sends after swap

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD" << std::endl;
        return false;
    }

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;

    if (!m_ui.init(m_window)) return false;

    // Create default mapping profile
    auto mapping = std::make_unique<MappingProfile>();
    if (!mapping->init()) return false;
    m_mappings.push_back(std::move(mapping));

    // Create default output zone
    auto zone = std::make_unique<OutputZone>();
    if (!zone->init()) return false;
    zone->mappingIndex = 0;
    m_zones.push_back(std::move(zone));

    m_quad.createQuad();
    if (!m_passthroughShader.loadFromFiles("shaders/passthrough.vert", "shaders/passthrough.frag")) {
        return false;
    }
    if (!m_edgeBlendShader.loadFromFiles("shaders/passthrough.vert", "shaders/edgeblend.frag")) {
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

    // Etherea client — WebSocket for real-time transcript, SSE for hints
    m_ethereaClient.setTranscriptCallback([this](const std::string& text, bool isFinal) {
        // Update data bus with latest transcript segment
        m_dataBus.set("etherea.latest", text);
        if (isFinal && !text.empty()) {
            // Accumulate full transcript
            std::string prev = m_dataBus.get("etherea.transcript");
            if (!prev.empty()) prev += " ";
            prev += text;
            m_dataBus.set("etherea.transcript", prev);
        }
        // Record time for voice decay
        m_voiceLastInputTime = glfwGetTime();
    });
    m_speechState.available = true;

    // Auto-connect to Etherea (no session ID — server gives us the active session)
    m_ethereaClient.connect("http://localhost:7860");

    // Record initial monitor count and auto-connect if secondary exists
    m_lastMonitorCount = (int)ProjectorOutput::enumerateMonitors().size();
    if (m_projectorAutoConnect && m_lastMonitorCount > 1) {
        int sec = ProjectorOutput::findSecondaryMonitor(m_window);
        if (sec >= 0) {
            activeZone().outputDest = OutputDest::Fullscreen;
            activeZone().outputMonitor = sec;
        }
    }

#ifdef HAS_NDI
    NDIRuntime::instance().init();
    if (NDIRuntime::instance().isAvailable()) {
        // Create persistent finder — it accumulates sources over time via mDNS
        m_ndiFinder.create();
        m_ndiSources = m_ndiFinder.sources();
        // Auto-start composition output
        m_ndiOutput.create("Easel");
    }
#endif

    // Auto-connect to ShaderClaw shaders directory
    {
#ifdef _WIN32
        const char* home = getenv("USERPROFILE");
#else
        const char* home = getenv("HOME");
#endif
        if (home) {
            std::string candidates[] = {
#ifdef _WIN32
                std::string(home) + "\\ShaderClaw3\\shaders",
                std::string(home) + "\\Documents\\ShaderClaw3\\shaders",
                std::string(home) + "\\Documents\\ShaderClaw\\shaders",
#else
                std::string(home) + "/ShaderClaw3/shaders",
                std::string(home) + "/Documents/ShaderClaw3/shaders",
                std::string(home) + "/Documents/ShaderClaw/shaders",
                std::string(home) + "/conductor/workspaces/macbook-migration/doha/ShaderClaw3/shaders",
#endif
            };
            for (const auto& path : candidates) {
                if (std::filesystem::exists(path)) {
                    m_shaderClaw.connect(path);
                    std::cout << "[ShaderClaw] Auto-connected to: " << path << std::endl;
                    break;
                }
            }
        }
    }

    // Auto-load default project if it exists, otherwise blank
    {
        std::string defaultPath = defaultProjectPath();
        if (std::filesystem::exists(defaultPath)) {
            loadProject(defaultPath);
            std::cout << "[Easel] Auto-loaded default project" << std::endl;
        } else {
            std::cout << "[Easel] Starting with blank project" << std::endl;
        }
    }

    // Init 3D stage view
    m_stageView.init();

    // Auto-start OSC receiver on port 9000
    m_oscManager.startReceiver(9000);
    m_oscManager.setSendTarget("127.0.0.1", 9001);

    return true;
}

void Application::run() {
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        // Escape exits presentation fullscreen first, then closes projectors
        if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (m_editorFullscreen) {
                glfwSetWindowMonitor(m_window, nullptr,
                                     m_savedWindowX, m_savedWindowY,
                                     m_savedWindowW, m_savedWindowH, 0);
                m_editorFullscreen = false;
                continue;
            }
            bool hadOutput = !m_projectors.empty();
            for (auto& [idx, proj] : m_projectors) proj->destroy();
            m_projectors.clear();
            for (auto& zone : m_zones) {
                if (zone->outputDest == OutputDest::Fullscreen) {
                    zone->outputDest = OutputDest::None;
                    zone->outputMonitor = -1;
                    hadOutput = true;
                }
            }
            if (hadOutput) continue;
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

        // F11 = toggle editor fullscreen (borderless on current monitor)
        {
            static bool f11WasPressed = false;
            bool f11Now = glfwGetKey(m_window, GLFW_KEY_F11) == GLFW_PRESS;
            if (f11Now && !f11WasPressed) {
                if (!m_editorFullscreen) {
                    // Save windowed position/size
                    glfwGetWindowPos(m_window, &m_savedWindowX, &m_savedWindowY);
                    glfwGetWindowSize(m_window, &m_savedWindowW, &m_savedWindowH);

                    // Find which monitor the window is on
                    int monCount = 0;
                    GLFWmonitor** monitors = glfwGetMonitors(&monCount);
                    GLFWmonitor* best = glfwGetPrimaryMonitor();
                    int wx, wy;
                    glfwGetWindowPos(m_window, &wx, &wy);
                    for (int mi = 0; mi < monCount; mi++) {
                        int mx, my;
                        glfwGetMonitorPos(monitors[mi], &mx, &my);
                        const GLFWvidmode* mode = glfwGetVideoMode(monitors[mi]);
                        if (wx >= mx && wx < mx + mode->width &&
                            wy >= my && wy < my + mode->height) {
                            best = monitors[mi];
                            break;
                        }
                    }
                    const GLFWvidmode* mode = glfwGetVideoMode(best);
                    glfwSetWindowMonitor(m_window, best, 0, 0,
                                         mode->width, mode->height, mode->refreshRate);
                    m_editorFullscreen = true;
                } else {
                    // Restore windowed mode
                    glfwSetWindowMonitor(m_window, nullptr,
                                         m_savedWindowX, m_savedWindowY,
                                         m_savedWindowW, m_savedWindowH, 0);
                    m_editorFullscreen = false;
                }
            }
            f11WasPressed = f11Now;
        }

        int w, h;
        glfwGetFramebufferSize(m_window, &w, &h);
        if (w != m_windowWidth || h != m_windowHeight) {
            m_windowWidth = w;
            m_windowHeight = h;
        }

        // Auto-detect monitor hotplug — set active zone to fullscreen on secondary.
        // Debounced: GLFW transiently reports different monitor counts while new
        // windows (projectors) are being created. React only after the count has
        // stayed the same for ~1s to avoid nuking zone outputs on spurious blips.
        if (m_projectorAutoConnect) {
            static int s_pendingCount = -1;
            static int s_stableFrames = 0;
            int monitorCount = (int)ProjectorOutput::enumerateMonitors().size();
            if (s_pendingCount != monitorCount) {
                s_pendingCount = monitorCount;
                s_stableFrames = 0;
            } else if (s_stableFrames < 1000) {
                s_stableFrames++;
            }
            if (s_stableFrames >= 60 && monitorCount != m_lastMonitorCount) {
                if (monitorCount > 1 && activeZone().outputDest == OutputDest::None) {
                    int sec = ProjectorOutput::findSecondaryMonitor(m_window);
                    if (sec >= 0) {
                        activeZone().outputDest = OutputDest::Fullscreen;
                        activeZone().outputMonitor = sec;
                    }
                } else if (monitorCount <= 1) {
                    // Secondary monitor disconnected — clear fullscreen destinations
                    for (auto& zp : m_zones) {
                        if (zp->outputDest == OutputDest::Fullscreen) {
                            zp->outputDest = OutputDest::None;
                            zp->outputMonitor = -1;
                        }
                    }
                }
                m_lastMonitorCount = monitorCount;
            }
        }

        // Assign stable IDs to any layers that don't have one (e.g. duplicated via UI)
        for (int i = 0; i < m_layerStack.count(); i++) {
            if (m_layerStack[i]->id == 0) {
                m_layerStack[i]->id = m_nextLayerId++;
            }
        }

        updateSources();

        // Update audio analyzer (dt-based)
        {
            static double lastTime = glfwGetTime();
            double now = glfwGetTime();
            float dt = (float)(now - lastTime);
            lastTime = now;

            if (m_mixerEnabled && m_audioMixer.isRunning()) {
                // Mixer mode: drain mixed mono from mixer thread → feed to analyzer
                float monoBuf[4096];
                int count = m_audioMixer.drainMixedMono(monoBuf, 4096);
                if (count > 0) {
                    m_audioAnalyzer.feedSamples(monoBuf, count);
                }
            } else {
                // Legacy single-device mode
                m_audioAnalyzer.setDevice(m_selectedAudioDevice);
#ifdef HAS_FFMPEG
                // m_audioDevices is only populated when FFmpeg is available
                // (see Application.h — lives in the HAS_FFMPEG block).
                if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
                    m_audioAnalyzer.setDeviceId(m_audioDevices[m_selectedAudioDevice].id,
                                                m_audioDevices[m_selectedAudioDevice].isCapture);
                } else {
                    m_audioAnalyzer.setDeviceId("", false);
                }
#else
                m_audioAnalyzer.setDeviceId("", false);
#endif
            }
            m_audioAnalyzer.update(dt);
            m_audioRMS = m_audioAnalyzer.smoothedRMS();
            m_bpmSync.update(dt);
        }

        compositeAndWarp();
        presentOutputs();

        // Periodic auto-save every 30 seconds (crash recovery)
        {
            static double lastAutoSave = 0;
            double now = glfwGetTime();
            if (now - lastAutoSave > 30.0) {
                saveProject(defaultProjectPath());
                lastAutoSave = now;
            }
        }

        glViewport(0, 0, m_windowWidth, m_windowHeight);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        if (m_editorFullscreen) {
            // Presentation mode: draw active zone output fullscreen, no UI
            auto& z = activeZone();
            GLuint outTex = z.warpFBO.textureId();
            if (outTex) {
                // Letterbox to preserve aspect ratio
                float srcAspect = (float)z.width / (float)z.height;
                float winAspect = (float)m_windowWidth / (float)m_windowHeight;
                int vpW, vpH, vpX, vpY;
                if (srcAspect > winAspect) {
                    vpW = m_windowWidth;
                    vpH = (int)(m_windowWidth / srcAspect);
                    vpX = 0;
                    vpY = (m_windowHeight - vpH) / 2;
                } else {
                    vpH = m_windowHeight;
                    vpW = (int)(m_windowHeight * srcAspect);
                    vpX = (m_windowWidth - vpW) / 2;
                    vpY = 0;
                }
                glViewport(vpX, vpY, vpW, vpH);
                m_passthroughShader.use();
                m_passthroughShader.setInt("uTexture", 0);
                m_passthroughShader.setFloat("uOpacity", 1.0f);
                m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
                m_passthroughShader.setBool("uHasMask", false);
                m_passthroughShader.setBool("uFlipV", false);
                m_passthroughShader.setFloat("uTileX", 1.0f);
                m_passthroughShader.setFloat("uTileY", 1.0f);
                m_passthroughShader.setInt("uMosaicMode", 0);
                m_passthroughShader.setFloat("uFeather", 0.0f);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, outTex);
                m_quad.draw();
            }
            // Still need ImGui frame for F11/Escape handling
            m_ui.beginFrame();
            m_ui.endFrame();
        } else {
            glClearColor(0.031f, 0.035f, 0.039f, 1.0f); // #08090a — Linear marketing black, so translucent panels blend correctly
            glClear(GL_COLOR_BUFFER_BIT);

            m_ui.beginFrame();

            // Undo / Redo keybinds — use GLFW for reliable detection
            if (!ImGui::GetIO().WantTextInput) {
                static bool sUndoPrev = false, sRedoPrev = false;
                bool ctrl = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                            glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
                bool shift = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                bool z = glfwGetKey(m_window, GLFW_KEY_Z) == GLFW_PRESS;
                bool y = glfwGetKey(m_window, GLFW_KEY_Y) == GLFW_PRESS;

                bool undoNow = ctrl && z && !shift;
                bool redoNow = ctrl && ((z && shift) || y);

                if (undoNow && !sUndoPrev) m_undoStack.undo(m_layerStack, m_selectedLayer);
                if (redoNow && !sRedoPrev) m_undoStack.redo(m_layerStack, m_selectedLayer);
                sUndoPrev = undoNow;
                sRedoPrev = redoNow;
            }

            renderUI();
            m_ui.endFrame();
        }

        // Check for agent screenshot trigger (after UI is rendered)
        pollScreenshotTrigger();

        // Dispatch Etherea events on main thread
        m_ethereaClient.poll();

        // Push hints and prompt from Etherea into data bus
        {
            auto hints = m_ethereaClient.hints();
            for (int i = 0; i < 3 && i < (int)hints.size(); i++)
                m_dataBus.set("etherea.hint." + std::to_string(i), hints[i]);
            std::string prompt = m_ethereaClient.prompt();
            if (!prompt.empty())
                m_dataBus.set("etherea.prompt", prompt);
        }

        // Voice decay: fade layers with DataBus text bindings after speech stops
        // 2s hold at full opacity, then ease-out over decay duration
        // Only active while decaying (not after fully faded, so UI can regain control)
        if (m_voiceDecayEnabled && m_voiceLastInputTime > 0.0) {
            float elapsed = (float)(glfwGetTime() - m_voiceLastInputTime);
            float totalDuration = m_voiceDecayHold + m_voiceDecayDuration;
            if (elapsed < totalDuration) {
                float decayFactor;
                if (elapsed < m_voiceDecayHold) {
                    decayFactor = 1.0f;
                } else {
                    float t = (elapsed - m_voiceDecayHold) / std::max(0.01f, m_voiceDecayDuration);
                    decayFactor = (1.0f - t) * (1.0f - t); // ease-out quadratic
                }

                // Apply to all layers that have DataBus text bindings
                for (auto& [bindKey, dataKey] : m_dataBus.bindings()) {
                    if (dataKey.empty()) continue;
                    auto sep = bindKey.find(':');
                    if (sep == std::string::npos) continue;
                    uint32_t layerId = (uint32_t)std::stoul(bindKey.substr(0, sep));
                    for (int i = 0; i < m_layerStack.count(); i++) {
                        if (m_layerStack[i]->id == layerId) {
                            m_layerStack[i]->opacity = decayFactor;
                            break;
                        }
                    }
                }
            }
        }

        // Apply data bus bindings to shader text params
        for (auto& [bindKey, dataKey] : m_dataBus.bindings()) {
            if (dataKey.empty()) continue;
            auto sep = bindKey.find(':');
            if (sep == std::string::npos) continue;
            uint32_t layerId = (uint32_t)std::stoul(bindKey.substr(0, sep));
            std::string paramName = bindKey.substr(sep + 1);
            for (int i = 0; i < m_layerStack.count(); i++) {
                auto& layer = m_layerStack[i];
                if (layer->id == layerId && layer->source && layer->source->isShader()) {
                    auto* shader = static_cast<ShaderSource*>(layer->source.get());
                    std::string val = m_dataBus.get(dataKey);
                    // Uppercase for shader text encoding
                    for (auto& c : val) c = (char)toupper((unsigned char)c);
                    shader->setText(paramName, val);
                    break;
                }
            }
        }

        glfwSwapBuffers(m_window);
    }
}

void Application::shutdown() {
    // Auto-save current state as default project
    {
        std::string defaultPath = defaultProjectPath();
        saveProject(defaultPath);
        std::cout << "[Easel] Auto-saved default project" << std::endl;
    }

    m_audioMixer.stop();
#ifdef HAS_FFMPEG
    m_recorder.stop();
    m_rtmpOutput.stop();
    cleanupAudioMeter();
#endif
#ifdef HAS_NDI
    // Destroy per-layer NDI senders before shutting down runtime
    for (int i = 0; i < m_layerStack.count(); i++) {
        m_layerStack[i]->ndiSender.destroy();
    }
    // Destroy per-zone NDI senders
    for (auto& zp : m_zones) {
        zp->ndiOutput.destroy();
    }
    m_ndiOutput.destroy();
    NDIRuntime::instance().shutdown();
#endif
#ifdef HAS_SPOUT
    for (auto& zp : m_zones) {
        zp->spoutOutput.destroy();
    }
    m_spoutOutput.destroy();
#endif
    m_ethereaClient.disconnect();
#ifdef HAS_OPENCV
    m_scanner.cancelScan();
    m_webcam.close();
#endif
    for (auto& [idx, proj] : m_projectors) proj->destroy();
    m_projectors.clear();
    m_ui.shutdown();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Application::updateSources() {
    // Hot-reload any changed Shader-Claw shaders
    m_shaderClaw.update();

    // Get mouse state for interactive shaders (normalized 0-1)
    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);
    int winW, winH;
    glfwGetWindowSize(m_window, &winW, &winH);
    float normMX = (winW > 0) ? (float)(mx / winW) : 0.5f;
    float normMY = (winH > 0) ? 1.0f - (float)(my / winH) : 0.5f; // flip Y for GL
    bool mousePressed = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& layer = m_layerStack[i];
        if (layer->source) {
            // Pass audio + mouse state to ShaderSource layers
            if (layer->source->isShader()) {
                auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
                // Match shader resolution to composition size (or per-layer override)
                // Use the largest zone that contains this layer so the shader renders
                // at sufficient quality for all outputs (editor, projector, NDI).
                int sw = 0, sh = 0;
                if (layer->shaderWidth > 0 && layer->shaderHeight > 0) {
                    sw = layer->shaderWidth;
                    sh = layer->shaderHeight;
                } else {
                    for (auto& zp : m_zones) {
                        bool inZone = zp->showAllLayers || zp->visibleLayerIds.count(layer->id);
                        if (inZone && zp->width * zp->height > sw * sh) {
                            sw = zp->width;
                            sh = zp->height;
                        }
                    }
                    if (sw == 0 || sh == 0) { sw = activeZone().width; sh = activeZone().height; }
                }
                shaderSrc->setResolution(sw, sh);
                shaderSrc->setAudioState(
                    m_audioAnalyzer.smoothedRMS(),
                    m_audioAnalyzer.bass(),
                    (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f,
                    m_audioAnalyzer.treble(),
                    m_audioAnalyzer.fftTexture()
                );
                shaderSrc->applyAudioBindings(
                    m_audioAnalyzer.smoothedRMS(),
                    m_audioAnalyzer.bass(),
                    (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f,
                    m_audioAnalyzer.treble(),
                    m_audioAnalyzer.beatDecay(),
                    &m_midiManager
                );
                shaderSrc->applyMidiBindings(m_midiCCValues);
                shaderSrc->setMouseState(normMX, normMY, mousePressed);

                // Refresh image input bindings (texture IDs may change each frame)
                for (auto& [name, binding] : shaderSrc->imageBindings()) {
                    if (binding.sourceLayerId == 0) continue;
                    for (int j = 0; j < m_layerStack.count(); j++) {
                        auto& srcLayer = m_layerStack[j];
                        if (srcLayer->id == binding.sourceLayerId && srcLayer->source) {
                            GLuint srcTex = srcLayer->source->textureId();
                            if (srcTex == 0) break; // source not yet initialized
                            binding.textureId = srcTex;
                            binding.width = srcLayer->source->width();
                            binding.height = srcLayer->source->height();
                            binding.flippedV = srcLayer->source->isFlippedV();
                            break;
                        }
                    }
                }
            }

            layer->source->update();

            // Auto-crop: detect black borders once when source first has a valid texture
            if (layer->autoCrop && !layer->autoCropDone &&
                layer->source->textureId() != 0 &&
                layer->source->width() > 0 && layer->source->height() > 0) {
                int w = layer->source->width();
                int h = layer->source->height();
                std::vector<uint8_t> px(w * h * 4);
                glBindTexture(GL_TEXTURE_2D, layer->source->textureId());
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glBindTexture(GL_TEXTURE_2D, 0);

                // Check if the image has any non-black content before cropping.
                // Sources like WGC start with an empty (black) texture that gets
                // filled a few frames later. Running auto-crop on an all-black
                // frame would crop everything and make the layer invisible.
                bool hasContent = false;
                for (int s = 0; s < w * h * 4 && !hasContent; s += 97 * 4) {
                    if (px[s] > 12 || px[s+1] > 12 || px[s+2] > 12) hasContent = true;
                }
                if (!hasContent) continue; // defer auto-crop until real content arrives

                const int thresh = 12;
                auto isBlack = [&](int x, int y) {
                    int idx = ((h - 1 - y) * w + x) * 4;
                    return px[idx] < thresh && px[idx+1] < thresh && px[idx+2] < thresh;
                };

                int cL = 0, cR = 0, cT = 0, cB = 0;
                for (int x = 0; x < w/2; x++) {
                    bool all = true;
                    for (int y = 0; y < h; y += 4) { if (!isBlack(x,y)) { all=false; break; } }
                    if (!all) break; cL = x+1;
                }
                for (int x = w-1; x >= w/2; x--) {
                    bool all = true;
                    for (int y = 0; y < h; y += 4) { if (!isBlack(x,y)) { all=false; break; } }
                    if (!all) break; cR = w-x;
                }
                for (int y = 0; y < h/2; y++) {
                    bool all = true;
                    for (int x = 0; x < w; x += 4) { if (!isBlack(x,y)) { all=false; break; } }
                    if (!all) break; cT = y+1;
                }
                for (int y = h-1; y >= h/2; y--) {
                    bool all = true;
                    for (int x = 0; x < w; x += 4) { if (!isBlack(x,y)) { all=false; break; } }
                    if (!all) break; cB = h-y;
                }

                layer->cropLeft   = (float)cL / (float)w;
                layer->cropRight  = (float)cR / (float)w;
                layer->cropTop    = (float)cT / (float)h;
                layer->cropBottom = (float)cB / (float)h;
                layer->autoCropDone = true;
            }
        }

#ifdef HAS_NDI
        // Auto-manage per-layer NDI output
        if (NDIRuntime::instance().isAvailable()) {
            if (layer->ndiEnabled) {
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
            } else if (layer->ndiSender.isActive()) {
                layer->ndiSender.destroy();
                layer->ndiName.clear();
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
            activeZone().warpFBO.bind();
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
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

    // Re-render any dirty masks on layers
    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& layer = m_layerStack[i];
        for (auto& mask : layer->masks) {
            if (mask.path.isDirty() && mask.path.count() >= 3) {
                if (!mask.texture) {
                    mask.texture = std::make_shared<Texture>();
                }
                m_maskRenderer.render(mask.path, 1024, 1024, *mask.texture);
                mask.path.clearDirty();
            }
        }
    }

    // Re-render any dirty canvas-level masks (MappingProfile)
    for (auto& mp : m_mappings) {
        for (auto& mask : mp->masks) {
            if (mask.path.isDirty() && mask.path.count() >= 3) {
                if (!mask.texture) {
                    mask.texture = std::make_shared<Texture>();
                }
                m_maskRenderer.render(mask.path, 1024, 1024, *mask.texture);
                mask.path.clearDirty();
            }
        }
    }

    // Composite each zone independently
    for (auto& zonePtr : m_zones) {
        compositeZone(*zonePtr);
    }
}

void Application::compositeZone(OutputZone& zone) {
    // Filter layers by zone visibility
    std::vector<std::shared_ptr<Layer>> layers;
    if (zone.showAllLayers) {
        layers = m_layerStack.layers();
    } else {
        for (int i = 0; i < m_layerStack.count(); i++) {
            if (zone.visibleLayerIds.count(m_layerStack[i]->id)) {
                layers.push_back(m_layerStack[i]);
            }
        }
    }

    {
        AudioState audio;
        audio.rms = m_audioAnalyzer.smoothedRMS();
        audio.bass = m_audioAnalyzer.bass();
        audio.lowMid = m_audioAnalyzer.lowMid();
        audio.highMid = m_audioAnalyzer.highMid();
        audio.treble = m_audioAnalyzer.treble();
        audio.beatDecay = m_audioAnalyzer.beatDecay();
        audio.beatDetected = m_audioAnalyzer.beatDetected();
        audio.fftTexture = m_audioAnalyzer.fftTexture();
        audio.time = (float)glfwGetTime();
        audio.bpm = m_bpmSync.bpm();
        audio.beatPhase = m_bpmSync.beatPhase();
        audio.beatPulse = m_bpmSync.beatPulse();
        audio.barPhase = m_bpmSync.barPhase();
        zone.compositor.setAudioState(audio);
    }
    zone.compositor.composite(layers);

    GLuint sourceTex = zone.compositor.resultTexture();
    if (layers.empty()) {
        sourceTex = m_testPattern.id();
    }

    // Per-layer masks are applied during compositing (CompositeEngine).
    // Canvas-level masks (MappingProfile) are applied here, after compositing, before warp.
    auto* mpMask = mappingForZone(zone);
    if (mpMask && !mpMask->masks.empty()) {
        int validCount = 0;
        GLuint singleMaskTex = 0;
        float singleFeather = 0.0f;
        bool singleInvert = false;
        for (auto& mask : mpMask->masks) {
            if (mask.texture && mask.texture->id() && mask.path.count() >= 3) {
                validCount++;
                singleMaskTex = mask.texture->id();
                if (validCount == 1) {
                    singleFeather = mask.feather;
                    singleInvert = mask.invert;
                }
            }
        }
        if (validCount > 0) {
            GLuint combinedMaskTex = singleMaskTex;
            if (validCount > 1) {
                if (m_maskPingPongFBO.width() != zone.width || m_maskPingPongFBO.height() != zone.height)
                    m_maskPingPongFBO.create(zone.width, zone.height);
                m_maskPingPongFBO.bind();
                glViewport(0, 0, zone.width, zone.height);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                m_passthroughShader.use();
                m_passthroughShader.setInt("uTexture", 0);
                m_passthroughShader.setFloat("uOpacity", 1.0f);
                m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
                m_passthroughShader.setBool("uHasMask", false);
                m_passthroughShader.setBool("uFlipV", false);
                m_passthroughShader.setFloat("uTileX", 1.0f);
                m_passthroughShader.setFloat("uTileY", 1.0f);
                m_passthroughShader.setInt("uMosaicMode", 0);
                m_passthroughShader.setFloat("uFeather", 0.0f);
                m_passthroughShader.setFloat("uMaskFeather", 0.0f);
                m_passthroughShader.setBool("uMaskInvert", false);
                for (auto& mask : mpMask->masks) {
                    if (mask.texture && mask.texture->id() && mask.path.count() >= 3) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, mask.texture->id());
                        m_quad.draw();
                    }
                }
                glDisable(GL_BLEND);
                Framebuffer::unbind();
                combinedMaskTex = m_maskPingPongFBO.textureId();
            }
            // Apply the canvas mask to the composite
            if (m_edgeBlendFBO.width() != zone.width || m_edgeBlendFBO.height() != zone.height)
                m_edgeBlendFBO.create(zone.width, zone.height);
            m_edgeBlendFBO.bind();
            glViewport(0, 0, zone.width, zone.height);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            m_passthroughShader.use();
            m_passthroughShader.setInt("uTexture", 0);
            m_passthroughShader.setFloat("uOpacity", 1.0f);
            m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
            m_passthroughShader.setBool("uHasMask", true);
            m_passthroughShader.setInt("uMask", 1);
            m_passthroughShader.setBool("uFlipV", false);
            m_passthroughShader.setFloat("uTileX", 1.0f);
            m_passthroughShader.setFloat("uTileY", 1.0f);
            m_passthroughShader.setInt("uMosaicMode", 0);
            m_passthroughShader.setFloat("uFeather", 0.0f);
            m_passthroughShader.setFloat("uMaskFeather", singleFeather);
            m_passthroughShader.setBool("uMaskInvert", singleInvert);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sourceTex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, combinedMaskTex);
            m_quad.draw();
            Framebuffer::unbind();
            sourceTex = m_edgeBlendFBO.textureId();
        }
    }

    // Store composite texture for canvas preview
    zone.canvasTexture = sourceTex;

    zone.warpFBO.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    auto* mp = mappingForZone(zone);
    if (sourceTex && mp) {
        if (mp->warpMode == ViewportPanel::WarpMode::CornerPin) {
            mp->cornerPin.render(sourceTex);
        } else if (mp->warpMode == ViewportPanel::WarpMode::MeshWarp) {
            mp->meshWarp.render(sourceTex);
        } else if (mp->warpMode == ViewportPanel::WarpMode::ObjMesh) {
            float aspect = (float)zone.width / (float)zone.height;
            mp->objMeshWarp.render(sourceTex, aspect);
        }
    } else if (sourceTex) {
        // No mapping — passthrough
        m_passthroughShader.use();
        m_passthroughShader.setInt("uTexture", 0);
        m_passthroughShader.setFloat("uOpacity", 1.0f);
        m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
        m_passthroughShader.setBool("uHasMask", false);
        m_passthroughShader.setBool("uFlipV", false);
        m_passthroughShader.setFloat("uTileX", 1.0f);
        m_passthroughShader.setFloat("uTileY", 1.0f);
        m_passthroughShader.setInt("uMosaicMode", 0);
        m_passthroughShader.setFloat("uFeather", 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTex);
        m_quad.draw();
    }

    Framebuffer::unbind();

    // Edge blend post-process (if any edge has blend width > 0)
    bool hasEdgeBlend = mp && (mp->edgeBlendLeft > 0 || mp->edgeBlendRight > 0 ||
                               mp->edgeBlendTop > 0 || mp->edgeBlendBottom > 0);
    if (hasEdgeBlend) {
        // Ensure temp FBO matches zone size
        if (m_edgeBlendFBO.width() != zone.width || m_edgeBlendFBO.height() != zone.height) {
            m_edgeBlendFBO.create(zone.width, zone.height);
        }
        // Render warpFBO through edge blend shader into temp FBO
        m_edgeBlendFBO.bind();
        glViewport(0, 0, zone.width, zone.height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        m_edgeBlendShader.use();
        m_edgeBlendShader.setInt("uTexture", 0);
        m_edgeBlendShader.setMat3("uTransform", glm::mat3(1.0f));
        m_edgeBlendShader.setFloat("uBlendLeft", mp->edgeBlendLeft / (float)zone.width);
        m_edgeBlendShader.setFloat("uBlendRight", mp->edgeBlendRight / (float)zone.width);
        m_edgeBlendShader.setFloat("uBlendTop", mp->edgeBlendTop / (float)zone.height);
        m_edgeBlendShader.setFloat("uBlendBottom", mp->edgeBlendBottom / (float)zone.height);
        m_edgeBlendShader.setFloat("uGamma", mp->edgeBlendGamma);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
        m_quad.draw();
        Framebuffer::unbind();

        // Copy back to warpFBO
        zone.warpFBO.bind();
        glViewport(0, 0, zone.width, zone.height);
        m_passthroughShader.use();
        m_passthroughShader.setInt("uTexture", 0);
        m_passthroughShader.setFloat("uOpacity", 1.0f);
        m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
        m_passthroughShader.setFloat("uTileX", 1.0f);
        m_passthroughShader.setFloat("uTileY", 1.0f);
        m_passthroughShader.setInt("uMosaicMode", 0);
        m_passthroughShader.setFloat("uFeather", 0.0f);
        m_passthroughShader.setBool("uHasMask", false);
        m_passthroughShader.setBool("uFlipV", false);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_edgeBlendFBO.textureId());
        m_quad.draw();
        Framebuffer::unbind();
    }

}

void Application::renderReadbackFBO(OutputZone& zone) {
    zone.readbackFBO.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    m_passthroughShader.use();
    m_passthroughShader.setInt("uTexture", 0);
    m_passthroughShader.setFloat("uOpacity", 1.0f);
    m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
    m_passthroughShader.setBool("uFlipV", true);
    m_passthroughShader.setBool("uHasMask", false);
    m_passthroughShader.setFloat("uTileX", 1.0f);
    m_passthroughShader.setFloat("uTileY", 1.0f);
    m_passthroughShader.setInt("uMosaicMode", 0);
    m_passthroughShader.setFloat("uFeather", 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
    m_quad.draw();
    Framebuffer::unbind();
}

void Application::presentOutputs() {
    glFinish(); // Ensure all zone FBOs are written before presenting

    // Track which monitor indices are still needed
    std::set<int> neededMonitors;

    // Per-zone output routing
    for (int i = 0; i < (int)m_zones.size(); i++) {
        auto& zone = *m_zones[i];

        if (zone.outputDest == OutputDest::Fullscreen && zone.outputMonitor >= 0) {
            // Verify monitor still exists before using it.
            auto monitors = ProjectorOutput::enumerateMonitors();
            if (zone.outputMonitor >= (int)monitors.size()) {
                // Monitor index out of range this frame. GLFW can report a
                // transiently shrunk monitor list while new windows are being
                // created — do NOT wipe the zone's saved destination. Just
                // skip rendering this frame; it will recover once the monitor
                // list stabilises. Keep the projector in neededMonitors so the
                // cleanup pass below doesn't destroy it either.
                neededMonitors.insert(zone.outputMonitor);
            } else {
                neededMonitors.insert(zone.outputMonitor);
                // Ensure a projector exists on this monitor
                auto it = m_projectors.find(zone.outputMonitor);
                if (it == m_projectors.end() || !it->second->isActive()) {
                    // Rate-limit retries so a persistently-failing monitor
                    // (e.g. editor's own) doesn't hammer glfwCreateWindow
                    // every frame. Retry no more often than once per ~60
                    // frames; after too many failures, give up and clear.
                    static std::unordered_map<int, int> s_retryCountdown;
                    static std::unordered_map<int, int> s_failureCount;
                    int key = zone.outputMonitor;
                    int& countdown = s_retryCountdown[key];
                    if (countdown > 0) {
                        countdown--;
                    } else {
                        auto proj = std::make_unique<ProjectorOutput>();
                        if (proj->create(m_window, zone.outputMonitor)) {
                            zone.resize(proj->projectorWidth(), proj->projectorHeight());
                            m_projectors[zone.outputMonitor] = std::move(proj);
                            s_failureCount[key] = 0;
                        } else {
                            // Back off for ~1s before next retry.
                            countdown = 60;
                            if (++s_failureCount[key] >= 10) {
                                // 10 consecutive failures (~10s) — give up.
                                std::cerr << "Projector on monitor " << key
                                          << " failed 10 times; clearing zone output."
                                          << std::endl;
                                zone.outputDest = OutputDest::None;
                                zone.outputMonitor = -1;
                                s_failureCount[key] = 0;
                            }
                        }
                    }
                }
                auto it2 = m_projectors.find(zone.outputMonitor);
                if (it2 != m_projectors.end() && it2->second->isActive()) {
                    it2->second->present(zone.warpFBO.textureId());
                }
            }
        }

#ifdef HAS_NDI
        if (zone.outputDest == OutputDest::NDI) {
            if (!zone.ndiOutput.isActive()) {
                std::string name = "Easel - " + (zone.ndiStreamName.empty() ? zone.name : zone.ndiStreamName);
                zone.ndiOutput.create(name);
            }
            if (zone.ndiOutput.isActive()) {
                renderReadbackFBO(zone);
                zone.ndiOutput.send(zone.readbackFBO.textureId(), zone.width, zone.height);
            }
        } else {
            // Destroy NDI sender if no longer needed
            if (zone.ndiOutput.isActive()) {
                zone.ndiOutput.destroy();
            }
        }
#endif
#ifdef HAS_SPOUT
        if (zone.outputDest == OutputDest::Spout) {
            if (!zone.spoutOutput.isActive()) {
                std::string name = "Easel - " + (zone.spoutStreamName.empty() ? zone.name : zone.spoutStreamName);
                zone.spoutOutput.create(name, zone.warpFBO.width(), zone.warpFBO.height());
            }
            if (zone.spoutOutput.isActive()) {
                zone.spoutOutput.send(zone.warpFBO.textureId(), zone.warpFBO.width(), zone.warpFBO.height());
            }
        } else {
            if (zone.spoutOutput.isActive()) {
                zone.spoutOutput.destroy();
            }
        }
#endif
    }

    // Clean up projectors for monitors no longer claimed by any zone
    for (auto it = m_projectors.begin(); it != m_projectors.end(); ) {
        if (neededMonitors.find(it->first) == neededMonitors.end()) {
            it->second->destroy();
            it = m_projectors.erase(it);
        } else {
            ++it;
        }
    }

    // Global outputs (stream/record) — use active zone
    auto& active = activeZone();
    bool needsReadback = false;
#ifdef HAS_NDI
    if (m_ndiOutputEnabled && m_ndiOutput.isActive()) needsReadback = true;
#endif
#ifdef HAS_FFMPEG
    if (m_rtmpOutput.isActive() || m_recorder.isActive()) needsReadback = true;
#endif
    if (needsReadback) {
        renderReadbackFBO(active);
    }

#ifdef HAS_NDI
    // Legacy global NDI output (composition toggle in NDI panel)
    if (m_ndiOutputEnabled && m_ndiOutput.isActive()) {
        m_ndiOutput.send(active.readbackFBO.textureId(), active.width, active.height);
    }
#endif
#ifdef HAS_SPOUT
    if (m_spoutOutputEnabled && m_spoutOutput.isActive()) {
        m_spoutOutput.send(active.warpFBO.textureId(), active.warpFBO.width(), active.warpFBO.height());
    }
#endif

#ifdef HAS_FFMPEG
    if (m_rtmpOutput.isActive()) {
        m_rtmpOutput.sendFrame(active.readbackFBO.textureId(), active.width, active.height);
    }
    if (m_recorder.isActive()) {
        m_recorder.sendFrame(active.readbackFBO.textureId(), active.width, active.height);
    }
#endif
}

void Application::addZone() {
    auto zone = std::make_unique<OutputZone>();
    zone->name = "Zone " + std::to_string(m_zones.size() + 1);
    // Match resolution of the first zone
    if (!m_zones.empty()) {
        zone->width = m_zones[0]->width;
        zone->height = m_zones[0]->height;
    }
    // Create a fresh mapping profile for this zone (independent masks/warp)
    auto mp = std::make_unique<MappingProfile>();
    mp->name = zone->name;
    mp->init();
    zone->mappingIndex = (int)m_mappings.size();
    m_mappings.push_back(std::move(mp));
    zone->init();
    m_zones.push_back(std::move(zone));
}

void Application::setupMultiGPUProjection(const std::vector<std::string>& ndiSourceNames) {
    // Auto-setup for multi-GPU projection mapping:
    // Creates one zone per NDI source, each with a dedicated NDI layer.
    // Each zone shows only its own layer and can be assigned to a projector.

    std::cout << "[MultiGPU] Setting up " << ndiSourceNames.size() << " projection zones\n";

    // Enumerate available monitors for auto-assignment
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

    for (size_t i = 0; i < ndiSourceNames.size(); i++) {
        // Create a zone for this stream
        auto zone = std::make_unique<OutputZone>();
        zone->name = "GPU " + std::to_string(i);
        if (!m_zones.empty()) {
            zone->width = m_zones[0]->width;
            zone->height = m_zones[0]->height;
        }
        zone->mappingIndex = 0;
        zone->showAllLayers = false; // Only show assigned layer
        zone->init();

#ifdef HAS_NDI
        // Connect NDI source
        auto source = std::make_shared<NDISource>();
        if (source->connect(ndiSourceNames[i])) {
            auto layer = std::make_shared<Layer>();
            layer->id = m_nextLayerId++;
            layer->name = "GPU " + std::to_string(i) + ": " + ndiSourceNames[i];
            layer->source = source;
            m_layerStack.addLayer(layer);

            // Set zone visibility to only this layer
            zone->visibleLayerIds.insert(layer->id);

            // Auto-assign to projector if monitor available (skip primary = index 0)
            int monitorIdx = static_cast<int>(i) + 1;
            if (monitorIdx < monitorCount) {
                zone->outputDest = OutputDest::Fullscreen;
                zone->outputMonitor = monitorIdx;
            }

            std::cout << "[MultiGPU] Zone " << i << ": " << ndiSourceNames[i]
                      << " -> layer " << layer->id;
            if (monitorIdx < monitorCount)
                std::cout << " -> monitor " << monitorIdx;
            std::cout << "\n";
        } else {
            std::cerr << "[MultiGPU] Failed to connect NDI: " << ndiSourceNames[i] << "\n";
        }
#else
        std::cerr << "[MultiGPU] NDI not available (compiled without HAS_NDI)\n";
#endif

        m_zones.push_back(std::move(zone));
    }

    std::cout << "[MultiGPU] Setup complete: " << m_zones.size() << " total zones\n";
}

void Application::removeZone(int index) {
    if ((int)m_zones.size() <= 1) return; // always keep at least one zone
    if (index < 0 || index >= (int)m_zones.size()) return;

    // Clean up outputs before removing
    auto& zone = *m_zones[index];
    if (zone.outputDest == OutputDest::Fullscreen && zone.outputMonitor >= 0) {
        // Only destroy if no other zone claims this monitor
        bool otherClaims = false;
        for (int i = 0; i < (int)m_zones.size(); i++) {
            if (i != index && m_zones[i]->outputDest == OutputDest::Fullscreen &&
                m_zones[i]->outputMonitor == zone.outputMonitor) {
                otherClaims = true;
                break;
            }
        }
        if (!otherClaims) {
            auto it = m_projectors.find(zone.outputMonitor);
            if (it != m_projectors.end()) {
                it->second->destroy();
                m_projectors.erase(it);
            }
        }
    }
#ifdef HAS_NDI
    if (zone.ndiOutput.isActive()) {
        zone.ndiOutput.destroy();
    }
#endif
#ifdef HAS_SPOUT
    if (zone.spoutOutput.isActive()) {
        zone.spoutOutput.destroy();
    }
#endif

    m_zones.erase(m_zones.begin() + index);
    if (m_activeZone >= (int)m_zones.size()) {
        m_activeZone = (int)m_zones.size() - 1;
    }
}

void Application::duplicateZone(int index) {
    if (index < 0 || index >= (int)m_zones.size()) return;
    auto& src = *m_zones[index];

    auto z = std::make_unique<OutputZone>();
    z->name = src.name + " Copy";
    z->width = src.width;
    z->height = src.height;
    z->compPreset = src.compPreset;
    z->showAllLayers = src.showAllLayers;
    z->visibleLayerIds = src.visibleLayerIds;
    z->outputDest = OutputDest::None; // user picks new output for copy
    z->outputMonitor = -1;

    // Deep-copy the source zone's mapping profile (independent masks/warp)
    auto* srcMapping = mappingForZone(src);
    auto mp = std::make_unique<MappingProfile>();
    mp->name = z->name;
    mp->init();
    if (srcMapping) {
        mp->warpMode = srcMapping->warpMode;
        mp->cornerPin.setCorners(srcMapping->cornerPin.corners());
        mp->edgeBlendLeft = srcMapping->edgeBlendLeft;
        mp->edgeBlendRight = srcMapping->edgeBlendRight;
        mp->edgeBlendTop = srcMapping->edgeBlendTop;
        mp->edgeBlendBottom = srcMapping->edgeBlendBottom;
        mp->edgeBlendGamma = srcMapping->edgeBlendGamma;
        // Copy masks
        for (auto& srcMask : srcMapping->masks) {
            MappingMask m;
            m.name = srcMask.name;
            m.path = srcMask.path;
            m.feather = srcMask.feather;
            m.invert = srcMask.invert;
            // Texture will be re-rendered on next frame
            mp->masks.push_back(std::move(m));
        }
    }
    z->mappingIndex = (int)m_mappings.size();
    m_mappings.push_back(std::move(mp));

    z->init();
    m_zones.push_back(std::move(z));
}

void Application::renderUI() {
    // Escape key deselects current layer — but NOT while we're in mask edit mode,
    // where Esc belongs to the mask editor (see ViewportPanel::renderMaskOverlay).
    if (!m_maskEditMode && m_selectedLayer >= 0 &&
        ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive()) {
        m_selectedLayer = -1;
    }

    // Keep mapping profile names in sync with their owning zone's name. When
    // the user renames a zone (via double-click on its tab), the mapping that
    // the zone points to should adopt the same name so the Mapping dropdown
    // reads meaningfully ("Main", "Floor screen", …) instead of stale defaults.
    for (auto& zPtr : m_zones) {
        if (!zPtr) continue;
        int mi = zPtr->mappingIndex;
        if (mi >= 0 && mi < (int)m_mappings.size() && m_mappings[mi]) {
            if (m_mappings[mi]->name != zPtr->name) {
                m_mappings[mi]->name = zPtr->name;
            }
        }
    }
    // Click on any non-interactive area deselects (Escape already handles keyboard)
    // Viewport and LayerPanel handle their own deselect on empty-space click.

    handleDroppedFiles();

    // Process MIDI events
    {
        auto events = m_midiManager.pollEvents();
        // Update normalized CC table for shader-parameter MIDI bindings
        for (const auto& ev : events) {
            if (ev.type == 0 && ev.channel >= 0 && ev.channel < 16 &&
                ev.number >= 0 && ev.number < 128) {
                m_midiCCValues[ev.channel][ev.number] = ev.value / 127.0f;
            }
        }
        auto actions = m_midiManager.processEvents(events);
        for (const auto& act : actions) {
            switch (act.target) {
                case MIDIMapping::Target::LayerOpacity:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->opacity = act.value;
                    break;
                case MIDIMapping::Target::LayerVisible:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->visible = act.value > 0.5f;
                    break;
                case MIDIMapping::Target::LayerPosX:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->position.x = act.value * 2.0f - 1.0f;
                    break;
                case MIDIMapping::Target::LayerPosY:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->position.y = act.value * 2.0f - 1.0f;
                    break;
                case MIDIMapping::Target::LayerScale:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->scale = glm::vec2(act.value * 2.0f);
                    break;
                case MIDIMapping::Target::LayerRotation:
                    if (act.layerIndex >= 0 && act.layerIndex < m_layerStack.count())
                        m_layerStack[act.layerIndex]->rotation = act.value * 360.0f;
                    break;
                case MIDIMapping::Target::SceneRecall:
                    m_sceneManager.recallScene(act.sceneIndex, m_layerStack);
                    break;
                case MIDIMapping::Target::BPMSet:
                    m_bpmSync.setBPM(act.value * 200.0f + 40.0f);
                    break;
                case MIDIMapping::Target::BPMTap:
                    if (act.value > 0.5f) m_bpmSync.tap();
                    break;
            }
        }
    }

    // Process OSC messages
    {
        auto msgs = m_oscManager.pollMessages();
        for (const auto& msg : msgs) {
            // /easel/layer/N/opacity float
            // /easel/layer/N/visible int(0/1)
            // /easel/layer/N/posX float
            // /easel/layer/N/posY float
            // /easel/layer/N/scale float
            // /easel/scene/N (recall scene N)
            // /easel/bpm float
            // /easel/tap (tap tempo)

            if (msg.address == "/easel/bpm" && !msg.floats.empty()) {
                m_bpmSync.setBPM(msg.floats[0]);
            } else if (msg.address == "/easel/tap") {
                m_bpmSync.tap();
            } else if (msg.address.rfind("/easel/scene/", 0) == 0) {
                int idx = atoi(msg.address.c_str() + 13);
                m_sceneManager.recallScene(idx, m_layerStack);
            } else if (msg.address.rfind("/easel/layer/", 0) == 0) {
                // Parse /easel/layer/N/property
                const char* rest = msg.address.c_str() + 13;
                int layerIdx = atoi(rest);
                const char* slash = strchr(rest, '/');
                if (slash && layerIdx >= 0 && layerIdx < m_layerStack.count()) {
                    auto& layer = m_layerStack[layerIdx];
                    std::string prop = slash + 1;
                    if (prop == "opacity" && !msg.floats.empty())
                        layer->opacity = std::max(0.0f, std::min(1.0f, msg.floats[0]));
                    else if (prop == "visible" && !msg.ints.empty())
                        layer->visible = msg.ints[0] != 0;
                    else if (prop == "posX" && !msg.floats.empty())
                        layer->position.x = msg.floats[0];
                    else if (prop == "posY" && !msg.floats.empty())
                        layer->position.y = msg.floats[0];
                    else if (prop == "scale" && !msg.floats.empty())
                        layer->scale = glm::vec2(msg.floats[0]);
                    else if (prop == "rotation" && !msg.floats.empty())
                        layer->rotation = msg.floats[0];
                }
            }
            // Forward to DataBus
            if (!msg.floats.empty()) {
                m_dataBus.set(msg.address, std::to_string(msg.floats[0]));
            } else if (!msg.strings.empty()) {
                m_dataBus.set(msg.address, msg.strings[0]);
            }
        }
    }

#ifdef HAS_FFMPEG
    float transportBarH = 56.0f;
#else
    float transportBarH = 0.0f;
#endif
    renderMenuBar();
    m_ui.setupDockspace(transportBarH);

    // Reset editing state when switching zones
    if (m_activeZone != m_prevActiveZone) {
        m_maskEditMode = false;
        m_viewportPanel.resetDragState();
        m_prevActiveZone = m_activeZone;
    }

    // Scoped: zone tab clicks inside viewport render may change m_activeZone
    {
        auto& z = activeZone();
        // Use projector aspect if active zone has one, otherwise zone w/h
        float projAspect = (float)z.width / (float)z.height;
        if (z.outputDest == OutputDest::Fullscreen && z.outputMonitor >= 0) {
            auto it = m_projectors.find(z.outputMonitor);
            if (it != m_projectors.end() && it->second->isActive()) {
                projAspect = it->second->aspectRatio();
            }
        }
        auto monitors = ProjectorOutput::enumerateMonitors();
        bool ndiAvail = false;
#ifdef HAS_NDI
        ndiAvail = NDIRuntime::instance().isAvailable();
#endif
        // Determine which monitor the editor window is on so the UI can hide it
        int editorMon = -1;
        {
            int wx, wy;
            glfwGetWindowPos(m_window, &wx, &wy);
            for (int mi = 0; mi < (int)monitors.size(); mi++) {
                const auto& m = monitors[mi];
                if (wx >= m.x && wx < m.x + m.width && wy >= m.y && wy < m.y + m.height) {
                    editorMon = mi;
                    break;
                }
            }
        }
        // Viewport preview: show the warped output so users on a single display
        // (common on Mac) can see corner-pin/mesh-warp changes live. Fall back to
        // the flat composite when in mask edit mode (masks are drawn in pre-warp space).
        GLuint previewTex = 0;
        if (m_maskEditMode) {
            previewTex = z.canvasTexture ? z.canvasTexture : z.compositor.resultTexture();
        } else {
            previewTex = z.warpFBO.textureId();
            if (!previewTex) previewTex = z.canvasTexture ? z.canvasTexture : z.compositor.resultTexture();
        }
        if (!previewTex) previewTex = m_testPattern.id();
        m_viewportPanel.setLayerSelected(m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count());
        m_viewportPanel.render(previewTex, mappingForZone(z), projAspect,
                               &m_zones, &m_activeZone, &monitors, ndiAvail, editorMon, &m_mappings);
    }

    // Handle signals from viewport tabs
    if (m_activeZone < 0) {
        int signal = -m_activeZone;
        if (signal >= 100 && signal < 200) {
            // Add zone (signal = 100 + zones.size())
            addZone();
            m_activeZone = (int)m_zones.size() - 1;
        } else if (signal >= 200 && signal < 300) {
            // Duplicate zone (signal = 200 + index)
            int idx = signal - 200;
            duplicateZone(idx);
            m_activeZone = (int)m_zones.size() - 1;
        } else if (signal >= 300 && signal < 400) {
            // Remove zone (signal = 300 + index)
            int idx = signal - 300;
            removeZone(idx);
            // m_activeZone was the negative signal — cap to valid range
            if (m_activeZone < 0 || m_activeZone >= (int)m_zones.size()) {
                m_activeZone = std::max(0, (int)m_zones.size() - 1);
            }
        } else {
            // Legacy: simple add
            addZone();
            m_activeZone = (int)m_zones.size() - 1;
        }
    }

    // Re-fetch after viewport render since m_activeZone may have changed
    auto& zone = activeZone();
    if (m_ui.isPanelVisible("Layers")) {
        m_layerPanel.render(m_layerStack, m_selectedLayer, &m_zones, m_activeZone);
    }

    // Handle "+" button signals from layer panel
    if (m_layerPanel.wantsAddImage) {
        std::string path = openFileDialog("Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0");
        if (!path.empty()) loadImage(path);
    }
    if (m_layerPanel.wantsAddVideo) {
        std::string path = openFileDialog("Videos\0*.mp4;*.avi;*.mkv;*.mov;*.webm\0All Files\0*.*\0");
        if (!path.empty()) loadVideo(path);
    }
    if (m_layerPanel.wantsAddShader) {
        std::string path = openFileDialog("ISF Shaders\0*.fs;*.frag;*.glsl\0All Files\0*.*\0");
        if (!path.empty()) loadShader(path);
    }

    // --- Masks panel --- (visible only in Stage workspace per UIManager whitelist)
    if (m_ui.isPanelVisible("Masks")) {
    ImGui::Begin("Masks");
    {
        // Edge blend sits at the top of Masks — both are output-stage refinements
        // that clip / blend the composited zone before it's sent to the projector.
        renderEdgeBlendInline(zone);
        ImGui::Dummy(ImVec2(0, 4));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 25));
        }
        ImGui::Dummy(ImVec2(0, 6));

        // ===== Canvas Masks (output-level, applied to entire composite) =====
        auto* canvasMaskMapping = mappingForZone(zone);
        // Zone-colored header
        // Monochrome zone indicators — distinguish zones by lightness, not hue
        static const float zcols[][3] = {
            {0.96f,0.96f,0.96f}, {0.86f,0.86f,0.86f}, {0.76f,0.76f,0.76f}, {0.66f,0.66f,0.66f},
            {0.56f,0.56f,0.56f}, {0.46f,0.46f,0.46f}, {0.80f,0.80f,0.80f}, {0.70f,0.70f,0.70f},
        };
        const float* zc = zcols[m_activeZone % 8];
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
        ImGui::Text("Canvas Masks");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
        ImGui::Text("Clips entire output before warp");
        ImGui::PopStyleColor();
        if (canvasMaskMapping) {
            for (int mi = 0; mi < (int)canvasMaskMapping->masks.size(); mi++) {
                ImGui::PushID(9000 + mi);
                auto& mask = canvasMaskMapping->masks[mi];
                bool isActive = (canvasMaskMapping->activeMaskIndex == mi && m_maskEditMode && m_selectedLayer < 0);

                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0], zc[1], zc[2], 0.25f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.125f, 0.165f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.73f, 0.78f, 1.0f));
                }
                char label[128];
                snprintf(label, sizeof(label), "%s (%d pts)", mask.name.c_str(), mask.path.count());
                float btnW = ImGui::GetContentRegionAvail().x - 28;
                if (ImGui::Button(label, ImVec2(btnW, 0))) {
                    if (isActive) {
                        m_maskEditMode = false;
                        canvasMaskMapping->activeMaskIndex = -1;
                    } else {
                        canvasMaskMapping->activeMaskIndex = mi;
                        m_maskEditMode = true;
                        m_selectedLayer = -1; // deselect layer to signal canvas mask mode
                    }
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                if (ImGui::Button("X", ImVec2(24, 0))) {
                    if (canvasMaskMapping->activeMaskIndex == mi) { m_maskEditMode = false; canvasMaskMapping->activeMaskIndex = -1; }
                    else if (canvasMaskMapping->activeMaskIndex > mi) canvasMaskMapping->activeMaskIndex--;
                    canvasMaskMapping->masks.erase(canvasMaskMapping->masks.begin() + mi);
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                    goto masks_panel_done;
                }
                ImGui::PopStyleColor(2);

                // Feather + Invert inline (when active)
                if (isActive) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("Feather##cmf", &mask.feather, 0.0f, 0.15f, "%.3f");
                    ImGui::Checkbox("Invert##cmi", &mask.invert);
                }

                ImGui::PopID();
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0], zc[1], zc[2], 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0], zc[1], zc[2], 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(zc[0], zc[1], zc[2], 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
            if (ImGui::Button("+ Canvas Mask", ImVec2(-1, 0))) {
                MappingMask newMask;
                newMask.name = "Canvas " + std::to_string(canvasMaskMapping->masks.size() + 1);
                canvasMaskMapping->masks.push_back(std::move(newMask));
                canvasMaskMapping->activeMaskIndex = (int)canvasMaskMapping->masks.size() - 1;
                m_maskEditMode = true;
                m_selectedLayer = -1;
            }
            ImGui::PopStyleColor(4);

            // Shape presets for active canvas mask
            if (m_maskEditMode && m_selectedLayer < 0 && canvasMaskMapping->activeMaskIndex >= 0 &&
                canvasMaskMapping->activeMaskIndex < (int)canvasMaskMapping->masks.size()) {
                auto& mask = canvasMaskMapping->masks[canvasMaskMapping->activeMaskIndex];
                float shapeW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.0f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0], zc[1], zc[2], 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0], zc[1], zc[2], 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(zc[0], zc[1], zc[2], 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
                if (ImGui::Button("Rect##c", ImVec2(shapeW, 0))) { mask.path.makeRectangle({0.5f, 0.5f}, {0.6f, 0.6f}); }
                ImGui::SameLine();
                if (ImGui::Button("Circle##c", ImVec2(shapeW, 0))) { mask.path.makeEllipse({0.5f, 0.5f}, {0.3f, 0.3f}); }
                ImGui::SameLine();
                if (ImGui::Button("Tri##c", ImVec2(shapeW, 0))) { mask.path.makeTriangle({0.5f, 0.5f}, 0.3f); }
                ImGui::SameLine();
                if (ImGui::Button("Oct##c", ImVec2(shapeW, 0))) { mask.path.makePolygon({0.5f, 0.5f}, 0.3f, 8); }
                ImGui::SameLine();
                if (ImGui::Button("Star##c", ImVec2(shapeW, 0))) { mask.path.makeStar({0.5f, 0.5f}, 0.3f, 0.15f, 5); }
                ImGui::PopStyleColor(4);
            }
        }

        // Divider between canvas and layer masks
        ImGui::Dummy(ImVec2(0, 6));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), IM_COL32(255, 255, 255, 40));
        }
        ImGui::Dummy(ImVec2(0, 6));

        // ===== Layer Masks (per-layer, follow layer transform) =====
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Text("Layer Masks");
        ImGui::PopStyleColor();
        {
        // Get selected layer for mask editing
        std::shared_ptr<Layer> maskLayer;
        if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
            maskLayer = m_layerStack[m_selectedLayer];
        }
        if (maskLayer) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("%s", maskLayer->name.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 2));

            for (int mi = 0; mi < (int)maskLayer->masks.size(); mi++) {
                ImGui::PushID(8000 + mi);
                auto& mask = maskLayer->masks[mi];
                bool isActive = (maskLayer->activeMaskIndex == mi && m_maskEditMode);

                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.125f, 0.165f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.73f, 0.78f, 1.0f));
                }

                char label[128];
                snprintf(label, sizeof(label), "%s (%d pts)", mask.name.c_str(), mask.path.count());
                float btnW = ImGui::GetContentRegionAvail().x - 28;
                if (ImGui::Button(label, ImVec2(btnW, 0))) {
                    if (isActive) {
                        m_maskEditMode = false;
                        maskLayer->activeMaskIndex = -1;
                    } else {
                        maskLayer->activeMaskIndex = mi;
                        m_maskEditMode = true;
                    }
                }
                ImGui::PopStyleColor(2);

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                if (ImGui::Button("X", ImVec2(24, 0))) {
                    if (maskLayer->activeMaskIndex == mi) { m_maskEditMode = false; maskLayer->activeMaskIndex = -1; }
                    else if (maskLayer->activeMaskIndex > mi) maskLayer->activeMaskIndex--;
                    maskLayer->masks.erase(maskLayer->masks.begin() + mi);
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                    goto masks_panel_done;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }

            // Add mask button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (ImGui::Button("+ Add Mask", ImVec2(-1, 0))) {
                Layer::LayerMask newMask;
                newMask.name = "Mask " + std::to_string(maskLayer->masks.size() + 1);
                maskLayer->masks.push_back(std::move(newMask));
                maskLayer->activeMaskIndex = (int)maskLayer->masks.size() - 1;
                m_maskEditMode = true;
            }
            ImGui::PopStyleColor(4);

            // Shape presets for active mask
            if (m_maskEditMode && maskLayer->activeMaskIndex >= 0 &&
                maskLayer->activeMaskIndex < (int)maskLayer->masks.size()) {
                auto& mask = maskLayer->masks[maskLayer->activeMaskIndex];
                float shapeW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.0f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("Rect", ImVec2(shapeW, 0))) { mask.path.makeRectangle({0.5f, 0.5f}, {0.6f, 0.6f}); }
                ImGui::SameLine();
                if (ImGui::Button("Circle", ImVec2(shapeW, 0))) { mask.path.makeEllipse({0.5f, 0.5f}, {0.3f, 0.3f}); }
                ImGui::SameLine();
                if (ImGui::Button("Tri", ImVec2(shapeW, 0))) { mask.path.makeTriangle({0.5f, 0.5f}, 0.3f); }
                ImGui::SameLine();
                if (ImGui::Button("Oct", ImVec2(shapeW, 0))) { mask.path.makePolygon({0.5f, 0.5f}, 0.3f, 8); }
                ImGui::SameLine();
                if (ImGui::Button("Star", ImVec2(shapeW, 0))) { mask.path.makeStar({0.5f, 0.5f}, 0.3f, 0.15f, 5); }
                ImGui::PopStyleColor(4);

                // Feather + Invert controls (per-mask)
                ImGui::Dummy(ImVec2(0, 3));
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("Feather##mf", &mask.feather, 0.0f, 0.15f, "%.3f");
                ImGui::Checkbox("Invert##mi", &mask.invert);

                // Alignment tools (visible when 2+ points selected)
                auto& selPts = m_viewportPanel.maskSelectedPoints();
                if (selPts.size() >= 2 && mask.path.count() >= 2) {
                    ImGui::Dummy(ImVec2(0, 3));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::Text("Align (%d pts)", (int)selPts.size());
                    ImGui::PopStyleColor();

                    float alignW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.0f;
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

                    auto& mpts = mask.path.points();
                    if (ImGui::Button("Left##al", ImVec2(alignW, 0))) {
                        float minX = 1.0f;
                        for (int si : selPts) if (si < (int)mpts.size()) minX = std::min(minX, mpts[si].position.x);
                        for (int si : selPts) if (si < (int)mpts.size()) mpts[si].position.x = minX;
                        mask.path.markDirty();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("CtrX##al", ImVec2(alignW, 0))) {
                        float avgX = 0; int c = 0;
                        for (int si : selPts) if (si < (int)mpts.size()) { avgX += mpts[si].position.x; c++; }
                        if (c > 0) { avgX /= c; for (int si : selPts) if (si < (int)mpts.size()) mpts[si].position.x = avgX; }
                        mask.path.markDirty();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Right##al", ImVec2(alignW, 0))) {
                        float maxX = 0.0f;
                        for (int si : selPts) if (si < (int)mpts.size()) maxX = std::max(maxX, mpts[si].position.x);
                        for (int si : selPts) if (si < (int)mpts.size()) mpts[si].position.x = maxX;
                        mask.path.markDirty();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Top##al", ImVec2(alignW, 0))) {
                        float maxY = 0.0f;
                        for (int si : selPts) if (si < (int)mpts.size()) maxY = std::max(maxY, mpts[si].position.y);
                        for (int si : selPts) if (si < (int)mpts.size()) mpts[si].position.y = maxY;
                        mask.path.markDirty();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Bot##al", ImVec2(alignW, 0))) {
                        float minY = 1.0f;
                        for (int si : selPts) if (si < (int)mpts.size()) minY = std::min(minY, mpts[si].position.y);
                        for (int si : selPts) if (si < (int)mpts.size()) mpts[si].position.y = minY;
                        mask.path.markDirty();
                    }
                    ImGui::PopStyleColor(3);
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                ImGui::TextWrapped("Shift+Click: multi-select  |  R-click: del");
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
            ImGui::Text("Select a layer to add masks");
            ImGui::PopStyleColor();
        }
        } // end layer masks scope
        masks_panel_done:;
    }
    ImGui::End();
    }  // end Masks visibility guard

    std::shared_ptr<Layer> selectedLayer;
    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
        selectedLayer = m_layerStack[m_selectedLayer];
    }
    MosaicAudioState mosaicAudio;
    mosaicAudio.selectedDevice = &m_selectedAudioDevice;
    mosaicAudio.bass = m_audioAnalyzer.bass();
    mosaicAudio.lowMid = m_audioAnalyzer.lowMid();
    mosaicAudio.highMid = m_audioAnalyzer.highMid();
    mosaicAudio.treble = m_audioAnalyzer.treble();
    mosaicAudio.beatDecay = m_audioAnalyzer.beatDecay();
#ifdef HAS_FFMPEG
    if (m_audioDevices.empty()) {
        m_audioDevices = VideoRecorder::enumerateAudioDevices();
    }
    for (auto& d : m_audioDevices) {
        mosaicAudio.devices.push_back({d.name, d.isCapture});
    }
#endif
    m_speechState.dataBus = &m_dataBus;
    m_speechState.activeLayerId = selectedLayer ? selectedLayer->id : 0;
    m_speechState.midi = &m_midiManager;

    // Capture undo snapshot BEFORE the property panel modifies values
    SceneSnapshot preEditSnapshot;
    bool capturedPre = false;
    if (!m_propertyPanel.undoNeeded) {
        preEditSnapshot = UndoStack::captureSnapshot(m_layerStack, m_selectedLayer);
        capturedPre = true;
    }

    if (m_ui.isPanelVisible("Properties")) {
        m_propertyPanel.render(selectedLayer, m_maskEditMode, &m_speechState, &mosaicAudio, (float)glfwGetTime(), &m_layerStack, &m_bpmSync, &m_sceneManager, &m_mosaicAudioDevice, &m_midiManager);
    }

    // If a property widget was just activated, push the pre-edit state (before the widget changed it)
    if (m_propertyPanel.undoNeeded) {
        if (capturedPre) {
            m_undoStack.pushSnapshot(std::move(preEditSnapshot));
        }
        m_propertyPanel.undoNeeded = false;
    }

    // Render warp editor FIRST so it can set m_maskEditMode
    auto* mp = mappingForZone(zone);
    if (mp && m_ui.isPanelVisible("Mapping")) {
        auto prevWarpMode = mp->warpMode;
        m_warpEditor.render(*mp, m_maskEditMode, &m_mappings, zone.mappingIndex);

        // Recreate warp FBO with/without depth when mode changes
        if (mp->warpMode != prevWarpMode) {
            bool needsDepth = (mp->warpMode == ViewportPanel::WarpMode::ObjMesh);
            zone.warpFBO.create(zone.width, zone.height, needsDepth);
        }

        // Handle OBJ load request
        if (m_warpEditor.wantsLoadOBJ()) {
            std::string path = openFileDialog("3D Models\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
            if (!path.empty()) {
                mp->objMeshWarp.loadModel(path);
            }
        }
    }

    // Set viewport edit mode AFTER warp editor (which may toggle m_maskEditMode)
    {
        MaskPath* editMaskPath = nullptr;
        glm::mat3 editMaskXform(1.0f);

        // Check for active layer mask
        if (m_maskEditMode && selectedLayer && selectedLayer->activeMaskIndex >= 0 &&
            selectedLayer->activeMaskIndex < (int)selectedLayer->masks.size()) {
            editMaskPath = &selectedLayer->masks[selectedLayer->activeMaskIndex].path;
            // Build the same transform the compositor uses
            editMaskXform = selectedLayer->getTransformMatrix();
            bool mosaicFill = (selectedLayer->tileX > 1.0f || selectedLayer->tileY > 1.0f ||
                               selectedLayer->mosaicMode != MosaicMode::Mirror);
            if (!mosaicFill) {
                int lw = selectedLayer->width(), lh = selectedLayer->height();
                if (lw > 0 && lh > 0 && zone.width > 0 && zone.height > 0) {
                    float srcAspect = (float)lw / lh;
                    float canvasAspect = (float)zone.width / zone.height;
                    glm::mat3 nativeScale(1.0f);
                    nativeScale[0][0] = srcAspect / canvasAspect;
                    nativeScale[1][1] = 1.0f;
                    editMaskXform = editMaskXform * nativeScale;
                }
            }
        }

        // Check for active canvas mask (m_selectedLayer < 0 signals canvas mask mode)
        auto* canvasMapping = mappingForZone(zone);
        if (!editMaskPath && m_maskEditMode && m_selectedLayer < 0 && canvasMapping &&
            canvasMapping->activeMaskIndex >= 0 &&
            canvasMapping->activeMaskIndex < (int)canvasMapping->masks.size()) {
            editMaskPath = &canvasMapping->masks[canvasMapping->activeMaskIndex].path;
            editMaskXform = glm::mat3(1.0f); // canvas masks are in canvas UV (identity)
        }

        if (m_maskEditMode && editMaskPath) {
            m_viewportPanel.setEditMode(ViewportPanel::EditMode::Mask);
            // Canvas masks get zone-colored overlay; layer masks get default (gold)
            int maskZoneIdx = (m_selectedLayer < 0) ? m_activeZone : -1;
            m_viewportPanel.renderMaskOverlay(*editMaskPath, editMaskXform, maskZoneIdx);

            // Save button / Esc handler in the mask banner fires this.
            if (m_viewportPanel.wantsExitMaskMode()) {
                m_viewportPanel.clearExitMaskSignal();
                m_maskEditMode = false;
                // Clear the active mask index so the mask row stops showing as "edit"
                auto* cm = mappingForZone(zone);
                if (cm && m_selectedLayer < 0) cm->activeMaskIndex = -1;
                if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
                    auto& layer = m_layerStack[m_selectedLayer];
                    if (layer) layer->activeMaskIndex = -1;
                }
            }
        } else {
            m_viewportPanel.setEditMode(ViewportPanel::EditMode::Normal);
            m_viewportPanel.renderLayerOverlay(m_layerStack, m_selectedLayer, zone.width, zone.height);
            m_maskEditMode = false;
            // Drop any stray exit signal so it doesn't fire later
            m_viewportPanel.clearExitMaskSignal();
        }
    }

    // Stage View (3D pre-viz) + Scene panel (displays / projectors / surfaces)
    {
        // Collect zone textures shared between the 3D viewport and the Scene panel.
        std::vector<GLuint> zoneTextures;
        for (auto& zp : m_zones) {
            zoneTextures.push_back(zp->warpFBO.textureId());
        }

        if (m_ui.isPanelVisible("Stage")) {
            // No inline top section — Composition + Output are in the Canvas
            // zone bar now, so the Stage panel is pure 3D viewport.
            m_stageView.render(zoneTextures, nullptr);

            // Handle import request from StageView
            if (m_stageView.wantsImport()) {
                m_stageView.clearImportSignal();
                std::string path = openFileDialog("3D Models\0*.obj;*.gltf;*.glb\0All Files\0*.*\0");
                if (!path.empty()) {
                    m_stageView.loadModel(path);
                }
            }
        }

        // Scene panel — lists of displays/projectors/surfaces/clusters, docked
        // on the right so the 3D viewport in the Stage tab gets full height.
        if (m_ui.isPanelVisible("Scene")) {
            ImGui::Begin("Scene");
            m_stageView.renderSceneInspector(zoneTextures);
            ImGui::End();
        }
    }

    // Projector settings are now rendered inline inside the Canvas tab
    // via renderCompositionInlinePanel(). The standalone "Projector" window is gone.

    // Sources panel — single window with tabs for each input source type.
    // Groups what used to be 5 separate panels (NDI / Spout / Capture /
    // ShaderClaw / Etherea) into one place so the right column isn't
    // overwhelmed with tabs.
    bool sourcesOpen = ImGui::Begin("Sources");
    ImGuiTabBarFlags sourcesTabFlags = ImGuiTabBarFlags_Reorderable
                                     | ImGuiTabBarFlags_FittingPolicyScroll;
    bool sourcesTabsOpen = sourcesOpen && ImGui::BeginTabBar("##SourcesTabs", sourcesTabFlags);

    // NDI tab (moved from position 4 to 1)
#ifdef HAS_NDI
    if (sourcesTabsOpen && NDIRuntime::instance().isAvailable() && ImGui::BeginTabItem("NDI")) {
        {
            // --- Broadcasting section ---
            if (ImGui::CollapsingHeader("Broadcasting", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Composition output toggle
                {
                    bool compositionOn = m_ndiOutputEnabled && m_ndiOutput.isActive();
                    if (compositionOn) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    }
                    if (ImGui::Checkbox("Easel  (composition)", &m_ndiOutputEnabled)) {
                        if (m_ndiOutputEnabled && !m_ndiOutput.isActive()) {
                            m_ndiOutput.create("Easel");
                        } else if (!m_ndiOutputEnabled && m_ndiOutput.isActive()) {
                            m_ndiOutput.destroy();
                        }
                    }
                    ImGui::PopStyleColor();
                }

                // Per-layer toggles
                for (int i = 0; i < m_layerStack.count(); i++) {
                    ImGui::PushID(5000 + i);
                    auto& layer = m_layerStack[i];
                    bool active = layer->ndiSender.isActive();

                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    }

                    std::string label = "Easel - " + layer->name;
                    if (label.length() > 50) label = label.substr(0, 47) + "...";
                    if (ImGui::Checkbox(label.c_str(), &layer->ndiEnabled)) {
                        // Toggle handled in updateSources
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }

                if (m_layerStack.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::Text("  No layers");
                    ImGui::PopStyleColor();
                }
            }

            // --- Receive section ---
            if (ImGui::CollapsingHeader("Receive", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
                    m_ndiSources = m_ndiFinder.sources();
                }
                ImGui::PopStyleColor(4);

                // Auto-refresh source list every ~2 seconds
                {
                    static double lastRefresh = 0;
                    double now = glfwGetTime();
                    if (now - lastRefresh > 2.0) {
                        m_ndiSources = m_ndiFinder.sources();
                        lastRefresh = now;
                    }
                }

                ImGui::Dummy(ImVec2(0, 2));
                if (m_ndiSources.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::TextWrapped("No NDI sources found on the network.");
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

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    if (ImGui::SmallButton("Add")) {
                        addNDISource(m_ndiSources[i].name);
                    }
                    ImGui::PopStyleColor(4);

                    ImGui::PopID();
                }

#ifdef HAS_WHEP
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 2));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                ImGui::Text("Scope (WHEP)");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.5f, 1.0f, 1.0f));
                if (ImGui::Button("Connect Scope", ImVec2(-1, 0))) {
                    addWHEPSource(WHEPSource::discoverUrl());
                }
                ImGui::PopStyleColor(4);
#endif
            }
        }
        ImGui::EndTabItem();
    }
#else
    if (sourcesTabsOpen && ImGui::BeginTabItem("NDI")) {
        ImGui::TextDisabled("NDI SDK not installed");
        ImGui::TextWrapped("Place NDI SDK headers in external/ndi/include/ and rebuild to enable NDI support.");
        ImGui::EndTabItem();
    }
#endif

    // ShaderClaw tab
    if (sourcesTabsOpen && ImGui::BeginTabItem("ShaderClaw")) {
    {
        if (!m_shaderClaw.isConnected()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextWrapped("Connect to a Shader-Claw shaders directory to browse and hot-reload ISF shaders.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));

            // Auto-detect common locations
            static char scPath[512] = "";
            if (scPath[0] == '\0') {
#ifdef _WIN32
                const char* home = getenv("USERPROFILE");
#else
                const char* home = getenv("HOME");
#endif
                if (home) {
                    // Try known ShaderClaw locations
                    std::string candidates[] = {
#ifdef _WIN32
                        std::string(home) + "\\ShaderClaw3\\shaders",
                        std::string(home) + "\\Documents\\ShaderClaw3\\shaders",
                        std::string(home) + "\\Documents\\ShaderClaw\\shaders",
                        std::string(home) + "\\shader-claw3\\shaders",
#else
                        std::string(home) + "/ShaderClaw3/shaders",
                        std::string(home) + "/Documents/ShaderClaw3/shaders",
                        std::string(home) + "/conductor/workspaces/macbook-migration/doha/ShaderClaw3/shaders",
#endif
                    };
                    for (const auto& tryPath : candidates) {
                        if (std::filesystem::exists(tryPath)) {
                            strncpy(scPath, tryPath.c_str(), sizeof(scPath) - 1);
                            break;
                        }
                    }
                }
            }

            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##SCPath", scPath, sizeof(scPath));

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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
                m_scThumbnails.clear();
                m_scThumbRenderer.reset();
                m_scPreview.reset();
                m_scPreviewPath.clear();
            }
            ImGui::PopStyleColor(4);

            // Refresh button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
                m_shaderClaw.refreshManifest();
            }
            ImGui::PopStyleColor(4);

            ImGui::Dummy(ImVec2(0, 4));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 40));
            ImGui::Dummy(ImVec2(0, 6));

            // Update animated preview shader (for hovered item)
            bool previewValid = false;
            if (m_scPreview) {
                m_scPreview->setResolution(64, 64);
                m_scPreview->update();
                m_scPreviewFrame++;
                previewValid = (m_scPreviewFrame > 2);
            }

            // Generate static thumbnails (one shader per frame to avoid lag)
            if (m_scThumbRenderer) {
                m_scThumbRenderer->setResolution(48, 48);
                m_scThumbRenderer->update();
                m_scThumbRenderFrame++;
                if (m_scThumbRenderFrame > 3) {
                    // Capture the rendered frame into a static texture
                    auto& entry = m_scThumbnails[m_scThumbRenderPath];
                    if (!entry.texture) entry.texture = std::make_shared<Texture>();
                    // Read pixels from the shader FBO
                    std::vector<uint8_t> pixels(48 * 48 * 4);
                    GLint prevFBO;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                    // ShaderSource renders to its own FBO, read from its texture
                    GLuint thumbTex = m_scThumbRenderer->textureId();
                    if (thumbTex != 0) {
                        GLuint tempFBO;
                        glGenFramebuffers(1, &tempFBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, tempFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, thumbTex, 0);
                        glReadPixels(0, 0, 48, 48, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                        glDeleteFramebuffers(1, &tempFBO);
                        entry.texture->createEmpty(48, 48);
                        entry.texture->updateData(pixels.data(), 48, 48);
                        entry.ready = true;
                    }
                    m_scThumbRenderer.reset();
                    m_scThumbRenderPath.clear();
                }
            } else {
                // Find next shader that needs a thumbnail
                for (const auto& shader : m_shaderClaw.shaders()) {
                    auto it = m_scThumbnails.find(shader.fullPath);
                    if (it == m_scThumbnails.end() || !it->second.ready) {
                        m_scThumbRenderer = std::make_shared<ShaderSource>();
                        if (m_scThumbRenderer->loadFromFile(shader.fullPath)) {
                            m_scThumbRenderPath = shader.fullPath;
                            m_scThumbRenderFrame = 0;
                        } else {
                            m_scThumbRenderer.reset();
                            // Mark as failed so we don't retry
                            m_scThumbnails[shader.fullPath].ready = true;
                        }
                        break;
                    }
                }
            }

            // Shader list with always-visible thumbnails
            std::string hoveredPath;
            float thumbSize = 36.0f;
            for (int i = 0; i < (int)m_shaderClaw.shaders().size(); i++) {
                const auto& shader = m_shaderClaw.shaders()[i];
                ImGui::PushID(2000 + i);

                // Show thumbnail (animated if hovered, static otherwise)
                bool isHoveredShader = (shader.fullPath == m_scPreviewPath);
                GLuint thumbTex = 0;
                if (isHoveredShader && previewValid && m_scPreview) {
                    thumbTex = m_scPreview->textureId();
                } else {
                    auto it = m_scThumbnails.find(shader.fullPath);
                    if (it != m_scThumbnails.end() && it->second.ready && it->second.texture)
                        thumbTex = it->second.texture->id();
                }

                if (thumbTex) {
                    ImGui::Image((ImTextureID)(intptr_t)thumbTex,
                                 ImVec2(thumbSize, thumbSize), ImVec2(0, 1), ImVec2(1, 0));
                    ImGui::SameLine();
                } else {
                    // Placeholder
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(p,
                        ImVec2(p.x + thumbSize, p.y + thumbSize), IM_COL32(30, 35, 45, 255), 3.0f);
                    ImGui::Dummy(ImVec2(thumbSize, thumbSize));
                    ImGui::SameLine();
                }

                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::SmallButton("+")) {
                    loadShader(shader.fullPath);
                }
                ImGui::PopStyleColor(4);

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, isHoveredShader
                    ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                    : ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                ImGui::Text("%s", shader.title.c_str());
                ImGui::PopStyleColor();

                if (!shader.description.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.8f));
                    std::string desc = shader.description;
                    if (desc.length() > 40) desc = desc.substr(0, 37) + "...";
                    ImGui::Text("%s", desc.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::EndGroup();

                // Load animated preview when hovered
                if (ImGui::IsItemHovered()) {
                    hoveredPath = shader.fullPath;
                }

                ImGui::PopID();
            }

            // Load/switch animated preview shader on hover
            if (!hoveredPath.empty() && hoveredPath != m_scPreviewPath) {
                m_scPreview = std::make_shared<ShaderSource>();
                if (m_scPreview->loadFromFile(hoveredPath)) {
                    m_scPreviewPath = hoveredPath;
                    m_scPreviewFrame = 0;
                } else {
                    m_scPreview.reset();
                    m_scPreviewPath.clear();
                }
            } else if (hoveredPath.empty()) {
                m_scPreview.reset();
                m_scPreviewPath.clear();
            }
        }
    }
    ImGui::EndTabItem();
    }  // end ShaderClaw tab

    // Etherea tab
    if (sourcesTabsOpen && ImGui::BeginTabItem("Etherea")) {
    {
        if (!m_ethereaClient.isRunning()) {
            static char etUrl[256] = "http://localhost:7860";
            static std::vector<EthereaSession> sessions;
            static int selectedSession = -1;
            static float lastFetch = -10.0f;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextWrapped("Connect to Etherea for live transcript via Deepgram.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));

            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##EtUrl", etUrl, sizeof(etUrl));

            // Fetch sessions button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (ImGui::Button("Refresh Sessions", ImVec2(-1, 0))) {
                sessions = EthereaClient::fetchSessions(etUrl);
                // Sort: active with transcript first, then by idle time
                std::sort(sessions.begin(), sessions.end(), [](const EthereaSession& a, const EthereaSession& b) {
                    // Non-paused with transcript wins
                    int scoreA = (!a.isPaused ? 2 : 0) + (a.transcriptLength > 0 ? 1 : 0);
                    int scoreB = (!b.isPaused ? 2 : 0) + (b.transcriptLength > 0 ? 1 : 0);
                    if (scoreA != scoreB) return scoreA > scoreB;
                    return a.idleSeconds < b.idleSeconds;
                });
                selectedSession = sessions.empty() ? -1 : 0;
            }
            ImGui::PopStyleColor(4);

            // Session list — only show active/interesting sessions
            if (!sessions.empty()) {
                ImGui::Dummy(ImVec2(0, 2));
                int shown = 0;
                for (int i = 0; i < (int)sessions.size(); i++) {
                    const auto& s = sessions[i];
                    // Hide empty paused sessions older than 30s
                    if (s.isPaused && s.transcriptLength == 0 && s.idleSeconds > 30.0f) continue;

                    bool selected = (i == selectedSession);

                    std::string label = s.id.substr(0, 8) + "...";
                    if (s.isPaused) label += " (paused)";
                    else if (s.transcriptLength > 0) label += " (" + std::to_string(s.transcriptLength) + " chars)";
                    else label += " (active)";

                    if (ImGui::RadioButton(label.c_str(), selected)) {
                        selectedSession = i;
                    }
                    shown++;
                }
                if (shown == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.5f));
                    ImGui::Text("No active sessions found");
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (ImGui::Button("Connect", ImVec2(-1, 0))) {
                std::string sid = (selectedSession >= 0 && selectedSession < (int)sessions.size())
                    ? sessions[selectedSession].id : "";
                m_ethereaClient.connect(etUrl, sid);
            }
            ImGui::PopStyleColor(4);
        } else {
            // Connected state — show WS + SSE status
            {
                bool ws = m_ethereaClient.wsConnected();
                bool sse = m_ethereaClient.sseConnected();
                if (ws && sse) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    ImGui::Text("WS + SSE");
                } else if (ws || sse) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.13f, 1.0f));
                    ImGui::Text("%s only", ws ? "WS" : "SSE");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.13f, 1.0f));
                    ImGui::Text("Connecting...");
                }
                ImGui::PopStyleColor();
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::SmallButton("Disconnect")) {
                m_ethereaClient.disconnect();
            }
            ImGui::PopStyleColor(4);

            // Transcript
            ImGui::Dummy(ImVec2(0, 4));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 40));
            ImGui::Dummy(ImVec2(0, 6));

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Transcript");
            ImGui::PopStyleColor();

            std::string transcript = m_ethereaClient.fullTranscript();
            if (!transcript.empty()) {
                if (transcript.size() > 200) transcript = "..." + transcript.substr(transcript.size() - 197);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 0.9f));
                ImGui::TextWrapped("%s", transcript.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.5f));
                ImGui::TextWrapped("Waiting for speech...");
                ImGui::PopStyleColor();
            }

            // Hints
            auto hints = m_ethereaClient.hints();
            if (!hints.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Hints");
                ImGui::PopStyleColor();
                for (int i = 0; i < (int)hints.size() && i < 3; i++) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.82f, 1.0f, 0.85f));
                    ImGui::TextWrapped("%d. %s", i + 1, hints[i].c_str());
                    ImGui::PopStyleColor();
                }
            }

            // Prompt
            std::string prompt = m_ethereaClient.prompt();
            if (!prompt.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Prompt");
                ImGui::PopStyleColor();
                if (prompt.size() > 120) prompt = prompt.substr(0, 117) + "...";
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.78f, 0.55f, 0.9f));
                ImGui::TextWrapped("%s", prompt.c_str());
                ImGui::PopStyleColor();
            }
        }

#ifdef HAS_WHEP
        // Scope Stream — auto-detect live pods via etherea health API
        ImGui::Dummy(ImVec2(0, 6));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(100, 0, 255, 60));
            ImGui::Dummy(ImVec2(0, 6));
        }

        if (ImGui::CollapsingHeader("Scope Stream", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Auto-refresh scope pods every 5 seconds
            static float lastScopeFetch = -10.0f;
            static std::vector<std::pair<std::string, std::string>> scopePods; // {id, prompt snippet}
            float now = (float)glfwGetTime();
            if (now - lastScopeFetch > 5.0f) {
                lastScopeFetch = now;
                std::string healthStr = httpRequest("GET", "http://localhost:7860/api/scope/health", "", "");
                scopePods.clear();
                try {
                    auto health = nlohmann::json::parse(healthStr);
                    for (auto& inst : health["instances"]) {
                        if (inst.value("responding", false)) {
                            std::string id = inst.value("instance_id", "");
                            std::string prompt;
                            auto& p = inst["last_confirmed_prompt"];
                            if (p.is_string()) {
                                prompt = p.get<std::string>();
                                if (prompt.size() > 40) prompt = prompt.substr(0, 37) + "...";
                            }
                            if (!id.empty()) scopePods.push_back({id, prompt});
                        }
                    }
                } catch (...) {}
            }

            // Show WHEP connection status
            if (m_whepConnecting) {
                if (m_whepConnecting->isConnected()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.4f, 1.0f));
                    ImGui::Text("Receiving stream");
                    ImGui::PopStyleColor();
                    if (m_whepConnecting->width() > 0) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                        ImGui::Text("(%dx%d)", m_whepConnecting->width(), m_whepConnecting->height());
                        ImGui::PopStyleColor();
                    }
                } else if (m_whepConnecting->isFailed()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    ImGui::TextWrapped("Failed: %s", m_whepConnecting->statusText().c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
                    std::string status = m_whepConnecting->statusText();
                    ImGui::Text("Connecting%s", status.empty() ? "..." : (" (" + status + ")").c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy(ImVec2(0, 2));
            }

            if (scopePods.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.6f));
                ImGui::TextWrapped("No active Scope instances");
                ImGui::PopStyleColor();
            }

            for (int i = 0; i < (int)scopePods.size(); i++) {
                ImGui::PushID(2000 + i);
                auto& [podId, podPrompt] = scopePods[i];

                // Pod ID (truncated)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.65f, 0.75f, 1.0f));
                ImGui::Text("%s", podId.substr(0, 12).c_str());
                ImGui::PopStyleColor();
                if (!podPrompt.empty()) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                    ImGui::Text("%s", podPrompt.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.5f, 1.0f, 1.0f));
                if (ImGui::SmallButton("Add")) {
                    // Use the mediamtx WHEP URL for this pod — the WKWebView path
                    // will automatically route through Scope's native WebRTC
                    std::string whepUrl = "https://" + podId + "-18889.proxy.runpod.net/longlive/whep";
                    addWHEPSource(whepUrl);
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }
#endif
    }
    ImGui::EndTabItem();
    }  // end Etherea tab

    // Capture tab (moved from position 1 to 4)
    if (sourcesTabsOpen && ImGui::BeginTabItem("Capture")) {
    {
#if defined(_WIN32) || defined(__APPLE__)
        if (ImGui::CollapsingHeader("Screen Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto capMonitors = CaptureSource::enumerateMonitors();
            for (int i = 0; i < (int)capMonitors.size(); i++) {
                ImGui::PushID(i);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::Text("%s  %dx%d", capMonitors[i].name.c_str(),
                            capMonitors[i].width, capMonitors[i].height);
                ImGui::PopStyleColor();
                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("Add")) {
                    addScreenCapture(i);
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Window Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("Add")) {
#ifdef _WIN32
                    addWindowCapture(m_windowList[i].hwnd, m_windowList[i].title);
#elif defined(__APPLE__)
                    addWindowCapture(m_windowList[i].windowID, m_windowList[i].title);
#endif
                }
                ImGui::PopStyleColor(4);

                ImGui::PopID();
            }
        }
#else
        ImGui::TextDisabled("Desktop capture is not available on Linux yet.");
        ImGui::TextWrapped("Use video files, shader sources, NDI, WHEP, or external network sources on this build.");
#endif
    }
    ImGui::EndTabItem();
    }  // end Capture tab

#ifdef HAS_SPOUT
    if (sourcesTabsOpen && ImGui::BeginTabItem("Spout")) {
    {
        bool spoutOn = m_spoutOutputEnabled && m_spoutOutput.isActive();
        if (spoutOn) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        }
        if (ImGui::Checkbox("Easel  (composition)", &m_spoutOutputEnabled)) {
            if (m_spoutOutputEnabled && !m_spoutOutput.isActive()) {
                auto& az = activeZone();
                m_spoutOutput.create("Easel", az.warpFBO.width(), az.warpFBO.height());
            } else if (!m_spoutOutputEnabled && m_spoutOutput.isActive()) {
                m_spoutOutput.destroy();
            }
        }
        ImGui::PopStyleColor();

        if (spoutOn) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 0.7f));
            ImGui::Text("Sending: %s", m_spoutOutput.name().c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTabItem();
    }  // end Spout tab
#endif

    // Close the Sources tab bar + window (opened before the Capture tab).
    if (sourcesTabsOpen) ImGui::EndTabBar();
    ImGui::End();

#ifdef HAS_FFMPEG
    if (m_ui.isPanelVisible("Stream")) {
    ImGui::Begin("Stream");
    {
        // Stream settings only — actions are in the transport bar
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("YouTube Stream Key");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##StreamKey", m_streamKeyBuf, sizeof(m_streamKeyBuf),
                         ImGuiInputTextFlags_Password);

        static const char* aspectNames[] = { "16:9", "4:3", "16:10", "Source" };
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Aspect");
        ImGui::PopStyleColor();
        ImGui::SameLine(58);
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##Aspect", &m_streamAspect, aspectNames, 4);

        if (m_rtmpOutput.isActive()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            int secs = (int)m_rtmpOutput.uptimeSeconds();
            ImGui::Text("LIVE  %02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
            ImGui::PopStyleColor();
            if (m_rtmpOutput.droppedFrames() > 0) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                ImGui::Text("(%d dropped)", m_rtmpOutput.droppedFrames());
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.38f, 0.44f, 1.0f));
            ImGui::TextWrapped("Use the transport bar below to go live or record.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    }  // end Stream visibility guard
#endif

    // Audio panel — BPM, device, levels, gain controls
    if (m_ui.isPanelVisible("Audio")) {
    ImGui::Begin("Audio");
    {
        // --- BPM (moved from Properties panel) ---
        {
            float currentBPM = m_bpmSync.bpm();
            float w = ImGui::GetContentRegionAvail().x;

            // Beat indicator dots + BPM text
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float dotY = p.y + 8;
                for (int b = 0; b < 4; b++) {
                    float dotCX = p.x + b * 16.0f;
                    int beatInBar = m_bpmSync.beatCount() % 4;
                    bool isCurrent = (b == beatInBar) && currentBPM > 0;
                    float pulse = isCurrent ? m_bpmSync.beatPulse() : 0.0f;
                    float r = 4.0f + pulse * 2.0f;
                    dl->AddCircleFilled(ImVec2(dotCX + 6, dotY), r,
                                        isCurrent ? IM_COL32(0, 220, 255, (int)(140 + pulse * 115))
                                                  : IM_COL32(50, 60, 80, 120));
                }
                char bpmBuf[16];
                if (currentBPM > 0) snprintf(bpmBuf, sizeof(bpmBuf), "%.1f BPM", currentBPM);
                else snprintf(bpmBuf, sizeof(bpmBuf), "--- BPM");
                dl->AddText(ImVec2(p.x + 74, p.y + 2),
                            currentBPM > 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 115, 140, 180),
                            bpmBuf);
                ImGui::Dummy(ImVec2(w, 18));
            }

            // TAP + BPM input + Reset
            {
                float btnW = (w - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("TAP", ImVec2(btnW, 0))) m_bpmSync.tap();
                ImGui::PopStyleColor(4);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(btnW);
                float bpmVal = currentBPM;
                if (ImGui::DragFloat("##BPMVal", &bpmVal, 0.5f, 0.0f, 300.0f, "%.0f BPM"))
                    m_bpmSync.setBPM(bpmVal);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.05f, 0.05f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.1f, 0.1f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.35f, 0.35f, 0.8f));
                if (ImGui::Button("Reset", ImVec2(btnW, 0))) {
                    m_bpmSync.setBPM(0);
                    m_bpmSync.resetPhase();
                }
                ImGui::PopStyleColor(3);
            }
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));
        }

        // --- Device selection ---
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Input");
        ImGui::PopStyleColor();

#ifdef HAS_FFMPEG
        if (m_audioDevices.empty()) {
            m_audioDevices = VideoRecorder::enumerateAudioDevices();
        }
        const char* currentName = "System Audio (loopback)";
        if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
            currentName = m_audioDevices[m_selectedAudioDevice].name.c_str();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##AudioInput", currentName)) {
            if (ImGui::Selectable("System Audio (loopback)", m_selectedAudioDevice == -1)) {
                m_selectedAudioDevice = -1;
            }
            for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                const auto& d = m_audioDevices[i];
                char label[256];
                snprintf(label, sizeof(label), "%s%s", d.name.c_str(),
                         d.isCapture ? "  (mic)" : "  (loopback)");
                if (ImGui::Selectable(label, m_selectedAudioDevice == i)) {
                    m_selectedAudioDevice = i;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::SmallButton("Refresh##audio")) {
            m_audioDevices = VideoRecorder::enumerateAudioDevices();
        }
#else
        ImGui::TextDisabled("Audio device selection requires FFmpeg build");
#endif

        ImGui::Dummy(ImVec2(0, 4));

        // --- Big 4-band meters (Bass / LowMid / HighMid / Treble) ---
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Levels");
        ImGui::PopStyleColor();

        {
            float avail = ImGui::GetContentRegionAvail().x;
            float barH = 80.0f;
            float bandW = (avail - 9.0f) / 4.0f; // 3px gaps between bars
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            struct BandInfo { const char* name; float level; ImU32 color; };
            BandInfo bands[4] = {
                { "BASS", m_audioAnalyzer.bass(),    IM_COL32(220, 60, 60, 230) },
                { "LOW",  m_audioAnalyzer.lowMid(),  IM_COL32(230, 150, 30, 230) },
                { "HI",   m_audioAnalyzer.highMid(), IM_COL32(60, 200, 90, 230) },
                { "TREB", m_audioAnalyzer.treble(),  IM_COL32(30, 200, 220, 230) },
            };

            for (int b = 0; b < 4; b++) {
                float x0 = origin.x + b * (bandW + 3.0f);
                float x1 = x0 + bandW;
                // Background
                draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + barH),
                                    IM_COL32(18, 22, 30, 255), 3.0f);
                // Level fill (bottom-up)
                float h = bands[b].level * barH;
                draw->AddRectFilled(ImVec2(x0 + 1, origin.y + barH - h),
                                    ImVec2(x1 - 1, origin.y + barH - 1),
                                    bands[b].color, 2.0f);
                // Border
                draw->AddRect(ImVec2(x0, origin.y), ImVec2(x1, origin.y + barH),
                              IM_COL32(255, 255, 255, 40), 3.0f);
                // Label at bottom
                ImVec2 ts = ImGui::CalcTextSize(bands[b].name);
                draw->AddText(ImVec2(x0 + (bandW - ts.x) * 0.5f, origin.y + barH + 2),
                              IM_COL32(150, 160, 180, 255), bands[b].name);
                // Numeric value at top
                char vbuf[16];
                snprintf(vbuf, sizeof(vbuf), "%.2f", bands[b].level);
                ImVec2 vts = ImGui::CalcTextSize(vbuf);
                draw->AddText(ImVec2(x0 + (bandW - vts.x) * 0.5f, origin.y + 2),
                              IM_COL32(220, 230, 245, 200), vbuf);
            }
            ImGui::Dummy(ImVec2(avail, barH + ImGui::GetFontSize() + 6));
        }

        // --- RMS + Beat indicator ---
        {
            float avail = ImGui::GetContentRegionAvail().x;
            float h = 14.0f;
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            // RMS bar
            float rmsW = avail - 26.0f;
            draw->AddRectFilled(origin, ImVec2(origin.x + rmsW, origin.y + h),
                                IM_COL32(18, 22, 30, 255), 2.0f);
            float rms = m_audioAnalyzer.smoothedRMS();
            draw->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1),
                                ImVec2(origin.x + 1 + (rmsW - 2) * rms, origin.y + h - 1),
                                IM_COL32(100, 160, 255, 220), 2.0f);
            // Beat dot (right side)
            float beat = m_audioAnalyzer.beatDecay();
            ImU32 beatCol = IM_COL32((int)(50 + 205 * beat), (int)(50 + 100 * beat),
                                     (int)(50 + 50 * beat), 255);
            draw->AddCircleFilled(ImVec2(origin.x + avail - 10, origin.y + h * 0.5f),
                                  5.0f + beat * 4.0f, beatCol);
            ImGui::Dummy(ImVec2(avail, h + 2));
        }

        ImGui::Dummy(ImVec2(0, 6));

        // --- Gain controls ---
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Gain");
        ImGui::PopStyleColor();

        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Input##masterGain", &m_audioAnalyzer.inputGain(), 0.0f, 10.0f, "%.2fx");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Bass##bGain", &m_audioAnalyzer.bassGain(), 0.0f, 5.0f, "%.2fx");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Low##lmGain", &m_audioAnalyzer.lowMidGain(), 0.0f, 5.0f, "%.2fx");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("High##hmGain", &m_audioAnalyzer.highMidGain(), 0.0f, 5.0f, "%.2fx");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Treble##tGain", &m_audioAnalyzer.trebleGain(), 0.0f, 5.0f, "%.2fx");

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Gate##nGate", &m_audioAnalyzer.noiseGate(), 0.0f, 0.5f, "%.2f");

        if (ImGui::SmallButton("Reset Gains")) {
            m_audioAnalyzer.inputGain() = 1.0f;
            m_audioAnalyzer.bassGain() = 1.0f;
            m_audioAnalyzer.lowMidGain() = 1.0f;
            m_audioAnalyzer.highMidGain() = 1.0f;
            m_audioAnalyzer.trebleGain() = 1.0f;
            m_audioAnalyzer.noiseGate() = 0.0f;
        }
    }
    ImGui::End();
    }  // end Audio visibility guard

    // MIDI panel — device selection + mapping
    if (m_ui.isPanelVisible("MIDI")) {
    ImGui::Begin("MIDI");
    {
        // Device selection
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Device");
        ImGui::PopStyleColor();

        auto devices = m_midiManager.listDevices();
        if (devices.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
            ImGui::TextWrapped("No MIDI devices found. Plug in a controller and reopen this panel.");
            ImGui::PopStyleColor();
        } else {
            // Current device label
            const char* currentLabel = m_midiManager.isOpen()
                ? devices[m_midiManager.deviceIndex()].c_str() : "None";

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##MIDIDevice", currentLabel)) {
                // "None" option to disconnect
                if (ImGui::Selectable("None", !m_midiManager.isOpen())) {
                    m_midiManager.closeDevice();
                }
                for (int i = 0; i < (int)devices.size(); i++) {
                    bool selected = (m_midiManager.isOpen() && m_midiManager.deviceIndex() == i);
                    if (ImGui::Selectable(devices[i].c_str(), selected)) {
                        m_midiManager.openDevice(i);
                    }
                }
                ImGui::EndCombo();
            }

            // Connection status
            if (m_midiManager.isOpen()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                ImGui::Text("Connected");
                ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy(ImVec2(0, 6));

        // MIDI activity monitor (last received event)
        if (m_midiManager.isOpen()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Mappings");
            ImGui::PopStyleColor();

            // Add mapping with learn mode
            if (!m_midiManager.isLearning()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("+ Learn Mapping", ImVec2(-1, 0))) {
                    m_midiManager.startLearn();
                }
                ImGui::PopStyleColor(4);
            } else {
                // Learning mode — waiting for MIDI input
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
                ImGui::TextWrapped("Move a knob or press a button on your controller...");
                ImGui::PopStyleColor();

                MIDIEvent learned = m_midiManager.lastLearnEvent();
                if (learned.value > 0) {
                    // Got an event — show what was detected and offer target selection
                    ImGui::Text("Detected: %s Ch%d #%d",
                        learned.type == 0 ? "CC" : (learned.type == 1 ? "NoteOn" : "NoteOff"),
                        learned.channel + 1, learned.number);

                    static int learnTarget = 0;
                    const char* targetNames[] = {
                        "Layer Opacity", "Layer Visible", "Layer Pos X", "Layer Pos Y",
                        "Layer Scale", "Layer Rotation", "Scene Recall", "BPM Set", "BPM Tap"
                    };
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##LearnTarget", &learnTarget, targetNames, IM_ARRAYSIZE(targetNames));

                    static int learnLayerIdx = 0;
                    if (learnTarget <= 5) { // Layer targets
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##LearnLayer",
                            (learnLayerIdx < m_layerStack.count()) ? m_layerStack[learnLayerIdx]->name.c_str() : "Layer 0")) {
                            for (int li = 0; li < m_layerStack.count(); li++) {
                                if (ImGui::Selectable(m_layerStack[li]->name.c_str(), learnLayerIdx == li))
                                    learnLayerIdx = li;
                            }
                            ImGui::EndCombo();
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    if (ImGui::Button("Assign", ImVec2(-1, 0))) {
                        MIDIMapping map;
                        map.channel = learned.channel;
                        map.type = (learned.type == 0) ? 0 : 1;
                        map.number = learned.number;
                        map.target = (MIDIMapping::Target)learnTarget;
                        map.layerIndex = learnLayerIdx;
                        m_midiManager.addMapping(map);
                        m_midiManager.stopLearn();
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        m_midiManager.stopLearn();
                    }
                } else {
                    if (ImGui::Button("Cancel")) {
                        m_midiManager.stopLearn();
                    }
                }
            }

            // Show existing mappings
            ImGui::Dummy(ImVec2(0, 4));
            int removeIdx = -1;
            for (int mi = 0; mi < (int)m_midiManager.mappings().size(); mi++) {
                const auto& map = m_midiManager.mappings()[mi];
                ImGui::PushID(5000 + mi);

                const char* typeStr = (map.type == 0) ? "CC" : "Note";
                const char* targetNames[] = {
                    "Opacity", "Visible", "PosX", "PosY", "Scale", "Rotation",
                    "Scene", "BPM", "Tap"
                };
                const char* tgt = targetNames[(int)map.target];

                char label[128];
                if ((int)map.target <= 5) {
                    const char* layerName = (map.layerIndex < m_layerStack.count())
                        ? m_layerStack[map.layerIndex]->name.c_str() : "?";
                    snprintf(label, sizeof(label), "%s Ch%d #%d -> %s [%s]",
                        typeStr, map.channel + 1, map.number, tgt, layerName);
                } else {
                    snprintf(label, sizeof(label), "%s Ch%d #%d -> %s",
                        typeStr, map.channel + 1, map.number, tgt);
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.75f, 0.82f, 1.0f));
                ImGui::Text("%s", label);
                ImGui::PopStyleColor();

                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                if (ImGui::SmallButton("X")) removeIdx = mi;
                ImGui::PopStyleColor(2);

                ImGui::PopID();
            }
            if (removeIdx >= 0) m_midiManager.removeMapping(removeIdx);
        }
    }
    ImGui::End();
    }  // end MIDI visibility guard

#if defined(HAS_OPENCV) && !defined(__APPLE__)
    if (m_ui.isPanelVisible("Scene Scanner")) {
        m_scanPanel.render(m_scanner, m_webcam);
    }
#endif

    // Audio Mixer panel
#ifdef HAS_FFMPEG
    if (m_ui.isPanelVisible("Audio Mixer")) {
    ImGui::Begin("Audio Mixer");
    {
        // Enable/disable toggle
        if (ImGui::Checkbox("Enable Mixer", &m_mixerEnabled)) {
            if (m_mixerEnabled) {
                m_audioAnalyzer.setExternalFeed(true);
                if (m_audioMixer.inputCount() == 0)
                    m_audioMixer.addInput("", "System Audio", false);
                m_audioMixer.start();
            } else {
                m_audioMixer.stop();
                m_audioAnalyzer.setExternalFeed(false);
            }
        }

        if (m_mixerEnabled) {
            ImGui::Dummy(ImVec2(0, 4));

            // Output device selector
            {
                std::string outName = m_audioMixer.outputDeviceName();
                if (outName.empty()) outName = "Default Output";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##MixerOut", outName.c_str())) {
                    if (ImGui::Selectable("Default Output", m_mixerOutputDevice == -1)) {
                        m_mixerOutputDevice = -1;
                        m_audioMixer.setOutputDevice("", "Default Output");
                    }
                    if (ImGui::Selectable("None (NDI only)", m_mixerOutputDevice == -2)) {
                        m_mixerOutputDevice = -2;
                        m_audioMixer.setOutputDevice("__none__", "None");
                    }
                    for (int i = 0; i < (int)m_outputDevices.size(); i++) {
                        ImGui::PushID(i + 1000);
                        if (ImGui::Selectable(m_outputDevices[i].name.c_str(), m_mixerOutputDevice == i)) {
                            m_mixerOutputDevice = i;
                            m_audioMixer.setOutputDevice(m_outputDevices[i].id, m_outputDevices[i].name);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }

            // Master volume
            float master = m_audioMixer.masterVolume() * 100.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##Master", &master, 0.0f, 100.0f, "Master  %.0f%%")) {
                m_audioMixer.setMasterVolume(master / 100.0f);
            }

            // NDI audio output toggle
#ifdef HAS_NDI
            {
                bool ndiAudio = m_audioMixer.isNDIAudioEnabled();
                if (ImGui::Checkbox("Send NDI Audio", &ndiAudio)) {
                    m_audioMixer.setNDIAudioEnabled(ndiAudio);
                }
                if (ndiAudio) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Easel Audio)");
                }
            }
#endif

            ImGui::Separator();

            // Per-input channel strips
            int numInputs = m_audioMixer.inputCount();
            for (int i = 0; i < numInputs; i++) {
                ImGui::PushID(i + 2000);
                std::string iname = m_audioMixer.inputName(i);
                if (iname.empty()) iname = "Input " + std::to_string(i);

                // Mute checkbox
                bool muted = m_audioMixer.isInputMuted(i);
                if (ImGui::Checkbox("##mute", &muted)) {
                    m_audioMixer.setInputMuted(i, muted);
                }
                ImGui::SameLine();

                // Volume slider
                float vol = m_audioMixer.inputVolume(i) * 100.0f;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30);
                if (ImGui::SliderFloat(("##vol" + std::to_string(i)).c_str(), &vol, 0.0f, 100.0f, (iname + "  %.0f%%").c_str())) {
                    m_audioMixer.setInputVolume(i, vol / 100.0f);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    m_audioMixer.removeInput(i);
                }
                ImGui::PopID();
            }

            // Add input
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##AddInput", "+ Add Input")) {
                for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                    ImGui::PushID(i + 3000);
                    if (ImGui::Selectable(m_audioDevices[i].name.c_str())) {
                        m_audioMixer.addInput(m_audioDevices[i].id,
                                              m_audioDevices[i].name,
                                              m_audioDevices[i].isCapture);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextWrapped("Enable the mixer to blend multiple audio inputs and route to an output device.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    }  // end Audio Mixer visibility guard

    renderTransportBar();
#endif

    // Scenes panel — save / recall presets. Moved out of Properties so
    // Properties only shows selected-layer content.
    if (m_ui.isPanelVisible("Scenes")) {
        ImGui::Begin("Scenes");
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 1.00f, 1.00f, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 1.00f, 1.00f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
            if (ImGui::Button("Save Scene", ImVec2(-1, 0))) {
                char nm[32];
                snprintf(nm, sizeof(nm), "Scene %d", m_sceneManager.count() + 1);
                m_sceneManager.saveScene(nm, m_layerStack);
            }
            ImGui::PopStyleColor(4);

            int removeIdx = -1;
            for (int s = 0; s < m_sceneManager.count(); s++) {
                ImGui::PushID(30000 + s);
                auto& scene = m_sceneManager[s];
                float w = ImGui::GetContentRegionAvail().x - 24;
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.06f, 0.07f, 0.10f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 1.00f, 1.00f, 0.35f));
                if (ImGui::Button(scene.name.c_str(), ImVec2(w, 0))) {
                    m_sceneManager.recallScene(s, m_layerStack);
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
                if (ImGui::SmallButton("x")) removeIdx = s;
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (removeIdx >= 0) m_sceneManager.removeScene(removeIdx);
        }
        ImGui::End();
    }
}

#ifdef HAS_FFMPEG
#ifdef _WIN32
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

void Application::cleanupAudioMeter() {
#ifdef _WIN32
    if (m_audioMeterInfo) {
        ((IAudioMeterInformation*)m_audioMeterInfo)->Release();
        m_audioMeterInfo = nullptr;
    }
    if (m_audioMeterDevice) {
        ((IMMDevice*)m_audioMeterDevice)->Release();
        m_audioMeterDevice = nullptr;
    }
#else
    m_audioMeterInfo = nullptr;
    m_audioMeterDevice = nullptr;
#endif
    m_meterDeviceIdx = -2;
}

void Application::updateAudioMeter() {
#ifdef _WIN32
    // Reinit meter if selected device changed or previous init failed
    if (m_meterDeviceIdx != m_selectedAudioDevice || !m_audioMeterInfo) {
        cleanupAudioMeter();
        m_meterDeviceIdx = m_selectedAudioDevice;

        // Ensure COM is initialized on this thread
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       (void**)&enumerator);
        if (SUCCEEDED(hr) && enumerator) {
            IMMDevice* device = nullptr;
            if (m_selectedAudioDevice == -1) {
                // Default output (for loopback metering)
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            } else if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
                auto& dev = m_audioDevices[m_selectedAudioDevice];
                if (dev.isCapture) {
                    std::wstring wid(dev.id.begin(), dev.id.end());
                    hr = enumerator->GetDevice(wid.c_str(), &device);
                } else {
                    std::wstring wid(dev.id.begin(), dev.id.end());
                    hr = enumerator->GetDevice(wid.c_str(), &device);
                }
            }
            if (SUCCEEDED(hr) && device) {
                IAudioMeterInformation* meter = nullptr;
                hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
                if (SUCCEEDED(hr) && meter) {
                    m_audioMeterInfo = meter;
                    m_audioMeterDevice = device;
                } else {
                    std::cerr << "[AudioMeter] IAudioMeterInformation activation failed (hr=0x"
                              << std::hex << hr << std::dec << ")" << std::endl;
                    device->Release();
                }
            } else {
                std::cerr << "[AudioMeter] Device not found (hr=0x"
                          << std::hex << hr << std::dec << " dev=" << m_selectedAudioDevice << ")" << std::endl;
            }
            enumerator->Release();
        } else {
            std::cerr << "[AudioMeter] CoCreateInstance failed (hr=0x"
                      << std::hex << hr << std::dec << ")" << std::endl;
        }
    }

    // Poll levels from IAudioMeterInformation, or fall back to AudioAnalyzer RMS
    if (m_audioMeterInfo) {
        IAudioMeterInformation* meter = (IAudioMeterInformation*)m_audioMeterInfo;
        float peak = 0.0f;
        HRESULT hr = meter->GetPeakValue(&peak);
        if (SUCCEEDED(hr)) {
            m_audioLevelPeak = peak;
        } else {
            // Meter became invalid (device disconnected?) — force reinit next frame
            cleanupAudioMeter();
        }

        UINT32 channelCount = 0;
        if (m_audioMeterInfo) { // re-check after potential cleanup
            meter->GetMeteringChannelCount(&channelCount);
            if (channelCount >= 2) {
                float peaks[8] = {};
                if (SUCCEEDED(meter->GetChannelsPeakValues(channelCount > 8 ? 8 : channelCount, peaks))) {
                    m_audioLevelL = peaks[0];
                    m_audioLevelR = peaks[1];
                }
            } else if (channelCount == 1) {
                float peaks[1] = {};
                if (SUCCEEDED(meter->GetChannelsPeakValues(1, peaks))) {
                    m_audioLevelL = m_audioLevelR = peaks[0];
                }
            }
        }
    } else
#endif
    {
        // Fallback: use AudioAnalyzer RMS if meter unavailable
        float rms = m_audioAnalyzer.smoothedRMS();
        m_audioLevelPeak = rms;
        m_audioLevelL = m_audioLevelR = rms;
    }

    // Smooth (fast attack, slow release)
    float dt = ImGui::GetIO().DeltaTime;
    float attack = 1.0f - expf(-dt * 30.0f);
    float release = 1.0f - expf(-dt * 6.0f);
    auto smooth = [&](float& s, float target) {
        s = target > s ? s + (target - s) * attack : s + (target - s) * release;
    };
    smooth(m_audioLevelSmooth, m_audioLevelPeak);
    smooth(m_audioLevelSmoothL, m_audioLevelL);
    smooth(m_audioLevelSmoothR, m_audioLevelR);


}

// Old cleanupMosaicMeter/updateMosaicMeter removed — replaced by AudioAnalyzer

void Application::renderTransportBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float barH = 56.0f;
    ImVec2 barPos(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - barH);
    ImVec2 barSize(vp->WorkSize.x, barH);

    // Color palette: cyan accent + neutral gray
    const ImU32 kAccent     = IM_COL32(255, 255, 255, 255);
    const ImU32 kAccentDim  = IM_COL32(255, 255, 255, 100);
    const ImU32 kAccentBg   = IM_COL32(255, 255, 255, 15);
    const ImU32 kAccentHov  = IM_COL32(255, 255, 255, 30);
    const ImU32 kText       = IM_COL32(200, 210, 225, 255);
    const ImU32 kTextDim    = IM_COL32(100, 115, 140, 180);
    const ImU32 kDivider    = IM_COL32(255, 255, 255, 15);
    const ImU32 kRed        = IM_COL32(255, 70, 70, 255);
    const ImU32 kRedDim     = IM_COL32(255, 70, 70, 100);

    ImGui::SetNextWindowPos(barPos);
    ImGui::SetNextWindowSize(barSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.028f, 0.032f, 0.045f, 1.0f));

    ImGui::Begin("##TransportBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto& zone = activeZone();
    float time = (float)ImGui::GetTime();
    updateAudioMeter();

    // Top border line
    draw->AddLine(ImVec2(barPos.x, barPos.y), ImVec2(barPos.x + barSize.x, barPos.y), kDivider);

    float btnH = 32.0f;
    float btnR = 5.0f;
    float cy = barPos.y + (barH - btnH) * 0.5f;

    // Helper: draw a transport button
    auto transportBtn = [&](const char* id, const char* label, float x, float w,
                            ImU32 borderCol, ImU32 textCol, bool enabled = true) -> bool {
        ImGui::SetCursorScreenPos(ImVec2(x, cy));
        if (!enabled) ImGui::BeginDisabled();
        ImGui::InvisibleButton(id, ImVec2(w, btnH));
        bool hov = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
        if (!enabled) ImGui::EndDisabled();
        ImVec2 mn(x, cy), mx(x + w, cy + btnH);
        draw->AddRectFilled(mn, mx, hov ? kAccentHov : kAccentBg, btnR);
        draw->AddRect(mn, mx, borderCol, btnR, 0, 1.0f);
        ImVec2 ts = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(x + (w - ts.x) * 0.5f, cy + (btnH - ts.y) * 0.5f), textCol, label);
        return clicked && enabled;
    };

    // Helper: vertical divider
    auto divider = [&](float x) {
        draw->AddLine(ImVec2(x, barPos.y + 12), ImVec2(x, barPos.y + barH - 12), kDivider);
    };

    float curX = barPos.x + 16;

    // ── REC ──
    if (!m_recorder.isActive()) {
        if (transportBtn("##Rec", "REC", curX, 64, kRedDim, kRed)) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
#ifdef _WIN32
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            char fname[128];
            strftime(fname, sizeof(fname), "recordings/%Y%m%d_%H%M%S.mp4", &tm_buf);
            m_recorder.setAudioDevice(m_selectedAudioDevice);
            m_recorder.start(fname, zone.warpFBO.width(), zone.warpFBO.height(), 30);
        }
        // Rec dot
        draw->AddCircleFilled(ImVec2(curX + 14, cy + btnH * 0.5f), 4.0f, kRed);
    } else {
        // Recording active: show timer + stop
        float pulse = 0.5f + 0.5f * sinf(time * 4.0f);
        draw->AddCircleFilled(ImVec2(curX + 8, cy + btnH * 0.5f), 5.0f, IM_COL32(255, 70, 70, (int)(pulse * 255)));
        int secs = (int)m_recorder.uptimeSeconds();
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
        draw->AddText(ImVec2(curX + 18, cy + (btnH - ImGui::GetTextLineHeight()) * 0.5f), kText, timeBuf);
        if (transportBtn("##StopRec", "STOP", curX + 90, 50, kRedDim, kRed)) {
            m_recorder.stop();
        }
    }
    curX += 80;

    divider(curX); curX += 12;

    // ── AUDIO METER (compact) ──
    {
        float meterW = 100.0f, meterH = 14.0f;
        float meterY = cy + (btnH - meterH) * 0.5f;
        float gap = 2.0f, singleH = (meterH - gap) * 0.5f;

        // Refresh device list periodically (every 3 seconds)
        {
            static double lastEnum = 0;
            double now = glfwGetTime();
            if (m_audioDevices.empty() || now - lastEnum > 3.0) {
                lastEnum = now;
                m_audioDevices = VideoRecorder::enumerateAudioDevices();
                // Build output-only device list for mixer
                m_outputDevices.clear();
                for (auto& d : m_audioDevices) {
                    if (!d.isCapture) m_outputDevices.push_back(d);
                }
            }
        }

        // Audio source dropdown (compact)
        ImGui::SetCursorScreenPos(ImVec2(curX, cy + 2));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.06f, 0.08f, 0.9f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        std::string audioPreview = m_mixerEnabled ? "Mixer" : "System Audio";
        if (!m_mixerEnabled && m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
            audioPreview = m_audioDevices[m_selectedAudioDevice].name;
            if (audioPreview.length() > 20) audioPreview = audioPreview.substr(0, 17) + "...";
        }
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("##AudioSrc", audioPreview.c_str())) {
            if (ImGui::Selectable("System Audio", !m_mixerEnabled && m_selectedAudioDevice == -1)) {
                if (m_mixerEnabled) { m_audioMixer.stop(); m_audioAnalyzer.setExternalFeed(false); m_mixerEnabled = false; }
                m_selectedAudioDevice = -1;
            }
            for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(m_audioDevices[i].name.c_str(), !m_mixerEnabled && m_selectedAudioDevice == i)) {
                    if (m_mixerEnabled) { m_audioMixer.stop(); m_audioAnalyzer.setExternalFeed(false); m_mixerEnabled = false; }
                    m_selectedAudioDevice = i;
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Mixer", m_mixerEnabled)) {
                if (!m_mixerEnabled) {
                    m_mixerEnabled = true;
                    m_audioAnalyzer.setExternalFeed(true);
                    if (m_audioMixer.inputCount() == 0)
                        m_audioMixer.addInput("", "System Audio", false);
                    m_audioMixer.start();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        curX += 164;

        // Stereo meter bars (cyan only)
        ImU32 trackBg = IM_COL32(20, 24, 35, 200);
        draw->AddRectFilled(ImVec2(curX, meterY), ImVec2(curX + meterW, meterY + singleH), trackBg, 2.0f);
        draw->AddRectFilled(ImVec2(curX, meterY + singleH + gap), ImVec2(curX + meterW, meterY + meterH), trackBg, 2.0f);

        float fillL = m_audioLevelSmoothL * meterW;
        float fillR = m_audioLevelSmoothR * meterW;
        draw->AddRectFilled(ImVec2(curX, meterY), ImVec2(curX + fillL, meterY + singleH), kAccentDim, 2.0f);
        draw->AddRectFilled(ImVec2(curX, meterY + singleH + gap), ImVec2(curX + fillR, meterY + meterH), kAccentDim, 2.0f);

        curX += meterW + 12;
    }

    divider(curX); curX += 12;

    // ── GO LIVE ──
    static const int aspectNums[] = { 16, 4, 16, 0 };
    static const int aspectDens[] = { 9,  3, 10, 0 };

    if (!m_rtmpOutput.isActive()) {
        bool hasKey = m_streamKeyBuf[0] != '\0';
        if (transportBtn("##Live", "GO LIVE", curX, 80, kAccentDim, hasKey ? kAccent : kTextDim, hasKey)) {
            m_rtmpOutput.start(m_streamKeyBuf, zone.warpFBO.width(), zone.warpFBO.height(),
                               aspectNums[m_streamAspect], aspectDens[m_streamAspect], 30);
        }
        if (!hasKey && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Set stream key in the Stream tab");
    } else {
        float pulse = 0.5f + 0.5f * sinf(time * 3.0f);
        draw->AddCircleFilled(ImVec2(curX + 8, cy + btnH * 0.5f), 5.0f, IM_COL32(255, 255, 255, (int)(pulse * 255)));
        int secs = (int)m_rtmpOutput.uptimeSeconds();
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "LIVE %02d:%02d", (secs / 60) % 60, secs % 60);
        draw->AddText(ImVec2(curX + 18, cy + (btnH - ImGui::GetTextLineHeight()) * 0.5f), kAccent, timeBuf);
        if (transportBtn("##EndLive", "END", curX + 110, 44, kRedDim, kRed)) {
            m_rtmpOutput.stop();
        }
    }
    curX += 96;

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}
#endif

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
            if (ImGui::MenuItem("New Project")) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                while (m_layerStack.count() > 0) {
                    m_layerStack.removeLayer(m_layerStack.count() - 1);
                }
                m_selectedLayer = -1;
                // Reset mappings, masks, and zones to defaults
                m_mappings.clear();
                auto mp = std::make_unique<MappingProfile>();
                mp->init();
                m_mappings.push_back(std::move(mp));
                m_zones.clear();
                auto zone = std::make_unique<OutputZone>();
                zone->mappingIndex = 0;
                zone->init();
                m_zones.push_back(std::move(zone));
                m_activeZone = 0;
                m_viewportPanel.resetZoom();
            }
            ImGui::Separator();
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
                    m_ndiSources = m_ndiFinder.sources();
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
#ifdef HAS_WHEP
            if (ImGui::MenuItem("Add Scope Stream (WHEP)")) {
                std::string url = WHEPSource::discoverUrl(
                    m_ethereaClient.isConnected() ? "http://localhost:7860" : "http://localhost:7860");
                addWHEPSource(url);
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
            ImGui::Separator();
            if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
                if (m_layerStack[m_selectedLayer]->groupId == 0) {
                    if (ImGui::MenuItem("Create Group", "Ctrl+G")) {
                        uint32_t gid = m_layerStack.createGroup("Group");
                        m_layerStack[m_selectedLayer]->groupId = gid;
                    }
                } else {
                    if (ImGui::MenuItem("Ungroup")) {
                        m_layerStack.removeGroup(m_layerStack[m_selectedLayer]->groupId);
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Zone")) {
            if (ImGui::MenuItem("Add Zone")) {
                addZone();
            }
            if (ImGui::MenuItem("Duplicate Active Zone")) {
                duplicateZone(m_activeZone);
                m_activeZone = (int)m_zones.size() - 1;
            }
            if (m_zones.size() > 1 && ImGui::MenuItem("Remove Active Zone")) {
                removeZone(m_activeZone);
            }
            ImGui::Separator();
            for (int i = 0; i < (int)m_zones.size(); i++) {
                bool selected = (i == m_activeZone);
                if (ImGui::MenuItem(m_zones[i]->name.c_str(), nullptr, selected)) {
                    m_activeZone = i;
                }
            }
            ImGui::EndMenu();
        }

        // Fullscreen toggle in menu bar
        ImGui::Separator();
        if (ImGui::MenuItem(m_editorFullscreen ? "Exit Fullscreen" : "Fullscreen", "F11")) {
            if (!m_editorFullscreen) {
                glfwGetWindowPos(m_window, &m_savedWindowX, &m_savedWindowY);
                glfwGetWindowSize(m_window, &m_savedWindowW, &m_savedWindowH);
                int monCount = 0;
                GLFWmonitor** monitors = glfwGetMonitors(&monCount);
                GLFWmonitor* best = glfwGetPrimaryMonitor();
                int wx, wy;
                glfwGetWindowPos(m_window, &wx, &wy);
                for (int mi = 0; mi < monCount; mi++) {
                    int mx, my;
                    glfwGetMonitorPos(monitors[mi], &mx, &my);
                    const GLFWvidmode* mode = glfwGetVideoMode(monitors[mi]);
                    if (wx >= mx && wx < mx + mode->width &&
                        wy >= my && wy < my + mode->height) {
                        best = monitors[mi];
                        break;
                    }
                }
                const GLFWvidmode* mode = glfwGetVideoMode(best);
                glfwSetWindowMonitor(m_window, best, 0, 0,
                                     mode->width, mode->height, mode->refreshRate);
                m_editorFullscreen = true;
            } else {
                glfwSetWindowMonitor(m_window, nullptr,
                                     m_savedWindowX, m_savedWindowY,
                                     m_savedWindowW, m_savedWindowH, 0);
                m_editorFullscreen = false;
            }
        }

        ImGui::EndMainMenuBar();
    }
}

void Application::registerLayerWithZones(uint32_t layerId) {
    for (auto& z : m_zones) {
        if (!z->showAllLayers) {
            z->visibleLayerIds.insert(layerId);
        }
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
    layer->id = m_nextLayerId++;
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
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
    layer->id = m_nextLayerId++;
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
    source->play();
#else
    std::cerr << "Video support not available (FFmpeg not found)" << std::endl;
#endif
}

#if defined(_WIN32) || defined(__APPLE__)
void Application::addScreenCapture(int monitorIndex) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<CaptureSource>();
    if (!source->start(monitorIndex)) {
        std::cerr << "Failed to start screen capture" << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "Screen Capture " + std::to_string(monitorIndex);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}
#endif

#ifdef _WIN32
void Application::addWindowCapture(HWND hwnd, const std::string& title) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<WindowCaptureSource>();
    if (!source->start(hwnd)) {
        std::cerr << "Failed to capture window: " << title << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "Win: " + title;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}
#elif defined(__APPLE__)
void Application::addWindowCapture(uint32_t windowID, const std::string& title) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<WindowCaptureSource>();
    if (!source->start(windowID)) {
        std::cerr << "Failed to capture window: " << title << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "Win: " + title;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}
#endif

void Application::loadShader(const std::string& path) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<ShaderSource>();
    if (!source->loadFromFile(path)) {
        std::cerr << "Failed to load shader: " << path << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);

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
    layer->id = m_nextLayerId++;
    layer->source = source;
    // Extract just the sender name part (after "MACHINE/")
    size_t slash = senderName.find('/');
    layer->name = "NDI: " + ((slash != std::string::npos) ? senderName.substr(slash + 1) : senderName);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}
#endif

#ifdef HAS_WHEP
void Application::addWHEPSource(const std::string& whepUrl) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<WHEPSource>();
    m_whepConnecting = source;
    m_whepStatus = "signaling";

    if (!source->connect(whepUrl)) {
        m_whepStatus = "signaling failed";
        std::cerr << "[WHEP] Connection failed for: " << whepUrl << std::endl;
        return;
    }

    m_whepStatus = "ICE connecting";

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "WHEP: " + whepUrl.substr(whepUrl.rfind('/') + 1);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

void Application::addScopeRTMP() {
    // Query Etherea status API for RTMP URL
    std::string statusJson = httpRequest("GET", "http://localhost:7860/api/scope/status", "", "");
    if (statusJson.empty()) {
        std::cerr << "[Scope] Failed to query Etherea status API\n";
        return;
    }

    // Extract rtmp_url from JSON
    std::string rtmpUrl;
    size_t pos = statusJson.find("\"rtmp_url\"");
    if (pos != std::string::npos) {
        size_t valStart = statusJson.find('"', pos + 10);
        if (valStart != std::string::npos) {
            size_t valEnd = statusJson.find('"', valStart + 1);
            if (valEnd != std::string::npos) {
                rtmpUrl = statusJson.substr(valStart + 1, valEnd - valStart - 1);
            }
        }
    }

    if (rtmpUrl.empty()) {
        std::cerr << "[Scope] No RTMP URL found in status response\n";
        return;
    }

    std::cout << "[Scope] Connecting via RTMP: " << rtmpUrl << std::endl;

    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<VideoSource>();
    if (!source->load(rtmpUrl)) {
        std::cerr << "[Scope] Failed to open RTMP stream: " << rtmpUrl << std::endl;
        return;
    }
    source->play();

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "Scope Stream";

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}
#endif

// --- File Drop ---

void Application::dropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    for (int i = 0; i < count; i++) {
        app->m_pendingDrops.push_back(paths[i]);
    }
}

void Application::handleDroppedFiles() {
    if (m_pendingDrops.empty()) return;

    for (const auto& path : m_pendingDrops) {
        // Determine file type by extension
        std::string lower = path;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);

        size_t dot = lower.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = lower.substr(dot);

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
            loadImage(path);
        } else if (ext == ".mp4" || ext == ".avi" || ext == ".mkv" || ext == ".mov" || ext == ".webm") {
            loadVideo(path);
        } else if (ext == ".fs" || ext == ".frag" || ext == ".glsl") {
            loadShader(path);
        }
    }
    m_pendingDrops.clear();
}

// --- Save/Load ---

void Application::saveProject(const std::string& path) {
    json j;
    j["version"] = 2;

    // Save mapping profiles
    json mappingsJson = json::array();
    for (auto& mPtr : m_mappings) {
        auto& m = *mPtr;
        json mj;
        mj["name"] = m.name;
        mj["warpMode"] = (int)m.warpMode;

        json corners = json::array();
        for (const auto& c : m.cornerPin.corners()) corners.push_back({c.x, c.y});
        mj["cornerPin"] = corners;

        mj["meshWarp"]["cols"] = m.meshWarp.cols();
        mj["meshWarp"]["rows"] = m.meshWarp.rows();
        json meshPoints = json::array();
        for (const auto& p : m.meshWarp.points()) meshPoints.push_back({p.x, p.y});
        mj["meshWarp"]["points"] = meshPoints;

        if (m.objMeshWarp.isLoaded()) {
            json objJson;
            objJson["path"] = m.objMeshWarp.objPath();
            objJson["modelScale"] = m.objMeshWarp.modelScale();
            objJson["modelPosition"] = {m.objMeshWarp.modelPosition().x,
                                         m.objMeshWarp.modelPosition().y,
                                         m.objMeshWarp.modelPosition().z};
            const auto& cam = m.objMeshWarp.camera();
            objJson["camera"]["azimuth"] = cam.azimuth;
            objJson["camera"]["elevation"] = cam.elevation;
            objJson["camera"]["distance"] = cam.distance;
            objJson["camera"]["fovDeg"] = cam.fovDeg;
            objJson["camera"]["target"] = {cam.target.x, cam.target.y, cam.target.z};
            json matsJson = json::array();
            for (const auto& mg : m.objMeshWarp.materials()) {
                json mj2;
                mj2["name"] = mg.name;
                mj2["textured"] = mg.textured;
                matsJson.push_back(mj2);
            }
            objJson["materials"] = matsJson;
            mj["objMesh"] = objJson;
        }

        if (m.edgeBlendLeft > 0 || m.edgeBlendRight > 0 || m.edgeBlendTop > 0 || m.edgeBlendBottom > 0) {
            mj["edgeBlendLeft"] = m.edgeBlendLeft;
            mj["edgeBlendRight"] = m.edgeBlendRight;
            mj["edgeBlendTop"] = m.edgeBlendTop;
            mj["edgeBlendBottom"] = m.edgeBlendBottom;
            mj["edgeBlendGamma"] = m.edgeBlendGamma;
        }

        // Canvas-level masks
        if (!m.masks.empty()) {
            json masksArr = json::array();
            for (auto& mask : m.masks) {
                json mkj;
                mkj["name"] = mask.name;
                mkj["closed"] = mask.path.closed();
                mkj["feather"] = mask.feather;
                mkj["invert"] = mask.invert;
                json pts = json::array();
                for (const auto& pt : mask.path.points()) {
                    json pj;
                    pj["pos"] = {pt.position.x, pt.position.y};
                    pj["in"] = {pt.handleIn.x, pt.handleIn.y};
                    pj["out"] = {pt.handleOut.x, pt.handleOut.y};
                    pj["smooth"] = pt.smooth;
                    pts.push_back(pj);
                }
                mkj["points"] = pts;
                masksArr.push_back(mkj);
            }
            mj["masks"] = masksArr;
        }

        mappingsJson.push_back(mj);
    }
    j["mappings"] = mappingsJson;

    // Save zones
    json zonesJson = json::array();
    for (auto& zonePtr : m_zones) {
        auto& z = *zonePtr;
        json zj;
        zj["name"] = z.name;
        zj["width"] = z.width;
        zj["height"] = z.height;
        zj["mappingIndex"] = z.mappingIndex;
        zj["showAllLayers"] = z.showAllLayers;

        json visIds = json::array();
        for (uint32_t id : z.visibleLayerIds) visIds.push_back(id);
        zj["visibleLayerIds"] = visIds;

        zj["outputDest"] = (int)z.outputDest;
        zj["outputMonitor"] = z.outputMonitor;
        zj["ndiStreamName"] = z.ndiStreamName;

        zonesJson.push_back(zj);
    }
    j["zones"] = zonesJson;

    // Save layers
    json layers = json::array();
    for (int i = 0; i < m_layerStack.count(); i++) {
        const auto& layer = m_layerStack[i];
        json layerJson;
        layerJson["id"] = layer->id;
        layerJson["name"] = layer->name;
        layerJson["visible"] = layer->visible;
        layerJson["opacity"] = layer->opacity;
        layerJson["blendMode"] = (int)layer->blendMode;
        layerJson["position"] = {layer->position.x, layer->position.y};
        layerJson["scale"] = {layer->scale.x, layer->scale.y};
        layerJson["rotation"] = layer->rotation;
        layerJson["flipH"] = layer->flipH;
        layerJson["flipV"] = layer->flipV;
        layerJson["mosaicMode"] = (int)layer->mosaicMode;
        layerJson["tileX"] = layer->tileX;
        layerJson["tileY"] = layer->tileY;
        layerJson["mosaicDensity"] = layer->mosaicDensity;
        layerJson["mosaicSpin"] = layer->mosaicSpin;
        layerJson["audioReactive"] = layer->audioReactive;
        layerJson["audioStrength"] = layer->audioStrength;
        layerJson["feather"] = layer->feather;
        if (layer->dropShadowEnabled) {
            json ds;
            ds["enabled"] = true;
            ds["offsetX"] = layer->dropShadowOffsetX;
            ds["offsetY"] = layer->dropShadowOffsetY;
            ds["blur"] = layer->dropShadowBlur;
            ds["opacity"] = layer->dropShadowOpacity;
            ds["spread"] = layer->dropShadowSpread;
            ds["colorR"] = layer->dropShadowColorR;
            ds["colorG"] = layer->dropShadowColorG;
            ds["colorB"] = layer->dropShadowColorB;
            layerJson["dropShadow"] = ds;
        }
        if (layer->shaderWidth > 0 && layer->shaderHeight > 0) {
            layerJson["shaderWidth"] = layer->shaderWidth;
            layerJson["shaderHeight"] = layer->shaderHeight;
        }
        if (layer->groupId != 0) layerJson["groupId"] = layer->groupId;
#ifdef HAS_NDI
        layerJson["ndiEnabled"] = layer->ndiEnabled;
#endif

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

                // Save audio bindings
                const auto& audioBinds = shaderSrc->audioBindings();
                if (!audioBinds.empty()) {
                    json abJson = json::array();
                    for (const auto& [name, ab] : audioBinds) {
                        if (ab.signal == AudioSignal::None) continue;
                        json abj;
                        abj["param"] = name;
                        abj["signal"] = (int)ab.signal;
                        abj["rangeMin"] = ab.rangeMin;
                        abj["rangeMax"] = ab.rangeMax;
                        abj["smoothing"] = ab.smoothing;
                        if (ab.signal == AudioSignal::MidiCC) {
                            abj["midiCC"] = ab.midiCC;
                            abj["midiChannel"] = ab.midiChannel;
                        }
                        abJson.push_back(abj);
                    }
                    if (!abJson.empty()) {
                        layerJson["audioBindings"] = abJson;
                    }
                }

                // Save image input bindings (which layer provides texture)
                const auto& bindings = shaderSrc->imageBindings();
                if (!bindings.empty()) {
                    json bindingsJson = json::object();
                    for (const auto& [name, binding] : bindings) {
                        if (binding.sourceLayerId != 0) {
                            bindingsJson[name] = binding.sourceLayerId;
                        }
                    }
                    if (!bindingsJson.empty()) {
                        layerJson["imageBindings"] = bindingsJson;
                    }
                }
            }
        }

        // Save per-layer masks
        if (!layer->masks.empty()) {
            json masksJson = json::array();
            for (auto& mask : layer->masks) {
                json mkj;
                mkj["name"] = mask.name;
                mkj["closed"] = mask.path.closed();
                mkj["feather"] = mask.feather;
                mkj["invert"] = mask.invert;
                json pts = json::array();
                for (const auto& pt : mask.path.points()) {
                    json pj;
                    pj["pos"] = {pt.position.x, pt.position.y};
                    pj["in"] = {pt.handleIn.x, pt.handleIn.y};
                    pj["out"] = {pt.handleOut.x, pt.handleOut.y};
                    pj["smooth"] = pt.smooth;
                    pts.push_back(pj);
                }
                mkj["points"] = pts;
                masksJson.push_back(mkj);
            }
            layerJson["masks"] = masksJson;
        }

        layers.push_back(layerJson);
    }
    j["layers"] = layers;

    // Save layer groups
    if (!m_layerStack.groups().empty()) {
        json groupsJson = json::array();
        for (const auto& [gid, grp] : m_layerStack.groups()) {
            json gj;
            gj["id"] = gid;
            gj["name"] = grp.name;
            gj["collapsed"] = grp.collapsed;
            gj["visible"] = grp.visible;
            groupsJson.push_back(gj);
        }
        j["groups"] = groupsJson;
    }

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

    // Helper to load warp state into a mapping profile from a JSON object
    auto loadMappingWarp = [](MappingProfile& m, const json& mj) {
        if (mj.contains("warpMode")) {
            m.warpMode = (ViewportPanel::WarpMode)mj["warpMode"].get<int>();
        }
        if (mj.contains("cornerPin")) {
            auto& corners = m.cornerPin.corners();
            const auto& cj = mj["cornerPin"];
            for (int i = 0; i < 4 && i < (int)cj.size(); i++) {
                corners[i] = {cj[i][0].get<float>(), cj[i][1].get<float>()};
            }
        }
        if (mj.contains("meshWarp")) {
            int cols = mj["meshWarp"]["cols"].get<int>();
            int rows = mj["meshWarp"]["rows"].get<int>();
            m.meshWarp.setGridSize(cols, rows);
            if (mj["meshWarp"].contains("points")) {
                auto& points = m.meshWarp.points();
                const auto& pj = mj["meshWarp"]["points"];
                for (int i = 0; i < (int)pj.size() && i < (int)points.size(); i++) {
                    points[i] = {pj[i][0].get<float>(), pj[i][1].get<float>()};
                }
            }
        }
        if (mj.contains("objMesh")) {
            const auto& oj = mj["objMesh"];
            if (oj.contains("path")) {
                m.objMeshWarp.loadModel(oj["path"].get<std::string>());
            }
            if (oj.contains("modelScale")) {
                m.objMeshWarp.modelScale() = oj["modelScale"].get<float>();
            }
            if (oj.contains("modelPosition")) {
                m.objMeshWarp.modelPosition() = {
                    oj["modelPosition"][0].get<float>(),
                    oj["modelPosition"][1].get<float>(),
                    oj["modelPosition"][2].get<float>()
                };
            }
            if (oj.contains("camera")) {
                auto& cam = m.objMeshWarp.camera();
                const auto& cj = oj["camera"];
                cam.azimuth = cj.value("azimuth", 0.0f);
                cam.elevation = cj.value("elevation", 0.3f);
                cam.distance = cj.value("distance", 3.0f);
                cam.fovDeg = cj.value("fovDeg", 50.0f);
                if (cj.contains("target")) {
                    cam.target = {cj["target"][0].get<float>(),
                                  cj["target"][1].get<float>(),
                                  cj["target"][2].get<float>()};
                }
            }
            if (oj.contains("materials")) {
                auto& mats = m.objMeshWarp.materials();
                const auto& matsJson = oj["materials"];
                for (const auto& matJ : matsJson) {
                    std::string name = matJ.value("name", "");
                    bool tex = matJ.value("textured", true);
                    for (auto& mg : mats) {
                        if (mg.name == name) { mg.textured = tex; break; }
                    }
                }
            }
        }
        m.edgeBlendLeft = mj.value("edgeBlendLeft", 0.0f);
        m.edgeBlendRight = mj.value("edgeBlendRight", 0.0f);
        m.edgeBlendTop = mj.value("edgeBlendTop", 0.0f);
        m.edgeBlendBottom = mj.value("edgeBlendBottom", 0.0f);
        m.edgeBlendGamma = mj.value("edgeBlendGamma", 2.2f);

        // Load masks
        if (mj.contains("masks")) {
            for (const auto& mkj : mj["masks"]) {
                MappingMask mask;
                mask.name = mkj.value("name", "Mask");
                mask.feather = mkj.value("feather", 0.0f);
                mask.invert = mkj.value("invert", false);
                if (mkj.contains("closed")) mask.path.setClosed(mkj["closed"].get<bool>());
                if (mkj.contains("points")) {
                    for (const auto& pj : mkj["points"]) {
                        MaskPoint pt;
                        pt.position = {pj["pos"][0].get<float>(), pj["pos"][1].get<float>()};
                        pt.handleIn = {pj["in"][0].get<float>(), pj["in"][1].get<float>()};
                        pt.handleOut = {pj["out"][0].get<float>(), pj["out"][1].get<float>()};
                        pt.smooth = pj.value("smooth", true);
                        mask.path.points().push_back(pt);
                    }
                    mask.path.markDirty();
                }
                m.masks.push_back(std::move(mask));
            }
        }
    };

    int version = j.value("version", 1);

    // Load mapping profiles
    m_mappings.clear();
    if (j.contains("mappings")) {
        for (const auto& mj : j["mappings"]) {
            auto mp = std::make_unique<MappingProfile>();
            mp->name = mj.value("name", "Default");
            mp->init();
            loadMappingWarp(*mp, mj);
            m_mappings.push_back(std::move(mp));
        }
    }


    if (version >= 2 && j.contains("zones")) {
        // v2 format: multiple zones
        m_zones.clear();

        // Backward compat: if no "mappings" key, extract warp from each zone
        bool legacyWarp = m_mappings.empty();

        for (const auto& zj : j["zones"]) {
            auto z = std::make_unique<OutputZone>();
            z->name = zj.value("name", "Zone");
            z->width = zj.value("width", 1920);
            z->height = zj.value("height", 1080);
            z->showAllLayers = zj.value("showAllLayers", true);
            if (zj.contains("visibleLayerIds")) {
                for (const auto& id : zj["visibleLayerIds"]) {
                    z->visibleLayerIds.insert(id.get<uint32_t>());
                }
            }
            z->init();

            if (legacyWarp) {
                // Old format: warp data is in the zone JSON — migrate to a mapping profile
                auto mp = std::make_unique<MappingProfile>();
                mp->name = z->name;
                mp->init();
                loadMappingWarp(*mp, zj);
                z->mappingIndex = (int)m_mappings.size();
                m_mappings.push_back(std::move(mp));
            } else {
                z->mappingIndex = zj.value("mappingIndex", 0);
            }

            // Recreate FBO with depth if ObjMesh mode
            auto* mp = (z->mappingIndex >= 0 && z->mappingIndex < (int)m_mappings.size())
                ? m_mappings[z->mappingIndex].get() : nullptr;
            if (mp && mp->warpMode == ViewportPanel::WarpMode::ObjMesh) {
                z->warpFBO.create(z->width, z->height, true);
            }

            z->outputDest = (OutputDest)zj.value("outputDest", 0);
            z->outputMonitor = zj.value("outputMonitor", -1);
            z->ndiStreamName = zj.value("ndiStreamName", std::string(""));

            m_zones.push_back(std::move(z));
        }
        if (m_zones.empty()) {
            auto z = std::make_unique<OutputZone>();
            z->init();
            m_zones.push_back(std::move(z));
        }
        m_activeZone = 0;
    } else {
        // v1 format: single zone — load warp into a mapping profile
        if (m_zones.empty()) {
            auto z = std::make_unique<OutputZone>();
            z->init();
            m_zones.push_back(std::move(z));
        }
        while (m_zones.size() > 1) m_zones.pop_back();
        m_activeZone = 0;

        if (m_mappings.empty()) {
            auto mp = std::make_unique<MappingProfile>();
            mp->init();
            loadMappingWarp(*mp, j);
            m_mappings.push_back(std::move(mp));
        }
        m_zones[0]->mappingIndex = 0;
    }

    // Ensure at least one mapping exists
    if (m_mappings.empty()) {
        auto mp = std::make_unique<MappingProfile>();
        mp->init();
        m_mappings.push_back(std::move(mp));
    }

    // Safety fixup: ensure each zone owns its own mapping profile. Old projects
    // (or mis-saved state) sometimes have two zones pointing at the same
    // mappingIndex — that makes canvas masks from one zone appear on the other
    // because they're literally the same MappingProfile. Give any duplicate a
    // fresh empty profile so its masks are independent.
    {
        std::vector<bool> used(m_mappings.size(), false);
        for (auto& zPtr : m_zones) {
            if (!zPtr) continue;
            int mi = zPtr->mappingIndex;
            if (mi < 0 || mi >= (int)m_mappings.size() || used[mi]) {
                auto mp = std::make_unique<MappingProfile>();
                mp->name = zPtr->name;
                mp->init();
                zPtr->mappingIndex = (int)m_mappings.size();
                m_mappings.push_back(std::move(mp));
                used.push_back(true);
            } else {
                used[mi] = true;
            }
        }
    }

    // Load layers
    if (j.contains("layers")) {
        for (const auto& layerJson : j["layers"]) {
            auto layer = std::make_shared<Layer>();
            layer->id = layerJson.value("id", (uint32_t)0);
            layer->name = layerJson.value("name", "Layer");
            layer->visible = layerJson.value("visible", true);
            layer->opacity = layerJson.value("opacity", 1.0f);
            layer->blendMode = (BlendMode)layerJson.value("blendMode", 0);
            layer->rotation = layerJson.value("rotation", 0.0f);
            layer->flipH = layerJson.value("flipH", false);
            layer->flipV = layerJson.value("flipV", false);
            layer->mosaicMode = (MosaicMode)layerJson.value("mosaicMode", 0);
            layer->tileX = layerJson.value("tileX", 1.0f);
            layer->tileY = layerJson.value("tileY", 1.0f);
            layer->mosaicDensity = layerJson.value("mosaicDensity", 4.0f);
            layer->mosaicSpin = layerJson.value("mosaicSpin", 0.0f);
            layer->audioReactive = layerJson.value("audioReactive", false);
            layer->audioStrength = layerJson.value("audioStrength", 0.15f);
            layer->feather = layerJson.value("feather", 0.0f);
            if (layerJson.contains("dropShadow")) {
                const auto& ds = layerJson["dropShadow"];
                layer->dropShadowEnabled = ds.value("enabled", false);
                layer->dropShadowOffsetX = ds.value("offsetX", 0.05f);
                layer->dropShadowOffsetY = ds.value("offsetY", 0.05f);
                layer->dropShadowBlur = ds.value("blur", 8.0f);
                layer->dropShadowOpacity = ds.value("opacity", 0.7f);
                layer->dropShadowSpread = ds.value("spread", 1.0f);
                layer->dropShadowColorR = ds.value("colorR", 0.0f);
                layer->dropShadowColorG = ds.value("colorG", 0.0f);
                layer->dropShadowColorB = ds.value("colorB", 0.0f);
            }
            layer->shaderWidth = layerJson.value("shaderWidth", 0);
            layer->shaderHeight = layerJson.value("shaderHeight", 0);
            layer->groupId = layerJson.value("groupId", (uint32_t)0);
#ifdef HAS_NDI
            layer->ndiEnabled = layerJson.value("ndiEnabled", false);
#endif

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
                    src->play();
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
                    // Restore audio bindings
                    if (layerJson.contains("audioBindings")) {
                        for (const auto& abj : layerJson["audioBindings"]) {
                            AudioBinding ab;
                            ab.signal = (AudioSignal)abj.value("signal", 0);
                            ab.rangeMin = abj.value("rangeMin", 0.0f);
                            ab.rangeMax = abj.value("rangeMax", 1.0f);
                            ab.smoothing = abj.value("smoothing", 0.3f);
                            ab.midiCC = abj.value("midiCC", -1);
                            ab.midiChannel = abj.value("midiChannel", -1);
                            src->audioBindings()[abj.value("param", "")] = ab;
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
#ifdef HAS_WHEP
            } else if (sourceType == "WHEP" && !sourcePath.empty()) {
                auto src = std::make_shared<WHEPSource>();
                if (src->connect(sourcePath)) {
                    layer->source = src;
                }
#endif
            }

            // Load per-layer masks
            if (layerJson.contains("masks")) {
                for (const auto& mkj : layerJson["masks"]) {
                    Layer::LayerMask mask;
                    mask.name = mkj.value("name", "Mask");
                    mask.feather = mkj.value("feather", 0.0f);
                    mask.invert = mkj.value("invert", false);
                    if (mkj.contains("closed")) mask.path.setClosed(mkj["closed"].get<bool>());
                    if (mkj.contains("points")) {
                        for (const auto& pj : mkj["points"]) {
                            MaskPoint pt;
                            pt.position = {pj["pos"][0].get<float>(), pj["pos"][1].get<float>()};
                            pt.handleIn = {pj["in"][0].get<float>(), pj["in"][1].get<float>()};
                            pt.handleOut = {pj["out"][0].get<float>(), pj["out"][1].get<float>()};
                            pt.smooth = pj.value("smooth", true);
                            mask.path.points().push_back(pt);
                        }
                    }
                    mask.path.markDirty();
                    layer->masks.push_back(std::move(mask));
                }
            }

            if (layer->source) {
                m_layerStack.addLayer(layer);
            }
        }
    }

    // Assign stable IDs to layers that don't have one (v1 files)
    uint32_t maxId = 0;
    for (int i = 0; i < m_layerStack.count(); i++) {
        maxId = std::max(maxId, m_layerStack[i]->id);
    }
    m_nextLayerId = maxId + 1;
    for (int i = 0; i < m_layerStack.count(); i++) {
        if (m_layerStack[i]->id == 0) {
            m_layerStack[i]->id = m_nextLayerId++;
        }
    }

    // Restore image input bindings (must happen after all layers are loaded)
    if (j.contains("layers")) {
        int idx = 0;
        for (const auto& layerJson : j["layers"]) {
            if (idx >= m_layerStack.count()) break;
            auto& layer = m_layerStack[idx];
            if (layer->source && layer->source->isShader() && layerJson.contains("imageBindings")) {
                auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
                for (auto& [name, srcIdJson] : layerJson["imageBindings"].items()) {
                    uint32_t srcId = srcIdJson.get<uint32_t>();
                    // Find the source layer and bind its texture
                    for (int j = 0; j < m_layerStack.count(); j++) {
                        auto& srcLayer = m_layerStack[j];
                        if (srcLayer->id == srcId && srcLayer->source) {
                            shaderSrc->bindImageInput(name,
                                srcLayer->source->textureId(),
                                srcLayer->source->width(),
                                srcLayer->source->height(),
                                srcId,
                                srcLayer->source->isFlippedV());
                            break;
                        }
                    }
                }
            }
            idx++;
        }
    }

    // Load layer groups
    if (j.contains("groups")) {
        for (const auto& gj : j["groups"]) {
            uint32_t gid = gj["id"].get<uint32_t>();
            LayerGroup grp;
            grp.name = gj.value("name", "Group");
            grp.collapsed = gj.value("collapsed", false);
            grp.visible = gj.value("visible", true);
            m_layerStack.groups()[gid] = grp;
        }
    }

    if (m_layerStack.count() > 0) {
        m_selectedLayer = 0;
    }

    // Backward compat: migrate old MappingProfile masks to first layer ONLY for
    // legacy v1 files. In v2+ "masks" on a MappingProfile are real canvas masks
    // (a first-class feature) and must NOT be clobbered — that was the bug that
    // made masks disappear on every reload.
    if (version < 2 && m_layerStack.count() > 0) {
        for (auto& mp : m_mappings) {
            if (!mp->masks.empty()) {
                auto& firstLayer = m_layerStack[0];
                for (auto& oldMask : mp->masks) {
                    Layer::LayerMask lm;
                    lm.name = oldMask.name;
                    lm.path = oldMask.path;
                    lm.texture = oldMask.texture;
                    firstLayer->masks.push_back(std::move(lm));
                }
                mp->masks.clear();
                std::cout << "Migrated v1 mapping masks to layer: " << firstLayer->name << std::endl;
            }
        }
    }

    std::cout << "Project loaded: " << path << std::endl;
}

void Application::captureScreenshot(const std::string& path) {
    auto& zone = activeZone();
    int w = zone.warpFBO.width();
    int h = zone.warpFBO.height();
    if (w <= 0 || h <= 0) {
        std::cerr << "Screenshot failed: no framebuffer" << std::endl;
        return;
    }

    std::vector<uint8_t> pixels(w * h * 4);
    glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
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

// Stage tab inline header — intentionally empty. Composition + Output now
// live in the Canvas zone bar (see ViewportPanel::render). The Stage tab's
// 3D viewport needs its full height, so no inline header clutters the top.
void Application::renderStageInlineSetup(OutputZone& zone) {
    (void)zone;
}

// Masks tab: Edge Blend (collapsible).
void Application::renderEdgeBlendInline(OutputZone& zone) {
    if (ImGui::CollapsingHeader("Edge Blend")) {
        auto* ebm = mappingForZone(zone);
        if (ebm) {
            float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBL", &ebm->edgeBlendLeft, 1.0f, 0.0f, (float)zone.width * 0.5f, "L %.0fpx");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBR", &ebm->edgeBlendRight, 1.0f, 0.0f, (float)zone.width * 0.5f, "R %.0fpx");

            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBT", &ebm->edgeBlendTop, 1.0f, 0.0f, (float)zone.height * 0.5f, "T %.0fpx");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBB", &ebm->edgeBlendBottom, 1.0f, 0.0f, (float)zone.height * 0.5f, "B %.0fpx");

            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##EBGamma", &ebm->edgeBlendGamma, 0.05f, 0.5f, 4.0f, "Gamma %.2f");
        }
    }
}
