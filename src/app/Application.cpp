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
#include "render/GLTransition.h"
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_internal.h>     // DockBuilderSetNodeSize for timeline minimize
#include "stb_image.h"
#include "stb_image_write.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <unordered_set>
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

// Implemented in WindowChrome_mac.mm — exposed as C so ObjC++ name mangling
// doesn't interfere with the link from this C++ TU.
extern "C" void EaselMac_UnifyTitleBar(GLFWwindow*);
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

#ifdef __APPLE__
    // Unify the title bar so the ImGui main menu sits alongside the
    // traffic-light buttons (Figma / VS Code style), freeing the row the
    // OS would otherwise reserve for a separate title strip.
    EaselMac_UnifyTitleBar(m_window);
#endif

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

    // Scan bundled gl-transitions shaders. Lazy-compile on first use.
    GLTransitionLibrary::instance().scan("assets/transitions/gl");

    // Also scan the user's drop-in folder at ~/Documents/Easel/transitions/
    // so custom `.glsl` files appear in the timeline transition picker
    // alongside the bundled ones. Created on first launch if missing.
    if (const char* home = std::getenv("HOME")) {
        std::string userTransitionsDir = std::string(home) + "/Documents/Easel/transitions";
        std::error_code ec;
        std::filesystem::create_directories(userTransitionsDir, ec);
        GLTransitionLibrary::instance().scanAdditional(userTransitionsDir);
    }

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
    // Wire the Stage Setup section into the Properties panel — surfaces
    // displays/projectors/surfaces inspector under "Setup" only when
    // sMode == Stage.
    m_propertyPanel.setStageView(&m_stageView);

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

            // Keep timeline tracks in sync with the layer stack every frame,
            // even when the Timeline panel is hidden. Newly-added layers get
            // a default clip spanning their natural duration so the clip-
            // driven visibility logic in applyToLayers works for them from
            // frame one. (Previously this lived inside renderTimelinePanel
            // and didn't run with the panel collapsed.)
            {
                std::unordered_set<uint32_t> liveIds;
                for (int i = 0; i < m_layerStack.count(); i++) {
                    auto l = m_layerStack[i];
                    if (!l || l->id == 0) continue;
                    liveIds.insert(l->id);
                    if (auto* tr = m_timeline.findTrack(l->id)) {
                        tr->name = l->name;
                    } else {
                        m_timeline.ensureTrack(l->id, l->name);
                        double d = (l->source) ? l->source->duration() : 0.0;
                        if (d <= 0.0) d = m_timeline.duration();
                        if (d > m_timeline.duration()) d = m_timeline.duration();
                        m_timeline.addClip(l->id, 0.0, d, l->name);
                    }
                }
                auto& tracks = m_timeline.tracks();
                for (int i = (int)tracks.size() - 1; i >= 0; i--) {
                    if (!liveIds.count(tracks[i].layerId)) {
                        m_timeline.removeTrackForLayer(tracks[i].layerId);
                    }
                }
            }

            // Advance timeline playhead and push clip → layer state before
            // compositing. dt is clamped to avoid the playhead leaping when a
            // modal dialog (Add Layer, Open Project, etc.) blocked the main
            // thread for multiple seconds — `now - lastTime` would otherwise
            // jump the scrubber to the end of the timeline after the dialog.
            float tlDt = dt;
            if (tlDt > 0.1f) tlDt = 1.0f / 60.0f;
            m_timeline.advance(tlDt);
            m_timeline.applyToLayers(m_layerStack);

            // Export flow: auto-stop recorder + pause when playhead crosses the Work Area end.
            if (m_timelineExporting && m_timeline.playhead() >= m_timelineExportEnd - 1e-3) {
                if (m_recorder.isActive()) m_recorder.stop();
                m_timeline.pause();
                m_timelineExporting = false;
            }
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

            // Undo / Redo keybinds — use GLFW for reliable detection.
            // macOS uses Cmd (Super), everything else uses Ctrl; accept both so
            // the same keystroke works across platforms.
            if (!ImGui::GetIO().WantTextInput) {
                static bool sUndoPrev = false, sRedoPrev = false;
                bool ctrl = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                            glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
                            glfwGetKey(m_window, GLFW_KEY_LEFT_SUPER)   == GLFW_PRESS ||
                            glfwGetKey(m_window, GLFW_KEY_RIGHT_SUPER)  == GLFW_PRESS;
                bool shift = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                bool z = glfwGetKey(m_window, GLFW_KEY_Z) == GLFW_PRESS;
                bool y = glfwGetKey(m_window, GLFW_KEY_Y) == GLFW_PRESS;

                bool undoNow = ctrl && z && !shift;
                bool redoNow = ctrl && ((z && shift) || y);

                if (undoNow && !sUndoPrev) m_undoStack.undo(m_layerStack, m_selectedLayer, m_timeline);
                if (redoNow && !sRedoPrev) m_undoStack.redo(m_layerStack, m_selectedLayer, m_timeline);
                sUndoPrev = undoNow;
                sRedoPrev = redoNow;

                // Esc — clear any current selection (layer + mask edit mode)
                // so the inspector empties and side-panels show their default
                // state. Behaves like "click off" without having to find an
                // empty spot in the canvas.
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_selectedLayer = -1;
                    m_maskEditMode = false;
                }

                // Delete / Backspace → remove selected layer. Guarded by
                // WantTextInput so typing in the shader editor or name field
                // doesn't nuke layers out from under you.
                if (!ImGui::GetIO().WantTextInput
                    && m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()
                    && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
                {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer);
                    auto l = m_layerStack[m_selectedLayer];
                    uint32_t rid = l ? l->id : 0;
                    m_layerStack.removeLayer(m_selectedLayer);
                    if (rid) m_timeline.removeTrackForLayer(rid);
                    if (m_selectedLayer >= m_layerStack.count()) {
                        m_selectedLayer = m_layerStack.count() - 1;
                    }
                }
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

            // Audio-reactive particle sources — feed bass/mid/treble +
            // beat-onset pulse to the ParticleSource each frame so the
            // emitter can scale spawn rate, particle size, velocity, and
            // burst on beats. Mirrors the ShaderSource audio plumbing
            // above.
            if (layer->source->typeName() == "Particles") {
                auto* ps = static_cast<ParticleSource*>(layer->source.get());
                float midAvg = (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f;
                ps->setAudioState(
                    m_audioAnalyzer.bass(),
                    midAvg,
                    m_audioAnalyzer.treble(),
                    m_audioAnalyzer.beatDecay()
                );
            }

            layer->source->update();
            if (layer->shaderTransitionActive && layer->nextSource) {
                layer->nextSource->update();
            }

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

    // Legacy bottom transport bar was merged into the Timeline panel; no
    // widgets render in the reserved strip anymore. Dockspace now spans
    // the full window so we don't leak a dead empty black band below the
    // dock node.
    float transportBarH = 0.0f;
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
        m_viewportPanel.setEditorFullscreen(m_editorFullscreen);
        m_viewportPanel.render(previewTex, mappingForZone(z), projAspect,
                               &m_zones, &m_activeZone, &monitors, ndiAvail, editorMon, &m_mappings);
        if (m_viewportPanel.wantsFullscreenToggle()) {
            m_viewportPanel.clearFullscreenSignal();
            toggleEditorFullscreen();
        }
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

    // Clean up orphaned timeline tracks for layers removed during LayerPanel render
    for (uint32_t rid : m_layerPanel.removedLayerIds) {
        m_timeline.removeTrackForLayer(rid);
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

    // Warp editor renders FIRST so the mapping parameters (corner pin / mesh
    // warp / obj mesh) appear at the TOP of the Mapping panel, with masks
    // tucked below as a collapsible dropdown.
    {
        auto* mpEarly = mappingForZone(zone);
        if (mpEarly && m_ui.isPanelVisible("Mapping")) {
            auto prevWarpMode = mpEarly->warpMode;
            m_warpEditor.render(*mpEarly, m_maskEditMode, &m_mappings, zone.mappingIndex);
            if (mpEarly->warpMode != prevWarpMode) {
                bool needsDepth = (mpEarly->warpMode == ViewportPanel::WarpMode::ObjMesh);
                zone.warpFBO.create(zone.width, zone.height, needsDepth);
            }
            if (m_warpEditor.wantsLoadOBJ()) {
                std::string path = openFileDialog("3D Models\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
                if (!path.empty()) {
                    mpEarly->objMeshWarp.loadModel(path);
                }
            }
        }
    }

    // --- Masks section (lives inside the Mapping panel as a collapsible
    // dropdown BELOW the mapping parameters). Closed by default — expand to
    // tweak canvas/layer masks.
    if (m_ui.isPanelVisible("Mapping")) {
    ImGui::Begin("        ###Mapping");
    if (ImGui::CollapsingHeader("Masks"))
    {
        // (Edge Blend moved to the bottom of this panel — you only want to
        //  reach for it AFTER you've shaped a mask, not before.)

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

        // Spacing between Canvas Masks and Layer Masks — no hairline
        // divider, just generous vertical rhythm to separate the groups.
        ImGui::Dummy(ImVec2(0, 16));

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

        // --- Edge Blend (moved to the bottom) ----------------------------
        // Edge blend is a projector-output refinement you reach for after a
        // mask is shaped. No hairline divider — vertical spacing does the
        // separating so the section rhythm stays clean.
        ImGui::Dummy(ImVec2(0, 16));
        renderEdgeBlendInline(zone);

        masks_panel_done:;
    }
    ImGui::End();
    }  // end Mapping panel (merged Masks section)

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
        // Build zoneTextures here so the Stage Setup section inside
        // Properties has access to the current FBO ids each frame.
        static std::vector<unsigned int> s_zoneTexs;
        s_zoneTexs.clear();
        for (auto& zp : m_zones) s_zoneTexs.push_back(zp->warpFBO.textureId());
        m_propertyPanel.setZoneTextures(&s_zoneTexs);
        m_propertyPanel.render(selectedLayer, m_maskEditMode, &m_speechState, &mosaicAudio, (float)glfwGetTime(), &m_layerStack, &m_bpmSync, &m_sceneManager, &m_mosaicAudioDevice, &m_midiManager);
    }

    // If a property widget was just activated, push the pre-edit state (before the widget changed it)
    if (m_propertyPanel.undoNeeded) {
        if (capturedPre) {
            m_undoStack.pushSnapshot(std::move(preEditSnapshot));
        }
        m_propertyPanel.undoNeeded = false;
    }

    // Warp editor now renders earlier in the frame (before the masks block)
    // so mapping parameters sit above the Masks dropdown in the panel.
    auto* mp = mappingForZone(zone);

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

        // Stage panel — layout top→bottom: (1) 3D viewport, (2) Setup
        // collapsible, (3) Scenes collapsible (merged in from the old Scenes
        // tab). Viewport gets the bulk of the panel; Setup and Scenes sit
        // below, each collapsible so the viewport breathes when closed.
        if (m_ui.isPanelVisible("Stage") && UIManager::sMode == UIManager::WorkspaceMode::Stage) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Stage");
            ImGui::PopStyleVar();

            // Shared secondary-nav row (pills + zones + OUTPUT + composition
            // chip + Fullscreen). Using the same helper as Canvas guarantees
            // the element positions don't shift when switching workspaces.
            {
                auto monitors = ProjectorOutput::enumerateMonitors();
                bool ndiAvail = false;
#ifdef HAS_NDI
                ndiAvail = NDIRuntime::instance().isAvailable();
#endif
                int editorMon = -1;
                {
                    int wx, wy;
                    glfwGetWindowPos(m_window, &wx, &wy);
                    for (size_t mi = 0; mi < monitors.size(); mi++) {
                        if (wx >= monitors[mi].x && wx < monitors[mi].x + monitors[mi].width &&
                            wy >= monitors[mi].y && wy < monitors[mi].y + monitors[mi].height) {
                            editorMon = (int)mi;
                            break;
                        }
                    }
                }
                m_viewportPanel.setEditorFullscreen(m_editorFullscreen);
                m_viewportPanel.renderNavBar(true, &m_zones, &m_activeZone,
                                             &monitors, ndiAvail, editorMon);
                if (m_viewportPanel.wantsFullscreenToggle()) {
                    m_viewportPanel.clearFullscreenSignal();
                    toggleEditorFullscreen();
                }
            }

            // (Stage toolbar is now rendered as a separate floating vertical
            // pill on the left edge — see m_stageView.renderToolbarFloating()
            // called after the Stage panel below.)

            float panelH = ImGui::GetContentRegionAvail().y;
            // Reserve space for the two collapsible sections. 40px covers the
            // two section headers when both are closed; open sections scroll
            // inside their own child regions below.
            float reservedH = 80.0f;
            float viewportH = std::max(200.0f, panelH - reservedH);

            // --- 3D viewport (first) ---
            ImGui::BeginChild("##StageViewport", ImVec2(0, viewportH), false);
            m_stageView.renderUI(zoneTextures);
            if (m_stageView.wantsImport()) {
                m_stageView.clearImportSignal();
                std::string path = openFileDialog(
                    "3D Models\0*.obj;*.gltf;*.glb\0All Files\0*.*\0");
                if (!path.empty()) m_stageView.loadModel(path);
            }
            ImGui::EndChild();

            // (Setup + Scenes sections moved to the Properties panel —
            // see PropertyPanel::render in Stage mode.)
            if (false) {
            }

            ImGui::End();
        }

        // (Old separate Scenes panel removed — merged into Stage above.)

        // Floating Stage toolbar — vertical pill of circular icon buttons
        // sitting in the left-rail slot (where Layers lives in Canvas
        // mode). Only rendered when Stage workspace is active.
        if (UIManager::sMode == UIManager::WorkspaceMode::Stage) {
            m_stageView.renderFloatingToolbar();
        }

        // SHOW workspace — live performance focus. Renders the secondary
        // nav (so the top bar doesn't disappear when switching modes) and
        // a centered live-output preview so the operator can see what's
        // currently going to the projector while they drive Timeline +
        // MIDI + Audio from the right rail.
        if (m_ui.isPanelVisible("Show") && UIManager::sMode == UIManager::WorkspaceMode::Show) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Show");
            ImGui::PopStyleVar();

            // Same shared nav so geometry doesn't shift between modes.
            {
                auto monitors = ProjectorOutput::enumerateMonitors();
                bool ndiAvail = false;
#ifdef HAS_NDI
                ndiAvail = NDIRuntime::instance().isAvailable();
#endif
                int editorMon = -1;
                {
                    int wx, wy;
                    glfwGetWindowPos(m_window, &wx, &wy);
                    for (size_t mi = 0; mi < monitors.size(); mi++) {
                        if (wx >= monitors[mi].x && wx < monitors[mi].x + monitors[mi].width &&
                            wy >= monitors[mi].y && wy < monitors[mi].y + monitors[mi].height) {
                            editorMon = (int)mi;
                            break;
                        }
                    }
                }
                m_viewportPanel.setEditorFullscreen(m_editorFullscreen);
                m_viewportPanel.renderNavBar(false, &m_zones, &m_activeZone,
                                             &monitors, ndiAvail, editorMon);
                if (m_viewportPanel.wantsFullscreenToggle()) {
                    m_viewportPanel.clearFullscreenSignal();
                    toggleEditorFullscreen();
                }
            }

            // Live output preview — shows the active zone's composited
            // output so the performer can see what the audience sees.
            ImGui::Dummy(ImVec2(0, 8));
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float aspect = 16.0f / 9.0f;
            if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size()) {
                auto& z = m_zones[m_activeZone];
                if (z->width > 0 && z->height > 0)
                    aspect = (float)z->width / (float)z->height;
            }
            float previewH = std::min(avail.y - 16.0f, avail.x / aspect);
            float previewW = previewH * aspect;
            float padX = (avail.x - previewW) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
            GLuint previewTex = 0;
            if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size())
                previewTex = m_zones[m_activeZone]->warpFBO.textureId();
            if (previewTex) {
                ImGui::Image((ImTextureID)(intptr_t)previewTex,
                             ImVec2(previewW, previewH),
                             ImVec2(0, 1), ImVec2(1, 0));
            } else {
                ImGui::Dummy(ImVec2(previewW, previewH));
            }

            ImGui::End();
        }
    }

    // Projector settings are now rendered inline inside the Canvas tab
    // via renderCompositionInlinePanel(). The standalone "Projector" window is gone.

    // Sources panel — single window with tabs for each input source type.
    // Groups what used to be 5 separate panels (NDI / Spout / Capture /
    // ShaderClaw / Etherea) into one place so the right column isn't
    // overwhelmed with tabs. Hidden in Stage and Show modes via the
    // mode-aware UIManager::isPanelVisible.
    bool sourcesVisible = m_ui.isPanelVisible("Sources");
    bool sourcesOpen = sourcesVisible && ImGui::Begin("Sources");
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
            // Connected — show shader browser. Color language: green means
            // "connected" (a healthy state, not an alert). Disconnect is a
            // calm muted pill — no red, since disconnecting is a normal user
            // action, not an error. Refresh lives as a small icon right next
            // to Disconnect to keep the row compact.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.78f, 0.52f, 1.0f));
            ImGui::Text("Connected");
            ImGui::PopStyleColor();
            // (Shader count removed — the tile gallery below makes the number
            //  obvious from scanning; a bare integer added noise.)

            // Right-aligned cluster: [⟳] Disconnect. Refresh icon sizes to
            // the actual text-line height so it matches SmallButton's vertical
            // footprint — previously `GetFrameHeight()` made it ~2x too tall.
            const float kDiscW = ImGui::CalcTextSize("Disconnect").x
                               + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f;
            const float kIconH = ImGui::GetTextLineHeight();
            float rightClusterW = kIconH + 6.0f + kDiscW;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - rightClusterW);

            // Refresh — small circular-arrow glyph, sized to match text row.
            {
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##SCRefresh", ImVec2(kIconH, kIconH));
                bool hov = ImGui::IsItemHovered();
                ImDrawList* d = ImGui::GetWindowDrawList();
                ImU32 bg = hov ? IM_COL32(255, 255, 255, 28)
                               : IM_COL32(255, 255, 255, 10);
                d->AddRectFilled(p0, ImVec2(p0.x + kIconH, p0.y + kIconH), bg, 4.0f);

                float cx = p0.x + kIconH * 0.5f;
                float cy = p0.y + kIconH * 0.5f;
                float r  = kIconH * 0.30f;
                ImU32 fg = IM_COL32(235, 240, 250, 230);
                const int seg = 20;
                for (int s = 0; s < seg; s++) {
                    float a0 = -0.35f * 3.14159f + (s    / (float)seg) * 5.2f;
                    float a1 = -0.35f * 3.14159f + ((s+1)/ (float)seg) * 5.2f;
                    d->AddLine(ImVec2(cx + cosf(a0)*r, cy + sinf(a0)*r),
                               ImVec2(cx + cosf(a1)*r, cy + sinf(a1)*r),
                               fg, 1.2f);
                }
                float ae = -0.35f * 3.14159f + 5.2f;
                float ax = cx + cosf(ae) * r;
                float ay = cy + sinf(ae) * r;
                d->AddTriangleFilled(ImVec2(ax - 2.5f, ay - 2.5f),
                                     ImVec2(ax + 2.5f, ay - 2.5f),
                                     ImVec2(ax,        ay + 2.5f), fg);
                if (hov) ImGui::SetTooltip("Refresh shader list");
                if (clicked) m_shaderClaw.refreshManifest();
            }

            ImGui::SameLine(0, 6);

            // Disconnect — neutral ghost pill (no alarming red).
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.72f, 0.76f, 0.82f, 1.0f));
            if (ImGui::SmallButton("Disconnect")) {
                m_shaderClaw.disconnect();
                m_scThumbnails.clear();
                m_scThumbRenderer.reset();
                m_scPreview.reset();
                m_scPreviewPath.clear();
            }
            ImGui::PopStyleColor(4);

            // (Old wide Refresh button + hairline divider removed — they added
            // clutter without structural value.)
            ImGui::Dummy(ImVec2(0, 6));

            // Update animated preview shader (for hovered item)
            bool previewValid = false;
            if (m_scPreview) {
                m_scPreview->setResolution(64, 64);
                m_scPreview->update();
                m_scPreviewFrame++;
                previewValid = (m_scPreviewFrame > 2);
            }

            // Generate static thumbnails (one shader per frame to avoid lag).
            // Rendered at 160x160 for the gallery grid — bigger than the old
            // 48x48 list cell so thumbnails read even at 2x DPI.
            const int kThumbRes = 160;
            if (m_scThumbRenderer) {
                m_scThumbRenderer->setResolution(kThumbRes, kThumbRes);
                m_scThumbRenderer->update();
                m_scThumbRenderFrame++;
                if (m_scThumbRenderFrame > 3) {
                    auto& entry = m_scThumbnails[m_scThumbRenderPath];
                    if (!entry.texture) entry.texture = std::make_shared<Texture>();
                    std::vector<uint8_t> pixels(kThumbRes * kThumbRes * 4);
                    GLint prevFBO;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                    GLuint thumbTex = m_scThumbRenderer->textureId();
                    if (thumbTex != 0) {
                        GLuint tempFBO;
                        glGenFramebuffers(1, &tempFBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, tempFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, thumbTex, 0);
                        glReadPixels(0, 0, kThumbRes, kThumbRes, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                        glDeleteFramebuffers(1, &tempFBO);
                        entry.texture->createEmpty(kThumbRes, kThumbRes);
                        entry.texture->updateData(pixels.data(), kThumbRes, kThumbRes);
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

            // ─── Sub-tabs: VFX vs Text ──────────────────────────────────
            // Split the shader library by intent: text-based shaders go
            // under "Text" (anything tagged Text in the manifest), the
            // rest under "VFX". Keeps the grid scannable when there are
            // 70+ shaders by separating the two main creative modes.
            static int s_scSubTab = 0; // 0 = VFX, 1 = Text
            {
                auto subTabBtn = [&](const char* label, int idx) {
                    bool active = (s_scSubTab == idx);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.96f, 0.97f, 1.00f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.05f, 0.07f, 0.10f, 1.00f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.78f, 0.80f, 0.85f, 1.0f));
                    }
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
                    if (ImGui::Button(label, ImVec2(0, 0))) s_scSubTab = idx;
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);
                };
                subTabBtn("VFX",  0);
                ImGui::SameLine(0, 6);
                subTabBtn("Text", 1);
                ImGui::Dummy(ImVec2(0, 6));
            }

            // ─── Gallery grid ───────────────────────────────────────────
            // Big thumbnail tiles laid out in a responsive grid (auto-sizes
            // columns to panel width). Each tile = thumbnail + title. Click a
            // thumbnail to add that shader as a layer; hover to see it
            // animated. Works like a shader-picker gallery.
            std::string hoveredPath;
            const float cellPad     = 8.0f;
            const float labelH      = 22.0f;
            const float minCellW    = 132.0f;
            const float maxCellW    = 200.0f;
            float availW = ImGui::GetContentRegionAvail().x;
            // Figure out how many columns fit.
            int cols = std::max(1, (int)((availW + cellPad) / (minCellW + cellPad)));
            float cellW = (availW - cellPad * (cols - 1)) / (float)cols;
            if (cellW > maxCellW) cellW = maxCellW;
            float thumbSize = cellW;           // square thumbs, full cell width
            float cellH = thumbSize + labelH;

            const auto& shaders = m_shaderClaw.shaders();
            // Build the visible-index list once per frame so column
            // wrapping ignores hidden entries cleanly. A shader counts
            // as Text iff its categories include "Text" (or its
            // filename starts with "text_" as a fallback for entries
            // that haven't been re-tagged yet).
            std::vector<int> visibleIdx;
            visibleIdx.reserve(shaders.size());
            for (int i = 0; i < (int)shaders.size(); i++) {
                const auto& sh = shaders[i];
                bool isText = false;
                for (const auto& c : sh.categories) {
                    if (c == "Text") { isText = true; break; }
                }
                if (!isText && sh.file.rfind("text_", 0) == 0) isText = true;
                bool keep = (s_scSubTab == 1) ? isText : !isText;
                if (keep) visibleIdx.push_back(i);
            }
            for (int vi = 0; vi < (int)visibleIdx.size(); vi++) {
                int i = visibleIdx[vi];
                const auto& shader = shaders[i];
                ImGui::PushID(2000 + i);

                // Layout: new row every `cols` items (using the
                // VISIBLE index so wrapping is unaffected by hidden
                // entries from the other sub-tab).
                if (vi % cols != 0) ImGui::SameLine(0, cellPad);
                ImVec2 cellPos = ImGui::GetCursorScreenPos();

                bool isHoveredShader = (shader.fullPath == m_scPreviewPath);
                GLuint thumbTex = 0;
                if (isHoveredShader && previewValid && m_scPreview) {
                    thumbTex = m_scPreview->textureId();
                } else {
                    auto it = m_scThumbnails.find(shader.fullPath);
                    if (it != m_scThumbnails.end() && it->second.ready && it->second.texture)
                        thumbTex = it->second.texture->id();
                }

                // Invisible hit button covering the whole cell — click = add, hover = preview.
                bool clicked = ImGui::InvisibleButton("##tile", ImVec2(thumbSize, cellH));
                bool hov = ImGui::IsItemHovered();
                ImDrawList* d = ImGui::GetWindowDrawList();

                // Tile background + subtle border (brightens on hover).
                ImU32 tileBg   = hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
                ImU32 tileEdge = hov ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 255, 255, 50);
                d->AddRectFilled(cellPos,
                                 ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                                 tileBg, 6.0f);
                d->AddRect(cellPos,
                           ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                           tileEdge, 6.0f, 0, 1.0f);

                // Thumbnail — clipped square at top of cell.
                ImVec2 thumbMin(cellPos.x + 4, cellPos.y + 4);
                ImVec2 thumbMax(cellPos.x + thumbSize - 4, cellPos.y + thumbSize - 4);
                if (thumbTex) {
                    d->AddImageRounded((ImTextureID)(intptr_t)thumbTex,
                                       thumbMin, thumbMax,
                                       ImVec2(0, 1), ImVec2(1, 0),
                                       IM_COL32(255, 255, 255, 255), 4.0f);
                } else {
                    d->AddRectFilled(thumbMin, thumbMax,
                                     IM_COL32(25, 28, 38, 255), 4.0f);
                    // Centered ellipsis to hint that a thumbnail is still cooking.
                    const char* wait = "...";
                    ImVec2 ws = ImGui::CalcTextSize(wait);
                    d->AddText(ImVec2(thumbMin.x + (thumbMax.x - thumbMin.x - ws.x) * 0.5f,
                                      thumbMin.y + (thumbMax.y - thumbMin.y - ws.y) * 0.5f),
                               IM_COL32(120, 130, 150, 200), wait);
                }

                // Title — truncated to fit the cell width.
                std::string title = shader.title.empty() ? "(untitled)" : shader.title;
                float titleMaxW = thumbSize - 16.0f;
                ImVec2 ts = ImGui::CalcTextSize(title.c_str());
                if (ts.x > titleMaxW) {
                    // Naive truncation — trim until it fits + ellipsis.
                    while (title.size() > 1 && ImGui::CalcTextSize((title + "...").c_str()).x > titleMaxW) {
                        title.pop_back();
                    }
                    title += "...";
                    ts = ImGui::CalcTextSize(title.c_str());
                }
                d->AddText(ImVec2(cellPos.x + (thumbSize - ts.x) * 0.5f,
                                  cellPos.y + thumbSize - 2),
                           isHoveredShader ? IM_COL32(255, 255, 255, 255)
                                           : IM_COL32(220, 226, 235, 220),
                           title.c_str());

                if (hov) {
                    hoveredPath = shader.fullPath;
                    if (!shader.description.empty()) {
                        ImGui::SetTooltip("%s\n\n%s", shader.title.c_str(), shader.description.c_str());
                    } else {
                        ImGui::SetTooltip("%s", shader.title.c_str());
                    }
                }
                if (clicked) loadShader(shader.fullPath);

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

            // Fetch sessions button — secondary pill (light fill).
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.24f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.85f, 0.87f, 0.92f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 10));
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
            ImGui::PopStyleVar(2);

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

            ImGui::Dummy(ImVec2(0, 6));
            // Connect — primary pill action, near-black fill on the dark
            // panel (mirrors the reference's "Update" button).
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.06f, 0.07f, 0.09f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.13f, 0.16f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.20f, 0.24f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.97f, 0.98f, 0.98f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 12));
            if (ImGui::Button("Connect", ImVec2(-1, 0))) {
                std::string sid = (selectedSession >= 0 && selectedSession < (int)sessions.size())
                    ? sessions[selectedSession].id : "";
                m_ethereaClient.connect(etUrl, sid);
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
        } else {
            // Connected state — compact: status dot + Disconnect pill on
            // a single row, then transcript/hints/prompt below.
            {
                bool ws  = m_ethereaClient.wsConnected();
                bool sse = m_ethereaClient.sseConnected();
                bool fullyConnected = ws && sse;
                ImU32 dotCol = fullyConnected
                    ? IM_COL32(34, 210, 130, 255)        // green
                    : (ws || sse)
                        ? IM_COL32(220, 180, 60, 255)    // amber — partial
                        : IM_COL32(220, 70, 70, 255);    // red — not connected
                ImVec2 cp = ImGui::GetCursorScreenPos();
                float h = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(cp.x + 6, cp.y + h * 0.5f), 4.0f, dotCol);
                ImGui::Dummy(ImVec2(16, h));
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.80f, 0.85f, 0.9f));
                ImGui::TextUnformatted(fullyConnected ? "Connected"
                                                       : (ws || sse) ? "Partial" : "Connecting…");
                ImGui::PopStyleColor();
            }

            // Right-aligned Disconnect — neutral pill, not red.
            float discW = ImGui::CalcTextSize("Disconnect").x + 24.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - discW);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.78f, 0.80f, 0.85f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
            if (ImGui::Button("Disconnect", ImVec2(discW, 0))) {
                m_ethereaClient.disconnect();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);

            ImGui::Dummy(ImVec2(0, 8));

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextUnformatted("TRANSCRIPT");
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
        // Scope Stream — auto-detect live pods via etherea health API.
        // (Divider line removed per UI cleanup pass — the collapsing
        // header below is enough visual separation.)
        ImGui::Dummy(ImVec2(0, 12));

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

    if (sourcesTabsOpen && ImGui::BeginTabItem("Particles")) {
        ImGui::TextWrapped("GPU particle system — drop onto a layer.");
        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Button("+ Add Particle System", ImVec2(-1, 0))) {
            addParticles();
        }
        ImGui::EndTabItem();
    }

#ifdef HAS_OPENCV
    if (sourcesTabsOpen && ImGui::BeginTabItem("Camera")) {
        ImGui::TextWrapped("Live webcam feed — drop onto a layer.");
        ImGui::Dummy(ImVec2(0, 8));

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(9000 + i);
            char label[32];
            snprintf(label, sizeof(label), "+ Add Camera %d", i);
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                addWebcam(i);
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor(4);

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
        ImGui::TextWrapped("Camera 0 is typically the built-in webcam. If a camera is already in use by the scene scanner it cannot be opened a second time.");
        ImGui::PopStyleColor();

        ImGui::EndTabItem();
    }
#endif

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
    // Match the conditional Begin() above — only End() if we Begin'd.
    if (sourcesTabsOpen) ImGui::EndTabBar();
    if (sourcesVisible) ImGui::End();

// (Stream panel removed — stream key + aspect now live in the GO LIVE
//  popup on the timeline transport bar.)

    // Audio panel — BPM, device, levels, gain controls
    if (m_ui.isPanelVisible("Audio")) {
    ImGui::Begin("        ###Audio");
    {
        // --- Device selection: [ Input ] [ combo ................ ] [ Refresh ]
        //     Single-line layout — label + combo + refresh all share a row.
#ifdef HAS_FFMPEG
        if (m_audioDevices.empty()) {
            m_audioDevices = VideoRecorder::enumerateAudioDevices();
        }
        const char* currentName = "System Audio (loopback)";
        if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
            currentName = m_audioDevices[m_selectedAudioDevice].name.c_str();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Input");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // Reserve room for the Refresh button at the right, combo fills the rest.
        float refreshW = ImGui::CalcTextSize("Refresh").x
                       + ImGui::GetStyle().FramePadding.x * 2.0f;
        float availRow = ImGui::GetContentRegionAvail().x;
        float comboW   = availRow - refreshW - ImGui::GetStyle().ItemSpacing.x;
        if (comboW < 80.0f) comboW = 80.0f;
        ImGui::SetNextItemWidth(comboW);
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
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##audio")) {
            m_audioDevices = VideoRecorder::enumerateAudioDevices();
        }
#else
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Input");
        ImGui::PopStyleColor();
        ImGui::SameLine();
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

        // Labeled gain rows — label in a fixed left column, slider fills
        // the remainder. Previously the label rendered AFTER the slider
        // (ImGui default) and got clipped off the panel's right edge.
        auto gainRow = [](const char* label, const char* id, float* v,
                           float minV, float maxV, const char* fmt) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.63f, 0.70f, 1.0f));
            ImGui::Text("%s", label);
            ImGui::PopStyleColor();
            ImGui::SameLine(64);
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat(id, v, minV, maxV, fmt);
        };
        gainRow("Input",  "##masterGain", &m_audioAnalyzer.inputGain(),   0.0f, 10.0f, "%.2fx");
        gainRow("Bass",   "##bGain",      &m_audioAnalyzer.bassGain(),    0.0f,  5.0f, "%.2fx");
        gainRow("Low",    "##lmGain",     &m_audioAnalyzer.lowMidGain(),  0.0f,  5.0f, "%.2fx");
        gainRow("High",   "##hmGain",     &m_audioAnalyzer.highMidGain(), 0.0f,  5.0f, "%.2fx");
        gainRow("Treble", "##tGain",      &m_audioAnalyzer.trebleGain(),  0.0f,  5.0f, "%.2fx");

        ImGui::Dummy(ImVec2(0, 4));
        gainRow("Gate", "##nGate", &m_audioAnalyzer.noiseGate(), 0.0f, 0.5f, "%.2f");

        if (ImGui::SmallButton("Reset Gains")) {
            m_audioAnalyzer.inputGain() = 1.0f;
            m_audioAnalyzer.bassGain() = 1.0f;
            m_audioAnalyzer.lowMidGain() = 1.0f;
            m_audioAnalyzer.highMidGain() = 1.0f;
            m_audioAnalyzer.trebleGain() = 1.0f;
            m_audioAnalyzer.noiseGate() = 0.0f;
        }

#ifdef HAS_FFMPEG
        // --- Mixer (merged from the old Audio Mixer panel) ---
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SeparatorText("Mixer");
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
            float master = m_audioMixer.masterVolume() * 100.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##Master", &master, 0.0f, 100.0f, "Master  %.0f%%")) {
                m_audioMixer.setMasterVolume(master / 100.0f);
            }
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
            int numInputs = m_audioMixer.inputCount();
            for (int i = 0; i < numInputs; i++) {
                ImGui::PushID(i + 2000);
                std::string iname = m_audioMixer.inputName(i);
                if (iname.empty()) iname = "Input " + std::to_string(i);
                bool muted = m_audioMixer.isInputMuted(i);
                if (ImGui::Checkbox("##mute", &muted)) {
                    m_audioMixer.setInputMuted(i, muted);
                }
                ImGui::SameLine();
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
#endif

        // --- BPM (bottom of panel — secondary to the metering and gain
        //     controls above, which change far more often during a session).
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));
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
        }
    }
    ImGui::End();
    }  // end Audio visibility guard

    // MIDI panel — device selection + mapping
    if (m_ui.isPanelVisible("MIDI")) {
    ImGui::Begin("        ###MIDI");
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

    // Audio Mixer panel merged into Audio; transport controls now live inside
    // the Timeline panel's transport row (renderTransportBar() is a no-op).

    // Timeline panel — shows per-layer tracks with clips along a time ruler.
    // Single playhead, linear time, clip-on-track model (AE/Resolume style).
    if (m_ui.isPanelVisible("Timeline")) {
        renderTimelinePanel();
    }

    // Overlay inspector tab icons (Properties/Mapping/Audio/MIDI) after all
    // panels have rendered so the icon painting lands on top of ImGui's
    // native tab bar text.
    m_ui.drawInspectorTabIcons();

    // Scenes panel now renders in the Stage-view scope above (where zoneTextures is live).
}

// Render the timeline's Work Area to a timestamped .mp4. Seeks the playhead to
// the in-point, starts playback + recorder, and arms the render-loop auto-stop
// that fires when the playhead crosses the out-point.
void Application::startTimelineExport() {
#ifdef HAS_FFMPEG
    if (m_timelineExporting || m_recorder.isActive()) return;

    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char fname[160];
    strftime(fname, sizeof(fname), "recordings/timeline_%Y%m%d_%H%M%S.mp4", &tm_buf);

    auto& zone = activeZone();
    m_recorder.setAudioDevice(m_selectedAudioDevice);
    if (!m_recorder.start(fname, zone.warpFBO.width(), zone.warpFBO.height(), 30)) {
        std::cerr << "[Timeline] Export failed: recorder.start() returned false\n";
        return;
    }

    double waStart = m_timeline.workAreaStart();
    double waEnd   = m_timeline.workAreaEnd();
    m_timeline.seek(waStart);
    m_timeline.play();

    m_timelineExporting = true;
    m_timelineExportEnd = waEnd;
    m_timelineExportPath = fname;
    std::cerr << "[Timeline] Exporting " << waStart << "s → " << waEnd
              << "s to " << fname << "\n";
#endif
}

// Minimal show-programming timeline UI. Transport + ruler + per-layer tracks.
// Interactions: click ruler to seek, drag playhead to scrub, drag clip body to
// move, drag clip edges to trim, right-click track for +Clip at cursor.
void Application::renderTimelinePanel() {
    // Tight edge margins — left 12px matches the Canvas/Stage nav indent,
    // vertical 16px keeps the transport row centred inside the minimised
    // height (FrameHeight + 32). Previously 20px left caused play/stop to
    // float ~40px from the window edge while REC hugged the right.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 16));
    // NoCollapse disables ImGui's default title-bar chevron — we render our
    // own minimize button at the far right of the transport row instead.
    ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();

    // Auto-resize the dock node when the minimize flag toggles. Docked windows
    // don't obey SetNextWindowSize, so we reach into the dock builder and
    // resize the containing node directly. Previous expanded height is saved
    // so "expand" restores whatever height the user had dragged to.
    //
    // Expanded-height auto-fit: if the user never dragged the splitter, we
    // size the node to fit the current content (transport + ruler + tracks +
    // audio lane) so empty space below the audio waveform doesn't render as
    // a dead black strip. Once they drag, that override wins until the next
    // minimize cycle.
    // File-scope static lives at the bottom of the function too (measured
    // there). Forward-declared here so both ends share the same storage.
    static float s_tlMeasuredContentH = 0.0f;
    {
        static bool   s_prevMinimized = false;
        static ImVec2 s_savedExpanded(0, 0);
        // Tab bar is hidden via NoTabBar, so we don't reserve space for it.
        // Minimised height = one transport row + 16px even top/bottom
        // padding (matches the WindowPadding push above — no dead strip).
        const float tabBarH = ImGui::GetFrameHeight() + 4.0f;
        const float minH = ImGui::GetFrameHeight() + 32.0f; // transport-only
        const float fitH = (s_tlMeasuredContentH > 0.0f)
                           ? s_tlMeasuredContentH + tabBarH
                           : 160.0f;
        if (m_timelineMinimized != s_prevMinimized) {
            ImGuiID dockId = ImGui::GetWindowDockID();
            if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId)) {
                if (m_timelineMinimized) {
                    // Only save a meaningful expanded height — if the panel
                    // was already tiny for some reason (no tracks on launch
                    // etc.), falling back to fitH on next expand is better
                    // than restoring to a useless 50px sliver.
                    if (node->Size.y > minH + 40.0f) s_savedExpanded = node->Size;
                    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(node->Size.x, minH));
                } else {
                    // Expand: pick the largest of (last-dragged, fit-to-
                    // content, a sensible default) so the panel always pops
                    // up visibly even if the saved height was stale.
                    float restoreH = s_savedExpanded.y;
                    if (fitH     > restoreH) restoreH = fitH;
                    if (220.0f   > restoreH) restoreH = 220.0f;
                    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(node->Size.x, restoreH));
                }
            }
            s_prevMinimized = m_timelineMinimized;
        } else {
            // Every frame: if we have a measured content height and the node
            // is close to our last-known target, snap it to the fresh measure.
            // This kills the dead black strip below the audio lane AND follows
            // layer add/remove without dragging.
            ImGuiID dockId = ImGui::GetWindowDockID();
            if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId)) {
                static float s_lastFitH = 0.0f;
                float targetH = m_timelineMinimized ? minH : fitH;
                if (s_lastFitH > 0.0f && std::abs(node->Size.y - s_lastFitH) < 8.0f) {
                    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(node->Size.x, targetH));
                }
                s_lastFitH = fitH;
            }
        }
    }

    static bool s_tlCollapsed = false;

    // Zoom & horizontal scroll state. Hoisted so the transport-row zoom slider
    // can drive them (previously hidden inside the ruler block, and wheel-driven).
    static float  s_tlZoom   = 1.0f;
    static double s_tlScroll = 0.0;
    if (s_tlZoom < 1.0f)  s_tlZoom = 1.0f;
    if (s_tlZoom > 64.0f) s_tlZoom = 64.0f;

    // ── Auto-sync tracks with the layer stack ─────────────────────────────
    // Every layer gets exactly one track. Newly-added layers also get a default
    // clip spanning their natural duration (video length if known, otherwise
    // the full timeline) — layers appear as editable bars immediately, no
    // right-click ritual required.
    {
        std::unordered_set<uint32_t> liveIds;
        for (int i = 0; i < m_layerStack.count(); i++) {
            auto l = m_layerStack[i];
            if (!l || l->id == 0) continue;
            liveIds.insert(l->id);
            if (auto* tr = m_timeline.findTrack(l->id)) {
                tr->name = l->name;  // keep label in sync
            } else {
                m_timeline.ensureTrack(l->id, l->name);
                double d = (l->source) ? l->source->duration() : 0.0;
                if (d <= 0.0) d = m_timeline.duration();
                if (d > m_timeline.duration()) d = m_timeline.duration();
                m_timeline.addClip(l->id, 0.0, d, l->name);
            }
        }
        // Remove tracks for deleted layers.
        auto& tracks = m_timeline.tracks();
        for (int i = (int)tracks.size() - 1; i >= 0; i--) {
            if (!liveIds.count(tracks[i].layerId)) {
                m_timeline.removeTrackForLayer(tracks[i].layerId);
            }
        }
    }

    // Keyboard shortcuts (only when the Timeline window is focused)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) m_timeline.togglePlay();
        if (ImGui::IsKeyPressed(ImGuiKey_Home))  m_timeline.seek(0.0);
        if (ImGui::IsKeyPressed(ImGuiKey_End))   m_timeline.seek(m_timeline.duration());
        // Work area: I sets in-point, O sets out-point, Backslash resets.
        if (ImGui::IsKeyPressed(ImGuiKey_I)) {
            m_timeline.setWorkArea(m_timeline.playhead(), m_timeline.workAreaEnd());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O)) {
            m_timeline.setWorkArea(m_timeline.workAreaStart(), m_timeline.playhead());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backslash)) {
            m_timeline.resetWorkArea();
        }
        // K = split every clip the playhead touches at the playhead.
        // (AE uses Ctrl+Shift+D; Premiere uses K then I/O; we pick the most
        //  direct mnemonic since Space is already Play.)
        if (ImGui::IsKeyPressed(ImGuiKey_K)) {
            double ph = m_timeline.playhead();
            for (auto& tr : m_timeline.tracks()) {
                // Snapshot: can't mutate clips while iterating.
                std::vector<uint32_t> hits;
                for (auto& c : tr.clips) {
                    if (ph > c.startTime + 0.05 && ph < c.endTime() - 0.05) hits.push_back(c.id);
                }
                for (uint32_t cid : hits) {
                    auto* c = m_timeline.findClip(tr.layerId, cid);
                    if (!c) continue;
                    double splitOffset = ph - c->startTime;
                    double rightDur = c->duration - splitOffset;
                    // New right-hand clip inherits source + sourceIn offset so video resumes.
                    auto* nc = m_timeline.addClip(tr.layerId, ph, rightDur, c->name, c->sourcePath);
                    if (nc) {
                        nc->kind = c->kind;
                        nc->tint = c->tint;
                        nc->sourceIn = c->sourceIn + splitOffset;
                        nc->sourceOut = c->sourceOut;
                    }
                    c->duration = splitOffset;
                }
                if (!hits.empty()) m_timeline.sortTrack(tr.layerId);
            }
        }
        // M = drop a marker at the playhead (scripted-show cue point).
        if (ImGui::IsKeyPressed(ImGuiKey_M)) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
            m_timeline.addMarker(m_timeline.playhead(), "Cue");
        }
        // (Backspace/Delete handler for selected clip lives later in the
        //  function where s_ctxClipId is in scope.)
    }

    // --- Layout constants (shared with ruler + tracks so everything aligns) ---
    const float labelW   = 120.0f;   // track-name / mute-solo column width
    const float trackH   = 36.0f;
    const float rulerH   = 20.0f;

    // (Timeline tab bar is hidden via ImGuiDockNodeFlags_NoTabBar — no
    // minimise/expand chevron rendered here. m_timelineMinimized is still
    // honoured below via the transport row keeping its height while the
    // ruler/track rows are collapsed when the user sets the flag some
    // other way, e.g. future keyboard shortcut.)

    // --- Transport row — shifted to align with the ruler region so the timeline
    //     reads as one vertical stack instead of ruler-indented-from-transport.
    {
        // Icon buttons: drawn with ImDrawList so glyph coverage is never a factor.
        auto iconBtn = [&](const char* id, int kind, bool active = false) -> bool {
            // kind: 0 = play ▶, 1 = pause ‖, 2 = stop ■, 3 = loop ↻
            // Match the height of framed widgets (DragFloat, Button) so the
            // whole transport row baselines cleanly.
            float h = ImGui::GetFrameHeight();
            ImVec2 size(h + 6.0f, h);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton(id, size);
            bool hov = ImGui::IsItemHovered();
            ImDrawList* d = ImGui::GetWindowDrawList();
            ImU32 bg = active ? IM_COL32(255, 255, 255, 36)
                     : hov    ? IM_COL32(255, 255, 255, 22)
                              : IM_COL32(255, 255, 255, 10);
            ImU32 fg = IM_COL32(235, 238, 244, 240);
            d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 6.0f);
            float cx = p.x + size.x * 0.5f;
            float cy = p.y + size.y * 0.5f;
            if (kind == 0) {
                // Play — right-pointing triangle
                d->AddTriangleFilled(ImVec2(cx - 3, cy - 5),
                                     ImVec2(cx + 5, cy),
                                     ImVec2(cx - 3, cy + 5), fg);
            } else if (kind == 1) {
                // Pause — two vertical bars
                d->AddRectFilled(ImVec2(cx - 4, cy - 5), ImVec2(cx - 1, cy + 5), fg, 1.0f);
                d->AddRectFilled(ImVec2(cx + 1, cy - 5), ImVec2(cx + 4, cy + 5), fg, 1.0f);
            } else if (kind == 2) {
                // Stop — filled square
                d->AddRectFilled(ImVec2(cx - 4, cy - 4), ImVec2(cx + 4, cy + 4), fg, 1.0f);
            } else if (kind == 3) {
                // Loop — infinity (∞) lemniscate: x = sin(t)*A, y = sin(2t)*A/2
                const int n = 32;
                for (int i = 0; i <= n; i++) {
                    float t = (float)i / n * 6.2831853f;
                    float xo = sinf(t) * 6.5f;
                    float yo = sinf(t * 2.0f) * 2.8f;
                    d->PathLineTo(ImVec2(cx + xo, cy + yo));
                }
                d->PathStroke(fg, 0, 1.6f);
            } else if (kind == 4) {
                // Minimize — horizontal bar (—)
                d->AddRectFilled(ImVec2(cx - 5, cy - 1), ImVec2(cx + 5, cy + 1), fg, 1.0f);
            } else if (kind == 5) {
                // Expand — upward chevron (⌃)
                d->AddLine(ImVec2(cx - 4, cy + 2), ImVec2(cx, cy - 3), fg, 1.6f);
                d->AddLine(ImVec2(cx + 4, cy + 2), ImVec2(cx, cy - 3), fg, 1.6f);
            } else if (kind == 6) {
                // Panel-collapse — downward chevron (▾)
                d->AddLine(ImVec2(cx - 4, cy - 2), ImVec2(cx, cy + 3), fg, 1.6f);
                d->AddLine(ImVec2(cx + 4, cy - 2), ImVec2(cx, cy + 3), fg, 1.6f);
            } else if (kind == 7) {
                // Panel-expand — upward chevron (▴)
                d->AddLine(ImVec2(cx - 4, cy + 2), ImVec2(cx, cy - 3), fg, 1.6f);
                d->AddLine(ImVec2(cx + 4, cy + 2), ImVec2(cx, cy - 3), fg, 1.6f);
            }
            return clicked;
        };

        // Transport sits flush-left against the window padding — same left edge
        // as the REC / output-route buttons in the bottom transport bar.
        // Minimize button moved out of this row — it lives at the top-left of
        // the panel, visually adjacent to the "Timeline" tab header.
        // Expand / collapse the panel. When minimized, the chevron points up
        // (click to reveal tracks); when expanded, it points down (click to
        // collapse to the transport-only strip). This is the only visible
        // affordance for the minimize state — without it new users have no way
        // to find their tracks since the timeline ships minimized.
        if (iconBtn("##TimelineMinimize", m_timelineMinimized ? 7 : 6))
            m_timelineMinimized = !m_timelineMinimized;
        ImGui::SameLine();

        bool playing = m_timeline.isPlaying();
        if (iconBtn("##PlayPause", playing ? 1 : 0, playing)) m_timeline.togglePlay();
        ImGui::SameLine();
        if (iconBtn("##Stop", 2)) m_timeline.stop();
        ImGui::SameLine();
        bool loop = m_timeline.looping();
        if (iconBtn("##Loop", 3, loop)) m_timeline.setLooping(!loop);

        // Timecode — click-to-seek playhead, double-click to edit total length.
        // Hit-target is FrameHeight tall so vertical center matches the icon
        // buttons and framed widgets on the rest of the row.
        ImGui::SameLine(0, 16);
        double ph = m_timeline.playhead();
        int pm = (int)ph / 60, ps = (int)ph % 60;
        double dur = m_timeline.duration();
        int dm = (int)dur / 60, ds = (int)dur % 60;
        char tcBuf[32];
        snprintf(tcBuf, sizeof(tcBuf), "%02d:%02d / %02d:%02d", pm, ps, dm, ds);
        ImVec2 tcTextSize = ImGui::CalcTextSize(tcBuf);
        float  tcFrameH   = ImGui::GetFrameHeight();
        ImVec2 tcPos      = ImGui::GetCursorScreenPos();
        bool tcClicked = ImGui::InvisibleButton("##TCEdit",
                                                ImVec2(tcTextSize.x, tcFrameH));
        bool tcDouble  = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
        // Draw text vertically centered inside the FrameHeight-tall hit box.
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(tcPos.x, tcPos.y + (tcFrameH - tcTextSize.y) * 0.5f),
            ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255)
                                   : IM_COL32(220, 226, 235, 235),
            tcBuf);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Double-click to edit timeline duration.");
        if (tcDouble) ImGui::OpenPopup("##DurEdit");
        (void)tcClicked;

        // Inline duration editor — opens beneath the timecode on double-click.
        if (ImGui::BeginPopup("##DurEdit")) {
            ImGui::TextDisabled("Timeline duration");
            ImGui::Separator();
            static int s_mm = 0, s_ss = 0;
            // Seed from the live duration each time the popup opens fresh.
            if (!ImGui::IsItemVisible() && !ImGui::IsAnyItemFocused()) {
                s_mm = dm; s_ss = ds;
            }
            ImGui::SetNextItemWidth(50);
            ImGui::InputInt("##MM", &s_mm, 0); ImGui::SameLine(); ImGui::TextUnformatted("m");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            ImGui::InputInt("##SS", &s_ss, 0); ImGui::SameLine(); ImGui::TextUnformatted("s");
            if (ImGui::Button("Set", ImVec2(-1, 0))) {
                if (s_mm < 0) s_mm = 0; if (s_ss < 0) s_ss = 0;
                if (s_ss > 59) s_ss = 59;
                double d = s_mm * 60.0 + s_ss;
                if (d < 1.0) d = 1.0;
                m_timeline.setDuration(d);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // `Dur 60s` numeric field stays as a secondary way to adjust duration.
        // With ConfigDragClickToInputText=true, a click on it becomes a text
        // edit, so users can also type here without touching the timecode.
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Dur");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(72);
        float dsec = (float)m_timeline.duration();
        if (ImGui::DragFloat("##Dur", &dsec, 1.0f, 1.0f, 3600.0f * 4.0f, "%.0fs")) {
            m_timeline.setDuration((double)dsec);
        }

        // Zoom slider — replaces the old wheel-zoom. Logarithmic feel so the
        // whole 1x→64x range fits a short slider without low-zoom being cramped.
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Zoom");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        {
            float logZ = std::log2(s_tlZoom);  // 0..6 for 1..64
            if (ImGui::SliderFloat("##Zoom", &logZ, 0.0f, 6.0f, "")) {
                s_tlZoom = std::pow(2.0f, logZ);
                if (s_tlZoom < 1.0f)  s_tlZoom = 1.0f;
                if (s_tlZoom > 64.0f) s_tlZoom = 64.0f;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Zoom: %.1fx — shows %.1fs at a time\n"
                                  "(double-click ruler to reset)",
                                  s_tlZoom, m_timeline.duration() / s_tlZoom);
            }
        }

        // ── Transport row layout (left → right):
        //   [play/stop/loop] [time] [dur] [zoom]
        //   ... [AUDIO COMBO][METER]  (centered together as one group)
        //   ... [WORK AREA (click-to-edit)] [REC]
        //
        // REC is the single record/export action — it starts a timeline export
        // (seeks to Work Area start, plays + records, auto-stops at Work Area
        // end). It lives right next to the Work Area readout so "this time
        // range → record this" reads as a single workflow unit.
#ifdef HAS_FFMPEG
        const float kRecBtnW     = 64.0f;
        const float kAudioComboW = 150.0f;
        const float kMeterW      = 100.0f;
        const float kGap         = 10.0f;

        auto& zone = activeZone();
        float frameH = ImGui::GetFrameHeight();
        updateAudioMeter();
        (void)zone;

        auto pillBtn = [&](const char* id, const char* label, float w,
                           ImU32 borderCol, ImU32 textCol, bool enabled = true) -> bool {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 size(w, frameH);
            if (!enabled) ImGui::BeginDisabled();
            bool clicked = ImGui::InvisibleButton(id, size);
            bool hov = ImGui::IsItemHovered();
            if (!enabled) ImGui::EndDisabled();
            ImDrawList* d = ImGui::GetWindowDrawList();
            ImU32 bg = hov ? IM_COL32(255, 255, 255, 30)
                           : IM_COL32(255, 255, 255, 15);
            d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 5.0f);
            d->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), borderCol, 5.0f, 0, 1.0f);
            ImVec2 ts = ImGui::CalcTextSize(label);
            d->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f,
                              p.y + (size.y - ts.y) * 0.5f),
                       textCol, label);
            return clicked && enabled;
        };

        const ImU32 kAccentDim = IM_COL32(255, 255, 255, 80);
        const ImU32 kRed       = IM_COL32(255, 70, 70, 255);
        const ImU32 kRedDim    = IM_COL32(255, 70, 70, 140);
        float timeNow = (float)ImGui::GetTime();

        bool recording  = m_recorder.isActive();
        bool exporting  = m_timelineExporting;

        // Refresh audio device enumeration every 3s so the combo stays current.
        {
            static double lastEnum = 0;
            double nowT = glfwGetTime();
            if (m_audioDevices.empty() || nowT - lastEnum > 3.0) {
                lastEnum = nowT;
                m_audioDevices = VideoRecorder::enumerateAudioDevices();
                m_outputDevices.clear();
                for (auto& dv : m_audioDevices) {
                    if (!dv.isCapture) m_outputDevices.push_back(dv);
                }
            }
        }

        // Work Area label (pre-compute so right-anchored width is known).
        double wa0 = m_timeline.workAreaStart();
        double wa1 = m_timeline.workAreaEnd();
        int wm0 = (int)wa0 / 60, ws0 = (int)wa0 % 60;
        int wm1 = (int)wa1 / 60, ws1 = (int)wa1 % 60;
        char waLbl[48];
        snprintf(waLbl, sizeof(waLbl), "%d:%02d-%d:%02d", wm0, ws0, wm1, ws1);
        float waW = ImGui::CalcTextSize(waLbl).x;

        // Centered group: [Audio combo][Meter] — treated as one block so both
        // sit in the middle of the transport row, with the meter reading next
        // to the source that drives it.
        float centerGroupW = kAudioComboW + kGap + kMeterW;
        std::string audioPreview = m_mixerEnabled ? "Mixer" : "System Audio";
        if (!m_mixerEnabled && m_selectedAudioDevice >= 0 &&
            m_selectedAudioDevice < (int)m_audioDevices.size()) {
            audioPreview = m_audioDevices[m_selectedAudioDevice].name;
            if (audioPreview.length() > 20) audioPreview = audioPreview.substr(0, 17) + "...";
        }
        ImGui::SameLine(0, 0);
        float contentMaxX = ImGui::GetWindowContentRegionMax().x;
        float contentMinX = ImGui::GetWindowContentRegionMin().x;
        float centerX = (contentMinX + contentMaxX) * 0.5f - centerGroupW * 0.5f;
        if (centerX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(centerX);
        ImGui::SetNextItemWidth(kAudioComboW);
        if (ImGui::BeginCombo("##AudioSrc", audioPreview.c_str())) {
            if (ImGui::Selectable("System Audio", !m_mixerEnabled && m_selectedAudioDevice == -1 && m_audioAnalyzer.wantsSystemAudio())) {
                if (m_mixerEnabled) { m_audioMixer.stop(); m_audioAnalyzer.setExternalFeed(false); m_mixerEnabled = false; }
                m_selectedAudioDevice = -1;
                m_audioAnalyzer.setWantsSystemAudio(true);
            }
            for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(m_audioDevices[i].name.c_str(), !m_mixerEnabled && m_selectedAudioDevice == i)) {
                    if (m_mixerEnabled) { m_audioMixer.stop(); m_audioAnalyzer.setExternalFeed(false); m_mixerEnabled = false; }
                    m_selectedAudioDevice = i;
                    m_audioAnalyzer.setWantsSystemAudio(false);
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Mixer", m_mixerEnabled)) {
                if (!m_mixerEnabled) {
                    m_mixerEnabled = true;
                    m_audioAnalyzer.setExternalFeed(true);
                    m_audioAnalyzer.setWantsSystemAudio(true);
                    if (m_audioMixer.inputCount() == 0)
                        m_audioMixer.addInput("", "System Audio", false);
                    m_audioMixer.start();
                }
            }
            ImGui::EndCombo();
        }

        // Stereo level meter — sits immediately to the right of the audio combo.
        ImGui::SameLine(0, kGap);
        {
            ImVec2 mpos = ImGui::GetCursorScreenPos();
            float meterH = 14.0f;
            float meterY = mpos.y + (frameH - meterH) * 0.5f;
            float gap = 2.0f, singleH = (meterH - gap) * 0.5f;
            ImU32 trackBg = IM_COL32(20, 24, 35, 200);
            ImDrawList* d = ImGui::GetWindowDrawList();
            d->AddRectFilled(ImVec2(mpos.x, meterY),
                             ImVec2(mpos.x + kMeterW, meterY + singleH), trackBg, 2.0f);
            d->AddRectFilled(ImVec2(mpos.x, meterY + singleH + gap),
                             ImVec2(mpos.x + kMeterW, meterY + meterH), trackBg, 2.0f);
            float fillL = m_audioLevelSmoothL * kMeterW;
            float fillR = m_audioLevelSmoothR * kMeterW;
            d->AddRectFilled(ImVec2(mpos.x, meterY),
                             ImVec2(mpos.x + fillL, meterY + singleH), kAccentDim, 2.0f);
            d->AddRectFilled(ImVec2(mpos.x, meterY + singleH + gap),
                             ImVec2(mpos.x + fillR, meterY + meterH), kAccentDim, 2.0f);
            ImGui::Dummy(ImVec2(kMeterW, frameH));
        }

        // Right-anchored cluster: [Work Area click-to-edit] [REC] [GO LIVE].
        // REC sits next to the Work Area time so "this range → record it"
        // reads as one continuous action; GO LIVE tails the cluster at the
        // window's right edge.
        const char* goLiveLbl = m_rtmpOutput.isActive() ? "END LIVE" : "GO LIVE";
        float goLiveW = ImGui::CalcTextSize(goLiveLbl).x
                      + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(0, 0);
        // Right-edge margin matches the timeline WindowPadding on the left
        // (~12px) so REC / GO LIVE sit the same distance from the window edge
        // as play/stop/loop sit from their left edge.
        float rightAnchorX = contentMaxX - waW - kGap - kRecBtnW - kGap - goLiveW - 4.0f;
        if (rightAnchorX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightAnchorX);
        {
            ImVec2 wapos = ImGui::GetCursorScreenPos();
            bool waClicked = ImGui::InvisibleButton("##WAEdit",
                                                    ImVec2(waW, frameH));
            bool waHov = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(wapos.x, wapos.y + (frameH - ImGui::CalcTextSize(waLbl).y) * 0.5f),
                waHov ? IM_COL32(255, 255, 255, 255)
                      : IM_COL32(200, 210, 225, 200),
                waLbl);
            if (waHov) ImGui::SetTooltip(
                "Work Area — the range REC will capture.\n"
                "Click to edit start/end (or use I / O at playhead).");
            if (waClicked) ImGui::OpenPopup("##WAEditPopup");

            if (ImGui::BeginPopup("##WAEditPopup")) {
                ImGui::TextDisabled("Work Area (record range)");
                ImGui::Separator();
                static int s_sM = 0, s_sS = 0, s_eM = 0, s_eS = 0;
                if (ImGui::IsWindowAppearing()) {
                    s_sM = (int)wa0 / 60; s_sS = (int)wa0 % 60;
                    s_eM = (int)wa1 / 60; s_eS = (int)wa1 % 60;
                }
                ImGui::TextUnformatted("Start");
                ImGui::SameLine(70);
                ImGui::SetNextItemWidth(50);
                ImGui::InputInt("##sM", &s_sM, 0); ImGui::SameLine(); ImGui::TextUnformatted("m");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(50);
                ImGui::InputInt("##sS", &s_sS, 0); ImGui::SameLine(); ImGui::TextUnformatted("s");

                ImGui::TextUnformatted("End");
                ImGui::SameLine(70);
                ImGui::SetNextItemWidth(50);
                ImGui::InputInt("##eM", &s_eM, 0); ImGui::SameLine(); ImGui::TextUnformatted("m");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(50);
                ImGui::InputInt("##eS", &s_eS, 0); ImGui::SameLine(); ImGui::TextUnformatted("s");

                ImGui::Separator();
                if (ImGui::Button("Set", ImVec2(120, 0))) {
                    if (s_sM < 0) s_sM = 0; if (s_sS < 0) s_sS = 0; if (s_sS > 59) s_sS = 59;
                    if (s_eM < 0) s_eM = 0; if (s_eS < 0) s_eS = 0; if (s_eS > 59) s_eS = 59;
                    double start = s_sM * 60.0 + s_sS;
                    double end   = s_eM * 60.0 + s_eS;
                    double dur   = m_timeline.duration();
                    if (start < 0) start = 0;
                    if (end > dur) end = dur;
                    if (end < start + 0.1) end = start + 0.1;
                    m_timeline.setWorkArea(start, end);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset", ImVec2(-1, 0))) {
                    m_timeline.resetWorkArea();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // REC — red pill, anchored at the far right next to Work Area time.
        ImGui::SameLine(0, kGap);
        const char* recLabel = (recording || exporting) ? "STOP" : "REC";
        if (pillBtn("##Rec", recLabel, kRecBtnW, kRedDim, kRed)) {
            if (recording || exporting) {
                if (exporting) { m_timeline.pause(); m_timelineExporting = false; }
                if (recording) m_recorder.stop();
            } else {
                m_recorder.setAudioDevice(m_selectedAudioDevice);
                startTimelineExport();
            }
        }
        // GO LIVE sits immediately to the right of REC — both are "broadcast"
        // actions, so they cluster together at the tail of the transport row.
        ImGui::SameLine(0, kGap);
        renderGoLiveButton();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            (recording || exporting)
              ? "Click to stop recording."
              : "Click to record the Work Area to MP4.");
        if (recording) {
            float pulse = 0.5f + 0.5f * sinf(timeNow * 4.0f);
            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();
            ImDrawList* d = ImGui::GetWindowDrawList();
            d->AddCircleFilled(ImVec2(itemMin.x + 8.0f, itemMin.y + (itemMax.y - itemMin.y) * 0.5f),
                               3.0f, IM_COL32(255, 70, 70, (int)(pulse * 255)));
        }
#else
        // Non-FFmpeg builds: Work Area right-anchored only.
        {
            double wa0 = m_timeline.workAreaStart();
            double wa1 = m_timeline.workAreaEnd();
            int wm0 = (int)wa0 / 60, ws0 = (int)wa0 % 60;
            int wm1 = (int)wa1 / 60, ws1 = (int)wa1 % 60;
            char waLbl[48];
            snprintf(waLbl, sizeof(waLbl), "%d:%02d-%d:%02d", wm0, ws0, wm1, ws1);
            float waW = ImGui::CalcTextSize(waLbl).x;
            ImGui::SameLine();
            float targetX = ImGui::GetWindowContentRegionMax().x - 12.0f - waW;
            if (targetX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(targetX);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", waLbl);
        }
#endif
    }

    // Minimized — stop after the transport row. Panel stays docked so users
    // can drag the splitter to shrink it to just this strip.
    if (m_timelineMinimized) {
        ImGui::End();
        return;
    }

    ImGui::Dummy(ImVec2(0, 4));

  if (!s_tlCollapsed) {

    // --- Ruler + tracks area — AE-style with fixed left gutter for layer names ---
    // The gutter mirrors the Layer panel's row treatment: visibility dot, kind
    // swatch, and the layer name. Each track row's gutter cell stays pinned
    // while the clip area scrolls/zooms.
    const float gutterW = 160.0f;
    float availW = ImGui::GetContentRegionAvail().x;
    float trackAreaW = availW - gutterW;
    if (trackAreaW < 120.0f) trackAreaW = 120.0f;

    double duration = m_timeline.duration();
    double playhead = m_timeline.playhead();

    // Zoom / scroll: zoom is now driven by the transport-row slider (hoisted
    // to the top of this function). Zoom=1 shows the whole show; zoom=64 shows
    // 1/64 of it. Horizontal wheel pans; zoom is slider-only.
    double visibleDur = duration / s_tlZoom;
    if (s_tlScroll < 0.0) s_tlScroll = 0.0;
    if (s_tlScroll + visibleDur > duration) s_tlScroll = duration - visibleDur;
    if (s_tlScroll < 0.0) s_tlScroll = 0.0;

    // timeToX returns an offset from rulerOrigin.x (which lives past the gutter).
    auto timeToX = [&](double t) {
        return (float)((t - s_tlScroll) / visibleDur) * trackAreaW;
    };
    auto xToTime = [&](float x) {
        return s_tlScroll + (double)(x / trackAreaW) * visibleDur;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Ruler row — gutter holds a "Tracks" title; ruler sits in the clip area.
    ImVec2 fullOrigin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(availW, rulerH));
    ImVec2 rulerOrigin(fullOrigin.x + gutterW, fullOrigin.y);
    // Gutter header — small caps label so users read the column at a glance.
    {
        dl->AddRectFilled(fullOrigin,
                          ImVec2(fullOrigin.x + gutterW - 4, fullOrigin.y + rulerH),
                          IM_COL32(255, 255, 255, 4), 6.0f);
        dl->AddText(ImGui::GetFont(), 10.0f,
                    ImVec2(fullOrigin.x + 10, fullOrigin.y + 5),
                    IM_COL32(170, 180, 200, 200), "TRACKS");
    }
    dl->AddRectFilled(rulerOrigin, ImVec2(rulerOrigin.x + trackAreaW, rulerOrigin.y + rulerH),
                      IM_COL32(255, 255, 255, 6), 6.0f);
    // Adaptive tick interval: scales with the VISIBLE duration, so zooming in
    // reveals finer subdivisions (down to 1s, 0.5s, 0.1s).
    double majorInterval = 10.0;
    while (visibleDur / majorInterval > 18.0) majorInterval *= 2.0;
    while (visibleDur / majorInterval < 6.0 && majorInterval > 0.1) majorInterval *= 0.5;
    double firstTick = std::floor(s_tlScroll / majorInterval) * majorInterval;
    for (double t = firstTick; t <= s_tlScroll + visibleDur + 0.01; t += majorInterval) {
        if (t < 0) continue;
        float x = rulerOrigin.x + timeToX(t);
        if (x < rulerOrigin.x - 1 || x > rulerOrigin.x + trackAreaW + 1) continue;
        dl->AddLine(ImVec2(x, rulerOrigin.y + rulerH - 5),
                    ImVec2(x, rulerOrigin.y + rulerH),
                    IM_COL32(255, 255, 255, 110));
        char lbl[16];
        if (majorInterval < 1.0) snprintf(lbl, sizeof(lbl), "%d:%05.2f", (int)t / 60, std::fmod(t, 60.0));
        else                      snprintf(lbl, sizeof(lbl), "%d:%02d", (int)t / 60, ((int)t) % 60);
        dl->AddText(ImGui::GetFont(), 10.0f,
                    ImVec2(x + 4, rulerOrigin.y + 4),
                    IM_COL32(255, 255, 255, 150), lbl);
    }
    // ── Section bands — colored strips on the ruler for intro/drop/etc. ──
    {
        int si = 0;
        for (const auto& s : m_timeline.sections()) {
            float sx0 = rulerOrigin.x + timeToX(s.startTime);
            float sx1 = rulerOrigin.x + timeToX(s.endTime);
            if (sx1 <= rulerOrigin.x || sx0 >= rulerOrigin.x + trackAreaW) { si++; continue; }
            if (sx0 < rulerOrigin.x) sx0 = rulerOrigin.x;
            if (sx1 > rulerOrigin.x + trackAreaW) sx1 = rulerOrigin.x + trackAreaW;
            // Palette: cycle 5 calm hues tied to section id so the same
            // section keeps its color across edits.
            ImU32 palette[5] = {
                IM_COL32(120, 180, 230,  90),
                IM_COL32(200, 130, 230,  90),
                IM_COL32(230, 180, 120,  90),
                IM_COL32(140, 220, 170,  90),
                IM_COL32(230, 140, 180,  90),
            };
            ImU32 tint = s.tint != 0 ? (ImU32)s.tint : palette[s.id % 5];
            dl->AddRectFilled(ImVec2(sx0, rulerOrigin.y),
                              ImVec2(sx1, rulerOrigin.y + rulerH),
                              tint, 4.0f);
            if (!s.name.empty()) {
                dl->AddText(ImGui::GetFont(), 10.0f,
                            ImVec2(sx0 + 6, rulerOrigin.y + 2),
                            IM_COL32(240, 245, 250, 220), s.name.c_str());
            }
            si++;
        }
    }

    // ── Markers — vertical dots on the ruler. Click to jump. ─────────────
    {
        uint32_t toRemoveMarker = 0;
        for (const auto& mk : m_timeline.markers()) {
            float mx = rulerOrigin.x + timeToX(mk.time);
            if (mx < rulerOrigin.x - 4 || mx > rulerOrigin.x + trackAreaW + 4) continue;
            // Diamond glyph at the bottom of the ruler.
            float cy = rulerOrigin.y + rulerH - 3;
            dl->AddCircleFilled(ImVec2(mx, cy), 4.0f,
                                IM_COL32(255, 220, 120, 230));
            dl->AddLine(ImVec2(mx, rulerOrigin.y + 2),
                        ImVec2(mx, cy),
                        IM_COL32(255, 220, 120, 120), 1.0f);
            if (!mk.name.empty()) {
                dl->AddText(ImGui::GetFont(), 10.0f,
                            ImVec2(mx + 6, rulerOrigin.y + 4),
                            IM_COL32(255, 230, 180, 220), mk.name.c_str());
            }
            // Click → seek playhead to marker; shift+click → delete.
            ImVec2 hitMin(mx - 6, rulerOrigin.y + rulerH - 9);
            ImGui::PushID((int)(mk.id + 0x90000000));
            ImGui::SetCursorScreenPos(hitMin);
            if (ImGui::InvisibleButton("##mk", ImVec2(12, 12))) {
                if (ImGui::GetIO().KeyShift) toRemoveMarker = mk.id;
                else                          m_timeline.seek(mk.time);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s  (click to jump, shift-click to delete)",
                                  mk.name.c_str());
            }
            ImGui::PopID();
        }
        if (toRemoveMarker) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
            m_timeline.removeMarker(toRemoveMarker);
        }
    }

    // ── Work Area band — visible subset that Export will render. ─────────
    // Draw the band BEHIND the ruler invisible button so drags on the band
    // itself are handled below (they would otherwise hit the ruler-seek path).
    double waStart = m_timeline.workAreaStart();
    double waEnd   = m_timeline.workAreaEnd();
    float  waX0    = rulerOrigin.x + timeToX(waStart);
    float  waX1    = rulerOrigin.x + timeToX(waEnd);
    if (waX1 - waX0 >= 2.0f) {
        // Translucent indigo band spanning the ruler + some pixels below.
        ImU32 waFill = IM_COL32(92, 104, 210, 40);
        ImU32 waEdge = IM_COL32(128, 140, 240, 220);
        dl->AddRectFilled(ImVec2(waX0, rulerOrigin.y),
                          ImVec2(waX1, rulerOrigin.y + rulerH),
                          waFill, 4.0f);
        // Bracket marks at each end.
        dl->AddRectFilled(ImVec2(waX0, rulerOrigin.y),
                          ImVec2(waX0 + 2, rulerOrigin.y + rulerH), waEdge);
        dl->AddRectFilled(ImVec2(waX1 - 2, rulerOrigin.y),
                          ImVec2(waX1, rulerOrigin.y + rulerH), waEdge);
    }

    ImGui::SetCursorScreenPos(rulerOrigin);
    ImGui::InvisibleButton("##TL_Ruler", ImVec2(trackAreaW, rulerH));
    bool rulerHover = ImGui::IsItemHovered();

    // Work Area drag: click within 6px of either edge to drag that edge.
    // Keeps the ruler-seek behavior outside the edge hot-zones.
    static int waDrag = 0; // 0 none, 1 drag start, 2 drag end
    float mxRel = ImGui::GetIO().MousePos.x - rulerOrigin.x;
    bool overWAStart = std::abs(mxRel - timeToX(waStart)) < 6.0f;
    bool overWAEnd   = std::abs(mxRel - timeToX(waEnd))   < 6.0f;
    if (rulerHover && (overWAStart || overWAEnd)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemClicked() && (overWAStart || overWAEnd)) {
        waDrag = overWAStart ? 1 : 2;
    }
    if (waDrag != 0 && ImGui::IsMouseDown(0)) {
        double t = xToTime(mxRel);
        if (t < 0.0) t = 0.0;
        if (t > m_timeline.duration()) t = m_timeline.duration();
        if (waDrag == 1) m_timeline.setWorkArea(t, m_timeline.workAreaEnd());
        else             m_timeline.setWorkArea(m_timeline.workAreaStart(), t);
    } else if (waDrag != 0 && !ImGui::IsMouseDown(0)) {
        waDrag = 0;
    }

    // Ruler-seek: only when NOT dragging a work-area edge.
    if (waDrag == 0) {
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
            m_timeline.seek(xToTime(mxRel));
        } else if (ImGui::IsItemClicked()) {
            m_timeline.seek(xToTime(mxRel));
        }
    }
    // Double-click ruler to reset zoom
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        s_tlZoom = 1.0f;
        s_tlScroll = 0.0;
    }

    // Right-click ruler → section / marker create menu. `s_rulerMenuTime`
    // captures the mouse time at open so the menu's "Add Section Here" and
    // "Add Marker Here" resolve relative to where the click landed.
    static double s_rulerMenuTime = 0.0;
    static uint32_t s_sectionEditId = 0;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
        s_rulerMenuTime = xToTime(mxRel);
        ImGui::OpenPopup("##RulerMenu");
    }
    if (ImGui::BeginPopup("##RulerMenu")) {
        int mm = (int)s_rulerMenuTime / 60, ss = (int)s_rulerMenuTime % 60;
        ImGui::TextDisabled("At %d:%02d", mm, ss);
        ImGui::Separator();
        if (ImGui::MenuItem("Add Section Here")) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
            double start = s_rulerMenuTime;
            double end   = std::min(m_timeline.duration(), start + 8.0);
            uint32_t sid = m_timeline.addSection(start, end, "Section");
            s_sectionEditId = sid;
        }
        if (ImGui::MenuItem("Add Section from Work Area")) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
            uint32_t sid = m_timeline.addSection(m_timeline.workAreaStart(),
                                                 m_timeline.workAreaEnd(),
                                                 "Section");
            s_sectionEditId = sid;
        }
        if (ImGui::MenuItem("Add Marker Here")) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
            m_timeline.addMarker(s_rulerMenuTime, "Cue");
        }
        if (!m_timeline.sections().empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Sections");
            uint32_t toRemove = 0;
            for (const auto& s : m_timeline.sections()) {
                ImGui::PushID((int)(s.id + 0x80000000));
                char buf[128];
                snprintf(buf, sizeof(buf), "%s  [%d:%02d-%d:%02d]",
                         s.name.empty() ? "Section" : s.name.c_str(),
                         (int)s.startTime / 60, (int)s.startTime % 60,
                         (int)s.endTime   / 60, (int)s.endTime   % 60);
                if (ImGui::MenuItem(buf)) {
                    m_timeline.seek(s.startTime);
                    s_sectionEditId = s.id;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) toRemove = s.id;
                ImGui::PopID();
            }
            if (toRemove) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                m_timeline.removeSection(toRemove);
            }
        }
        ImGui::EndPopup();
    }

    // ── Section rename / time editor ────────────────────────────────────
    // Opens right after "Add Section Here" (or via shift-click on a band
    // in the ruler) so users can name the act and nudge its bounds.
    if (s_sectionEditId != 0) {
        if (auto* sec = m_timeline.findSection(s_sectionEditId)) {
            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
            if (ImGui::Begin("##SectionEdit", nullptr,
                             ImGuiWindowFlags_NoDocking
                             | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextDisabled("Section");
                static char nameBuf[128] = {};
                static uint32_t lastId = 0;
                if (lastId != s_sectionEditId) {
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s", sec->name.c_str());
                    lastId = s_sectionEditId;
                }
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                    sec->name = nameBuf;
                }
                float start = (float)sec->startTime;
                float end   = (float)sec->endTime;
                ImGui::SetNextItemWidth(120);
                if (ImGui::DragFloat("Start", &start, 0.1f, 0.0f,
                                      (float)m_timeline.duration(), "%.1fs")) {
                    if (start < 0) start = 0;
                    if (start >= end - 0.1f) start = end - 0.1f;
                    sec->startTime = start;
                }
                ImGui::SetNextItemWidth(120);
                if (ImGui::DragFloat("End", &end, 0.1f, 0.0f,
                                      (float)m_timeline.duration(), "%.1fs")) {
                    if (end <= start + 0.1f) end = start + 0.1f;
                    if (end > m_timeline.duration()) end = (float)m_timeline.duration();
                    sec->endTime = end;
                }
                if (ImGui::Button("Done")) s_sectionEditId = 0;
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                if (ImGui::Button("Delete")) {
                    m_timeline.removeSection(s_sectionEditId);
                    s_sectionEditId = 0;
                }
                ImGui::PopStyleColor();
            }
            ImGui::End();
        } else {
            s_sectionEditId = 0;
        }
    }

    // Mouse wheel over the timeline = horizontal pan. Zoom is slider-only now.
    {
        float wheel = ImGui::GetIO().MouseWheel;
        bool hoverTL = rulerHover || ImGui::IsWindowHovered(
                            ImGuiHoveredFlags_RootAndChildWindows |
                            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (wheel != 0.0f && hoverTL) {
            s_tlScroll -= wheel * visibleDur * 0.08;
            if (s_tlScroll < 0.0) s_tlScroll = 0.0;
            if (s_tlScroll + visibleDur > duration) s_tlScroll = duration - visibleDur;
        }
    }

    // Vertical grid lines spanning the track area (AE-style rhythm).
    // Drawn after tracks so they overlay clip fills faintly — handled in track loop via the same xs.
    ImGui::Dummy(ImVec2(0, 4));

    // Clip drag state — track → clip id + drag mode (0 move, 1 left trim, 2 right trim)
    static uint32_t dragLayerId = 0, dragClipId = 0;
    static int dragMode = 0;
    static double dragStartTime = 0, dragStartDur = 0;
    static ImVec2 dragAnchor(0, 0);

    // Clip context menu state — set on right-click, consumed when popup renders.
    // `s_ctxClipId` doubles as "current clip selection" (set on left-click too).
    // `s_trPickerId` is the transition whose effect-picker popup should open
    // on the next frame (set when the user double-clicks a transition bar).
    static uint32_t s_ctxLayerId = 0, s_ctxClipId = 0;
    static uint32_t s_trPickerId = 0;
    static ImVec2   s_trPickerPos(0, 0);

    // Multi-select set — keyed by (layerId<<32 | clipId). Sibling to s_ctxClipId:
    // s_ctxClipId is the "primary" selection (what the inspector edits), and
    // s_multiSel extends that for batch drag/delete.
    static std::set<uint64_t> s_multiSel;
    auto selKey = [](uint32_t layerId, uint32_t clipId) -> uint64_t {
        return ((uint64_t)layerId << 32) | clipId;
    };
    // Per-selected-clip original start times — captured at drag start, so the
    // drag delta applies consistently to every selected clip.
    static std::map<uint64_t, double> s_dragStartOffsets;

    // --- Track rows (pill-shaped, flush-left, AE-style) ---
    const float trackSpacing = 4.0f;
    const float trimZone     = 8.0f; // edge hot-zone width in pixels

    // Build a layerId → groupId map so the loop below knows when to emit a
    // group header row. Also collects the group bar's time-range (union of
    // all member clips) so the header renders as a real draggable summary.
    struct GroupHeaderInfo {
        uint32_t groupId = 0;
        double start = 0.0, end = 0.0;
        std::string name;
    };
    auto layerGroupId = [&](uint32_t layerId) -> uint32_t {
        for (int i = 0; i < m_layerStack.count(); i++) {
            auto l = m_layerStack[i];
            if (l && l->id == layerId) return l->groupId;
        }
        return 0;
    };

    uint32_t lastGroupId = 0;
    for (auto& track : m_timeline.tracks()) {
        // Emit a group header row when we cross into a new non-zero group.
        uint32_t gid = layerGroupId(track.layerId);
        if (gid != 0 && gid != lastGroupId) {
            // Compute time-range of all tracks in this group.
            double gStart = 1e18, gEnd = 0.0;
            std::string gName = "Group";
            if (auto* grp = m_layerStack.groups().count(gid)
                          ? &m_layerStack.groups().at(gid) : nullptr) {
                gName = grp->name;
            }
            for (auto& gt : m_timeline.tracks()) {
                if (layerGroupId(gt.layerId) != gid) continue;
                for (const auto& c : gt.clips) {
                    if (c.startTime < gStart) gStart = c.startTime;
                    if (c.endTime() > gEnd)   gEnd   = c.endTime();
                }
            }
            if (gEnd > gStart) {
                const float ghH = 20.0f;
                ImVec2 ghOrigin = ImGui::GetCursorScreenPos();
                ImVec2 ghTrackOrigin(ghOrigin.x + gutterW, ghOrigin.y);
                ImGui::Dummy(ImVec2(gutterW + trackAreaW, ghH));

                // Header background — slightly brighter than tracks.
                dl->AddRectFilled(ghOrigin, ImVec2(ghOrigin.x + gutterW - 4,
                                                   ghOrigin.y + ghH),
                                  IM_COL32(255, 255, 255, 14), 6.0f);
                dl->AddRectFilled(ghTrackOrigin,
                                  ImVec2(ghTrackOrigin.x + trackAreaW,
                                         ghOrigin.y + ghH),
                                  IM_COL32(255, 255, 255, 10), 6.0f);

                // Chevron + group name in the gutter.
                dl->AddTriangleFilled(ImVec2(ghOrigin.x + 12, ghOrigin.y + 6),
                                      ImVec2(ghOrigin.x + 20, ghOrigin.y + 6),
                                      ImVec2(ghOrigin.x + 16, ghOrigin.y + ghH - 6),
                                      IM_COL32(200, 210, 230, 220));
                dl->AddText(ImVec2(ghOrigin.x + 28, ghOrigin.y + (ghH - 14.0f) * 0.5f),
                            IM_COL32(230, 235, 245, 240),
                            gName.empty() ? "Group" : gName.c_str());

                // Union bar in the clip area — thin pill that spans the group.
                float gx0 = ghTrackOrigin.x + timeToX(gStart);
                float gx1 = ghTrackOrigin.x + timeToX(gEnd);
                if (gx1 - gx0 < 2.0f) gx1 = gx0 + 2.0f;
                dl->AddRectFilled(ImVec2(gx0, ghOrigin.y + 4),
                                  ImVec2(gx1, ghOrigin.y + ghH - 4),
                                  IM_COL32(255, 255, 255, 28), 4.0f);
                dl->AddRect(ImVec2(gx0, ghOrigin.y + 4),
                            ImVec2(gx1, ghOrigin.y + ghH - 4),
                            IM_COL32(255, 255, 255, 90), 4.0f, 0, 1.0f);

                ImGui::Dummy(ImVec2(0, 2));
            }
        }
        lastGroupId = gid;

        ImGui::PushID((int)track.layerId);
        ImVec2 rowOrigin  = ImGui::GetCursorScreenPos();   // full-width left edge (gutter start)
        ImVec2 trackOrigin(rowOrigin.x + gutterW, rowOrigin.y); // clip area start
        float rowY = rowOrigin.y;

        // Full-width invisible hit area for hover/drag in the clip lane.
        // SetNextItemAllowOverlap lets the gutter's visibility dot + layer-name
        // buttons (drawn later, on top) still receive clicks even though this
        // button covers the gutter region.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##track", ImVec2(gutterW + trackAreaW, trackH));

        // Track pill — only over the clip area; the gutter gets its own treatment.
        dl->AddRectFilled(trackOrigin,
                          ImVec2(trackOrigin.x + trackAreaW, rowY + trackH),
                          IM_COL32(255, 255, 255, 10), 8.0f);

        // Faint vertical grid synced to ruler majors (rhythm guides)
        for (double t = majorInterval; t < duration; t += majorInterval) {
            float gx = trackOrigin.x + timeToX(t);
            dl->AddLine(ImVec2(gx, rowY + 4),
                        ImVec2(gx, rowY + trackH - 4),
                        IM_COL32(255, 255, 255, 14));
        }

        // Empty-track hint — only shown for the rare case where every clip on
        // a track has been deleted. Layers always auto-create a default bar.
        if (track.clips.empty()) {
            dl->AddText(ImGui::GetFont(), 11.0f,
                        ImVec2(trackOrigin.x + 12,
                               rowY + (trackH - 11.0f) * 0.5f),
                        IM_COL32(255, 255, 255, 70), "click-and-drag to add a clip");
        }

        // Clips — pills with rounded corners; hover shows trim handles.
        // Color coding: video=indigo, shader=purple, color=gray — derived from
        // clip.kind, or auto-sniffed from sourcePath extension when kind=Auto.
        auto resolveKind = [](const TimelineClip& c) -> ClipKind {
            if (c.kind != ClipKind::Auto) return c.kind;
            if (c.sourcePath.empty()) return ClipKind::Shader; // a bare placeholder acts like a shader slot
            auto dot = c.sourcePath.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : c.sourcePath.substr(dot);
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            if (ext == ".fs" || ext == ".frag" || ext == ".isf") return ClipKind::Shader;
            return ClipKind::Video;
        };
        auto kindFill = [](ClipKind k, bool selected, bool hover) -> ImU32 {
            int boost = selected ? 90 : hover ? 60 : 38;
            switch (k) {
                case ClipKind::Video:  return IM_COL32( 92, 116, 220, boost + 40);
                case ClipKind::Shader: return IM_COL32(150,  96, 210, boost + 40);
                case ClipKind::Color:  return IM_COL32(130, 140, 150, boost + 30);
                default:               return IM_COL32(200, 200, 210, boost + 30);
            }
        };
        auto kindEdge = [](ClipKind k, bool selected) -> ImU32 {
            int a = selected ? 230 : 140;
            switch (k) {
                case ClipKind::Video:  return IM_COL32(130, 156, 255, a);
                case ClipKind::Shader: return IM_COL32(190, 130, 250, a);
                case ClipKind::Color:  return IM_COL32(180, 188, 198, a);
                default:               return IM_COL32(255, 255, 255, a);
            }
        };
        auto basename = [](const std::string& p) -> std::string {
            if (p.empty()) return {};
            auto slash = p.find_last_of("/\\");
            return (slash == std::string::npos) ? p : p.substr(slash + 1);
        };

        float mx = ImGui::GetIO().MousePos.x;
        float my = ImGui::GetIO().MousePos.y;
        for (auto& clip : track.clips) {
            float x0 = trackOrigin.x + timeToX(clip.startTime);
            float x1 = trackOrigin.x + timeToX(clip.endTime());
            if (x1 - x0 < 2.0f) x1 = x0 + 2.0f;
            ImVec2 a(x0, rowY + 4), b(x1, rowY + trackH - 4);
            bool selected = (dragLayerId == track.layerId && dragClipId == clip.id);
            bool hover    = (mx >= x0 && mx <= x1 && my >= a.y && my <= b.y && ImGui::IsItemHovered());
            ClipKind k    = resolveKind(clip);
            bool inMultiSel = s_multiSel.count(selKey(track.layerId, clip.id)) > 0;
            ImU32 fill    = (clip.tint != 0) ? (ImU32)clip.tint : kindFill(k, selected || inMultiSel, hover);
            ImU32 border  = (selected || inMultiSel)
                          ? IM_COL32(255, 220, 110, 235)
                          : kindEdge(k, false);
            dl->AddRectFilled(a, b, fill, 6.0f);

            // ── Live thumbnail for video/image clips currently under the
            //    playhead. Draws the active layer's source texture with its
            //    flip honored, tinted with the fill alpha so it never drowns
            //    out the label. Shader clips skip this path (the source has
            //    no "frame" per se).
            if (k == ClipKind::Video && x1 - x0 > 30.0f) {
                Layer* live = nullptr;
                for (int i = 0; i < m_layerStack.count(); i++) {
                    auto l = m_layerStack[i];
                    if (l && l->id == track.layerId) { live = l.get(); break; }
                }
                if (live && live->source && live->source->textureId() != 0
                    && clip.contains(m_timeline.playhead()))
                {
                    bool flipV = live->source->isFlippedV();
                    ImVec2 uv0 = flipV ? ImVec2(0,1) : ImVec2(0,0);
                    ImVec2 uv1 = flipV ? ImVec2(1,0) : ImVec2(1,1);
                    dl->PushClipRect(a, b, true);
                    dl->AddImage((ImTextureID)(intptr_t)live->source->textureId(),
                                 a, b, uv0, uv1, IM_COL32(255, 255, 255, 160));
                    dl->PopClipRect();
                }
            }

            // ── Stylized audio visualization — a horizontal capsule pattern
            //    for audio-extension clips so they read as "audio" at a
            //    glance. Honest about not being real peaks; we draw a thin
            //    envelope anchored to the clip rect so it looks like a
            //    waveform placeholder without claiming to have decoded the
            //    file.
            {
                auto lastDot = clip.sourcePath.find_last_of('.');
                std::string ext = (lastDot == std::string::npos)
                                  ? "" : clip.sourcePath.substr(lastDot);
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                bool isAudio = (ext == ".wav" || ext == ".mp3"
                             || ext == ".m4a" || ext == ".flac"
                             || ext == ".aiff" || ext == ".aif"
                             || ext == ".ogg");
                if (isAudio && x1 - x0 > 30.0f) {
                    float midY = (a.y + b.y) * 0.5f;
                    float h    = (b.y - a.y) * 0.35f;
                    // Deterministic amplitude from a cheap hash of path+id
                    // so the same clip always looks the same across frames.
                    auto hashAt = [&](int i) -> float {
                        uint32_t s = clip.id * 2654435761u + (uint32_t)i * 374761393u;
                        s = (s ^ (s >> 15)) * 2246822519u;
                        s = (s ^ (s >> 13)) * 3266489917u;
                        s ^= s >> 16;
                        return (s & 0xFFFF) / 65535.0f;
                    };
                    int buckets = (int)((x1 - x0) / 3.0f);
                    if (buckets > 240) buckets = 240;
                    dl->PushClipRect(a, b, true);
                    for (int i = 0; i < buckets; i++) {
                        float u  = (float)i / buckets;
                        float px = x0 + u * (x1 - x0);
                        float amp = h * (0.35f + 0.65f * hashAt(i));
                        dl->AddLine(ImVec2(px, midY - amp),
                                    ImVec2(px, midY + amp),
                                    IM_COL32(255, 255, 255, 90), 1.2f);
                    }
                    dl->PopClipRect();
                }
            }

            dl->AddRect(a, b, border, 6.0f, 0, 1.0f);

            // Label: prefer filename (the "what will this clip play?" question),
            // fall back to clip.name, then to duration. Width-aware clipping so
            // tiny clips still show something.
            std::string src = basename(clip.sourcePath);
            const char* primary = !src.empty() ? src.c_str()
                                               : (!clip.name.empty() ? clip.name.c_str() : "clip");
            float clipPxW = x1 - x0;
            char dlbl[96];
            if (clipPxW < 40.0f)        snprintf(dlbl, sizeof(dlbl), "%.0fs", clip.duration);
            else if (clipPxW < 110.0f)  snprintf(dlbl, sizeof(dlbl), "%s", primary);
            else                         snprintf(dlbl, sizeof(dlbl), "%s · %.1fs", primary, clip.duration);
            dl->PushClipRect(a, b, true);
            dl->AddText(ImGui::GetFont(), 11.0f,
                        ImVec2(x0 + 8, rowY + 10),
                        IM_COL32(255, 255, 255, 245), dlbl);
            dl->PopClipRect();

            // Visible trim handles on hover (discoverability for resize)
            if (hover && dragClipId == 0) {
                bool onLeft  = (mx < x0 + trimZone);
                bool onRight = (mx > x1 - trimZone);
                if (onLeft) {
                    dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x0 + 3, b.y),
                                      IM_COL32(255, 220, 80, 220), 2.0f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                } else if (onRight) {
                    dl->AddRectFilled(ImVec2(x1 - 3, a.y), ImVec2(x1, b.y),
                                      IM_COL32(255, 220, 80, 220), 2.0f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }

            // Begin drag on click within this clip rect — also select the clip
            // so the inline inspector strip appears below the tracks. Shift /
            // Cmd / Ctrl toggles membership in the multi-select set.
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0) && dragClipId == 0
                && mx >= x0 && mx <= x1 && my >= a.y && my <= b.y)
            {
                int mode = 0;
                if (mx < x0 + trimZone) mode = 1;
                else if (mx > x1 - trimZone) mode = 2;
                // Snapshot timeline BEFORE the drag so Cmd+Z restores the
                // pre-drag clip position/size.
                m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                dragLayerId = track.layerId;
                dragClipId = clip.id;
                dragMode = mode;
                dragStartTime = clip.startTime;
                dragStartDur = clip.duration;
                dragAnchor = ImGui::GetIO().MousePos;
                s_ctxLayerId = track.layerId;
                s_ctxClipId  = clip.id;

                bool additive = ImGui::GetIO().KeyShift
                             || ImGui::GetIO().KeySuper
                             || ImGui::GetIO().KeyCtrl;
                uint64_t k = selKey(track.layerId, clip.id);
                if (additive) {
                    if (s_multiSel.count(k)) s_multiSel.erase(k);
                    else                     s_multiSel.insert(k);
                } else if (!s_multiSel.count(k)) {
                    // Plain click on an unselected clip → exclusive selection.
                    s_multiSel.clear();
                    s_multiSel.insert(k);
                }

                // Capture every selected clip's startTime for batch-move delta.
                s_dragStartOffsets.clear();
                if (mode == 0) {
                    for (uint64_t sk : s_multiSel) {
                        uint32_t lid = (uint32_t)(sk >> 32);
                        uint32_t cid = (uint32_t)(sk & 0xFFFFFFFFu);
                        if (auto* c = m_timeline.findClip(lid, cid)) {
                            s_dragStartOffsets[sk] = c->startTime;
                        }
                    }
                }
            }

        }

        // Left-click-drag on empty track area (not over any existing clip) →
        // create a new clip spanning the drag. This is the "click and drag the
        // desired layer time" gesture — the sole creation path now that right-
        // click interactions are disabled.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0) && dragClipId == 0
            && mx >= trackOrigin.x && my >= rowY && my <= rowY + trackH)
        {
            float relX = mx - trackOrigin.x;
            double t = xToTime(relX);
            bool onExistingClip = false;
            for (const auto& c : track.clips) {
                if (t >= c.startTime && t < c.endTime()) { onExistingClip = true; break; }
            }
            if (!onExistingClip) {
                auto* nc = m_timeline.addClip(track.layerId, t, 0.1, track.name);
                if (nc) {
                    // Enter right-trim drag immediately so the user's drag sizes the new clip.
                    dragLayerId   = track.layerId;
                    dragClipId    = nc->id;
                    dragMode      = 2;                          // right trim
                    dragStartTime = nc->startTime;
                    dragStartDur  = nc->duration;
                    dragAnchor    = ImGui::GetIO().MousePos;
                }
            }
        }

        // Gutter cell — layer hierarchy row: [visibility dot] [kind swatch] name
        // Mirrors the Layer panel's row treatment so users read the column as
        // "these are my layers, in stacking order."
        {
            // Kind swatch colour — derive from any clip on this track, else fall
            // back to the live layer source type. Gives users a consistent
            // "this track is a video track" / "this is a shader track" signal.
            ClipKind swatchKind = ClipKind::Shader;
            if (!track.clips.empty()) swatchKind = resolveKind(track.clips.front());
            // Find the live layer to mirror its visibility.
            Layer* layerForRow = nullptr;
            for (int i = 0; i < m_layerStack.count(); i++) {
                auto l = m_layerStack[i];
                if (l && l->id == track.layerId) { layerForRow = l.get(); break; }
            }
            bool visible = layerForRow ? layerForRow->visible : true;

            // Subtle gutter-cell bg — no border, just a hairline right divider
            // against the track clip area.
            dl->AddRectFilled(ImVec2(rowOrigin.x, rowY + 2),
                              ImVec2(rowOrigin.x + gutterW - 4, rowY + trackH - 2),
                              IM_COL32(255, 255, 255, 4), 6.0f);
            dl->AddLine(ImVec2(rowOrigin.x + gutterW - 2, rowY + 4),
                        ImVec2(rowOrigin.x + gutterW - 2, rowY + trackH - 4),
                        IM_COL32(255, 255, 255, 18), 1.0f);

            // Visibility dot — clickable, toggles layer->visible.
            // AllowOverlap lets this button receive clicks despite the
            // full-row ##track InvisibleButton drawn earlier.
            float dotCx = rowOrigin.x + 14;
            float dotCy = rowY + trackH * 0.5f;
            ImU32 dotCol = visible ? IM_COL32(230, 235, 245, 240)
                                   : IM_COL32(120, 130, 140, 140);
            dl->AddCircleFilled(ImVec2(dotCx, dotCy), 4.5f, dotCol);
            ImGui::SetCursorScreenPos(ImVec2(dotCx - 8, dotCy - 8));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##vis", ImVec2(16, 16))) {
                if (layerForRow) layerForRow->visible = !layerForRow->visible;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                visible ? "Layer visible — click to hide"
                        : "Layer hidden — click to show");

            // Kind swatch — small rounded square tinted by ClipKind.
            ImU32 swatch = (swatchKind == ClipKind::Video)  ? IM_COL32(130, 156, 255, 220)
                         : (swatchKind == ClipKind::Shader) ? IM_COL32(190, 130, 250, 220)
                                                             : IM_COL32(180, 188, 198, 220);
            dl->AddRectFilled(ImVec2(rowOrigin.x + 28, rowY + trackH * 0.5f - 5),
                              ImVec2(rowOrigin.x + 38, rowY + trackH * 0.5f + 5),
                              swatch, 2.5f);

            // Layer name — truncated to fit the gutter.
            dl->PushClipRect(ImVec2(rowOrigin.x + 44, rowY),
                             ImVec2(rowOrigin.x + gutterW - 8, rowY + trackH),
                             true);
            dl->AddText(ImVec2(rowOrigin.x + 44, rowY + (trackH - 14.0f) * 0.5f),
                        visible ? IM_COL32(230, 235, 245, 255)
                                : IM_COL32(150, 158, 170, 200),
                        track.name.empty() ? "Layer" : track.name.c_str());
            dl->PopClipRect();

            // Clickable name region — selects this layer in the app so the
            // Property panel shows its knobs. Drawn after the ##track button
            // so hits land here first (AllowOverlap makes the outer ##track
            // transparent to this).
            ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 24, rowY + 4));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##layerPick",
                                       ImVec2(gutterW - 28, trackH - 8))) {
                // Find the layer's index in the stack so PropertyPanel picks it up.
                for (int i = 0; i < m_layerStack.count(); i++) {
                    auto l = m_layerStack[i];
                    if (l && l->id == track.layerId) { m_selectedLayer = i; break; }
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Click to edit layer properties");
        }

        // (Right-click-to-add-clip removed — layers auto-create their bar when
        // they appear in the stack. Per-clip context menu on existing bars
        // still opens on right-click via the clip-hover block above.)

        // ── Sublanes (automation / MIDI / audio-reactive) ─────────────────
        // One thin row per TimelineLane bound to this track. Renders placeholder
        // keyframe dots for automation, note blocks for MIDI, and a level
        // histogram for audio-reactive bindings — none of these drive runtime
        // yet (data-model stubs), but the row lets users plant points and
        // structure their show.
        {
            uint32_t toRemoveLane = 0;
            for (auto& ln : m_timeline.lanes()) {
                if (ln.layerId != track.layerId) continue;
                const float lnH = 18.0f;
                ImVec2 lnO = ImGui::GetCursorScreenPos();
                ImVec2 lnClip(lnO.x + gutterW, lnO.y);
                ImGui::Dummy(ImVec2(gutterW + trackAreaW, lnH));

                // Background + gutter label
                dl->AddRectFilled(ImVec2(lnO.x, lnO.y + 1),
                                  ImVec2(lnO.x + gutterW - 4, lnO.y + lnH - 1),
                                  IM_COL32(255, 255, 255, 6), 4.0f);
                dl->AddRectFilled(lnClip,
                                  ImVec2(lnClip.x + trackAreaW, lnO.y + lnH),
                                  IM_COL32(255, 255, 255, 6), 4.0f);
                ImU32 kindCol;
                switch (ln.kind) {
                    case TimelineLaneKind::Automation:    kindCol = IM_COL32(130, 200, 255, 200); break;
                    case TimelineLaneKind::MIDI:          kindCol = IM_COL32(255, 180, 120, 200); break;
                    case TimelineLaneKind::AudioReactive: kindCol = IM_COL32(130, 230, 170, 200); break;
                    default:                              kindCol = IM_COL32(220, 220, 220, 200); break;
                }
                dl->AddRectFilled(ImVec2(lnO.x + 18, lnO.y + lnH * 0.5f - 3),
                                  ImVec2(lnO.x + 24, lnO.y + lnH * 0.5f + 3),
                                  kindCol, 1.5f);
                char lblBuf[96];
                snprintf(lblBuf, sizeof(lblBuf), "%s  %s",
                         timelineLaneKindName(ln.kind),
                         ln.paramName.empty() ? "(param)" : ln.paramName.c_str());
                dl->AddText(ImGui::GetFont(), 10.0f,
                            ImVec2(lnO.x + 30, lnO.y + (lnH - 10.0f) * 0.5f),
                            IM_COL32(220, 230, 245, 230), lblBuf);

                // Per-kind body render. Double-click empty area adds a point.
                ImGui::PushID((int)(ln.id + 0x70000000));
                ImGui::SetCursorScreenPos(lnClip);
                ImGui::InvisibleButton("##ln", ImVec2(trackAreaW, lnH));
                bool laneHover = ImGui::IsItemHovered();
                if (laneHover && ImGui::IsMouseDoubleClicked(0)) {
                    TimelineLanePoint p;
                    p.time  = xToTime(ImGui::GetIO().MousePos.x - lnClip.x);
                    p.value = 0.5f;
                    ln.points.push_back(p);
                }
                if (laneHover && ImGui::IsMouseClicked(1)) {
                    ImGui::OpenPopup("##LaneMenu");
                }
                if (ImGui::BeginPopup("##LaneMenu")) {
                    if (ImGui::MenuItem("Clear points")) ln.points.clear();
                    if (ImGui::MenuItem("Remove lane"))   toRemoveLane = ln.id;
                    ImGui::EndPopup();
                }

                if (ln.kind == TimelineLaneKind::Automation) {
                    // Draw points as dots and lines between them. Sort by time
                    // for the polyline connection.
                    std::sort(ln.points.begin(), ln.points.end(),
                              [](const TimelineLanePoint& a, const TimelineLanePoint& b){
                                  return a.time < b.time;
                              });
                    for (size_t i = 0; i + 1 < ln.points.size(); i++) {
                        const auto& p0 = ln.points[i];
                        const auto& p1 = ln.points[i + 1];
                        float x0 = lnClip.x + timeToX(p0.time);
                        float x1 = lnClip.x + timeToX(p1.time);
                        float y0 = lnO.y + lnH - 2.0f - p0.value * (lnH - 4.0f);
                        float y1 = lnO.y + lnH - 2.0f - p1.value * (lnH - 4.0f);
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), kindCol, 1.2f);
                    }
                    for (const auto& p : ln.points) {
                        float px = lnClip.x + timeToX(p.time);
                        float py = lnO.y + lnH - 2.0f - p.value * (lnH - 4.0f);
                        dl->AddCircleFilled(ImVec2(px, py), 3.0f, kindCol);
                    }
                } else if (ln.kind == TimelineLaneKind::MIDI) {
                    // Each point = a small note block at its time, 0.5s wide.
                    for (const auto& p : ln.points) {
                        float px0 = lnClip.x + timeToX(p.time);
                        float px1 = lnClip.x + timeToX(p.time + 0.5);
                        dl->AddRectFilled(ImVec2(px0, lnO.y + 3),
                                          ImVec2(px1, lnO.y + lnH - 3),
                                          kindCol, 2.0f);
                    }
                } else if (ln.kind == TimelineLaneKind::AudioReactive) {
                    // Dashed horizontal band — visual note that this binds to
                    // live audio signal rather than stored keyframes.
                    for (float x = lnClip.x; x < lnClip.x + trackAreaW; x += 8) {
                        dl->AddLine(ImVec2(x,     lnO.y + lnH * 0.5f),
                                    ImVec2(x + 4, lnO.y + lnH * 0.5f),
                                    kindCol, 1.4f);
                    }
                }
                ImGui::PopID();
            }
            if (toRemoveLane) m_timeline.removeLane(toRemoveLane);
        }

        // ── "+ Lane" row — one-shot button that offers the three lane kinds.
        {
            ImGui::PushID((int)(track.layerId + 0x60000000));
            ImVec2 rO = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(gutterW, 14));
            ImGui::SetCursorScreenPos(ImVec2(rO.x + 20, rO.y));
            if (ImGui::SmallButton("+ Lane")) {
                ImGui::OpenPopup("##AddLaneMenu");
            }
            if (ImGui::BeginPopup("##AddLaneMenu")) {
                if (ImGui::MenuItem("Automation (opacity)")) {
                    m_timeline.addLane(track.layerId,
                                        TimelineLaneKind::Automation, "opacity");
                }
                if (ImGui::MenuItem("MIDI")) {
                    m_timeline.addLane(track.layerId, TimelineLaneKind::MIDI, "CC1");
                }
                if (ImGui::MenuItem("Audio-Reactive")) {
                    m_timeline.addLane(track.layerId,
                                        TimelineLaneKind::AudioReactive, "bass > opacity");
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, trackSpacing));

        // ── Transition lane (between this row and the next) ───────────────
        // Thin row that holds any TimelineTransition connecting this layer
        // with the next layer below it. If the two layers' bars overlap in
        // time and no transition exists there yet, a subtle "+" affordance
        // appears inside the overlap region — click to create a crossfade.
        auto& allTracks = m_timeline.tracks();
        int thisIdx = -1;
        for (int k = 0; k < (int)allTracks.size(); k++) {
            if (allTracks[k].layerId == track.layerId) { thisIdx = k; break; }
        }
        bool hasNext = (thisIdx >= 0 && thisIdx + 1 < (int)allTracks.size());
        if (hasNext) {
            auto& nextTrack = allTracks[thisIdx + 1];
            uint32_t aId = track.layerId;      // upper row
            uint32_t bId = nextTrack.layerId;  // lower row

            const float laneH = 16.0f;
            ImVec2 laneOrigin = ImGui::GetCursorScreenPos();
            ImVec2 laneTrackOrigin(laneOrigin.x + gutterW, laneOrigin.y);
            ImGui::Dummy(ImVec2(gutterW + trackAreaW, laneH));

            // Lane background (very faint).
            dl->AddRectFilled(laneTrackOrigin,
                              ImVec2(laneTrackOrigin.x + trackAreaW,
                                     laneOrigin.y + laneH),
                              IM_COL32(255, 255, 255, 4), 4.0f);

            // Gutter label — tiny "⇄" icon + blank space to mirror the row rhythm.
            dl->AddTriangleFilled(ImVec2(laneOrigin.x + 18, laneOrigin.y + 4),
                                  ImVec2(laneOrigin.x + 18, laneOrigin.y + laneH - 4),
                                  ImVec2(laneOrigin.x + 26, laneOrigin.y + laneH * 0.5f),
                                  IM_COL32(180, 150, 240, 180));
            dl->AddTriangleFilled(ImVec2(laneOrigin.x + 36, laneOrigin.y + 4),
                                  ImVec2(laneOrigin.x + 36, laneOrigin.y + laneH - 4),
                                  ImVec2(laneOrigin.x + 28, laneOrigin.y + laneH * 0.5f),
                                  IM_COL32(180, 150, 240, 180));

            // Draw each transition connecting this pair.
            for (auto& tr : m_timeline.transitions()) {
                bool connectsPair =
                    (tr.fromLayerId == aId && tr.toLayerId == bId) ||
                    (tr.fromLayerId == bId && tr.toLayerId == aId);
                if (!connectsPair) continue;

                float tx0 = laneTrackOrigin.x + timeToX(tr.startTime);
                float tx1 = laneTrackOrigin.x + timeToX(tr.endTime());
                if (tx1 - tx0 < 2.0f) tx1 = tx0 + 2.0f;
                ImVec2 ta(tx0, laneOrigin.y + 2), tb(tx1, laneOrigin.y + laneH - 2);

                bool selected = (dragLayerId == 0xFFFFFFFF && dragClipId == tr.id);
                ImU32 fill = IM_COL32(170, 120, 230, selected ? 210 : 150);
                ImU32 edge = IM_COL32(210, 170, 255, selected ? 240 : 180);
                dl->AddRectFilled(ta, tb, fill, 4.0f);
                dl->AddRect(ta, tb, edge, 4.0f, 0, 1.0f);

                // Label (short name) if width allows.
                if (tx1 - tx0 > 40.0f) {
                    dl->PushClipRect(ta, tb, true);
                    dl->AddText(ImGui::GetFont(), 10.0f,
                                ImVec2(tx0 + 6, laneOrigin.y + 2),
                                IM_COL32(255, 255, 255, 235),
                                tr.name.empty() ? "crossfade" : tr.name.c_str());
                    dl->PopClipRect();
                }

                // Drag / resize: reuse the clip drag state machine by encoding
                // the transition id in dragClipId and 0xFFFFFFFF in dragLayerId.
                bool hoverT = (mx >= tx0 && mx <= tx1 && my >= ta.y && my <= tb.y
                               && ImGui::IsItemHovered());
                if (hoverT && dragClipId == 0) {
                    bool onLeft  = (mx < tx0 + trimZone);
                    bool onRight = (mx > tx1 - trimZone);
                    if (onLeft || onRight) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                // Single-click starts drag (body = move, edges = trim).
                if (hoverT && ImGui::IsMouseClicked(0) && dragClipId == 0) {
                    int mode = 0;
                    if (mx < tx0 + trimZone)      mode = 1;
                    else if (mx > tx1 - trimZone) mode = 2;
                    m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                    dragLayerId = 0xFFFFFFFF;   // sentinel: "this is a transition drag"
                    dragClipId  = tr.id;
                    dragMode    = mode;
                    dragStartTime = tr.startTime;
                    dragStartDur  = tr.duration;
                    dragAnchor    = ImGui::GetIO().MousePos;
                }
                // Double-click opens the effect picker inline at the bar.
                if (hoverT && ImGui::IsMouseDoubleClicked(0)) {
                    s_trPickerId  = tr.id;
                    s_trPickerPos = ImVec2(tx0, tb.y + 2);
                    ImGui::OpenPopup("##TrPicker");
                    // Cancel any drag that the first click of the double started.
                    dragLayerId = dragClipId = 0;
                    dragMode = 0;
                }
            }

            // Auto-"+" affordance in the overlap region of the two layers'
            // first clips — keep it simple: single overlap detection.
            auto* aTrack = m_timeline.findTrack(aId);
            auto* bTrack = m_timeline.findTrack(bId);
            if (aTrack && bTrack && !aTrack->clips.empty() && !bTrack->clips.empty()) {
                double as = aTrack->clips.front().startTime;
                double ae = aTrack->clips.back().endTime();
                double bs = bTrack->clips.front().startTime;
                double be = bTrack->clips.back().endTime();
                double ovStart = std::max(as, bs);
                double ovEnd   = std::min(ae, be);
                if (ovEnd - ovStart > 0.2) {
                    // Check if there's already a transition in this overlap.
                    bool haveTr = false;
                    for (const auto& tr : m_timeline.transitions()) {
                        bool connects = (tr.fromLayerId == aId && tr.toLayerId == bId) ||
                                        (tr.fromLayerId == bId && tr.toLayerId == aId);
                        if (connects && tr.endTime() > ovStart && tr.startTime < ovEnd) {
                            haveTr = true; break;
                        }
                    }
                    if (!haveTr) {
                        double ovMid = (ovStart + ovEnd) * 0.5;
                        float px = laneTrackOrigin.x + timeToX(ovMid);
                        float py = laneOrigin.y + laneH * 0.5f;
                        float r  = 6.0f;
                        // Small "+" circle
                        ImVec2 hit(px - r, py - r);
                        ImGui::SetCursorScreenPos(hit);
                        bool clicked = ImGui::InvisibleButton("##addtr", ImVec2(r * 2, r * 2));
                        bool hov = ImGui::IsItemHovered();
                        ImU32 col = hov ? IM_COL32(210, 170, 255, 240)
                                        : IM_COL32(170, 140, 210, 170);
                        dl->AddCircleFilled(ImVec2(px, py), r, col);
                        dl->AddLine(ImVec2(px - 3, py), ImVec2(px + 3, py),
                                    IM_COL32(255, 255, 255, 230), 1.5f);
                        dl->AddLine(ImVec2(px, py - 3), ImVec2(px, py + 3),
                                    IM_COL32(255, 255, 255, 230), 1.5f);
                        if (clicked) {
                            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                            double d = std::min(1.0, (ovEnd - ovStart) * 0.6);
                            m_timeline.addTransition(aId, bId,
                                                     ovMid - d * 0.5, d,
                                                     "crossfade");
                        }
                    }
                }
            }
        }
    }

    // ── Selected clip / transition inline inspector (compact pill row) ────
    // Only appears when something is selected. Styled to match the header
    // pills so it feels like the same app, not a legacy panel.
    if (auto* selClip = m_timeline.findClip(s_ctxLayerId, s_ctxClipId)) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushID((int)s_ctxClipId);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 172, 230));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Clip");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 14);

        // Transition picker
        auto names = GLTransitionLibrary::instance().names();
        std::string preview = selClip->transitionInName.empty()
                              ? std::string("No transition")
                              : std::string("> ") + selClip->transitionInName;
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("##clipXition", preview.c_str())) {
            if (ImGui::Selectable("No transition", selClip->transitionInName.empty())) {
                selClip->transitionInName.clear();
            }
            for (const auto& n : names) {
                bool sel = (selClip->transitionInName == n);
                if (ImGui::Selectable(n.c_str(), sel)) selClip->transitionInName = n;
            }
            ImGui::EndCombo();
        }

        // Transition duration — only meaningful when a transition is set.
        if (!selClip->transitionInName.empty() || !selClip->transitionInShaderPath.empty()) {
            ImGui::SameLine(0, 10);
            ImGui::SetNextItemWidth(110);
            float xd = (float)selClip->transitionInDuration;
            if (ImGui::SliderFloat("##clipXitionDur", &xd, 0.05f, 4.0f, "%.2fs")) {
                selClip->transitionInDuration = (double)xd;
            }
        }

        // Playback-mode dropdown — drives how the clip's source plays as the
        // playhead moves through it (Loop / Hold are wired into applyToLayers).
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Mode");
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##clipMode",
                               clipPlaybackModeName(selClip->playbackMode))) {
            for (int i = 0; i < 5; i++) {
                ClipPlaybackMode m = (ClipPlaybackMode)i;
                bool sel = (m == selClip->playbackMode);
                if (ImGui::Selectable(clipPlaybackModeName(m), sel)) {
                    selClip->playbackMode = m;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Forward / Loop / Hold are wired. Reverse & Ping-Pong are UI-only.");

        // Custom ISF shader path for the per-clip enter transition. Overrides
        // the gl-transitions name above; routed through the Layer's shader
        // transition slot in Timeline::applyToLayers.
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("ISF");
        ImGui::SameLine(0, 6);
        {
            static char isfBuf[512];
            static uint32_t lastClipId = 0;
            if (lastClipId != s_ctxClipId) {
                std::snprintf(isfBuf, sizeof(isfBuf), "%s",
                              selClip->transitionInShaderPath.c_str());
                lastClipId = s_ctxClipId;
            }
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("##clipISF", isfBuf, sizeof(isfBuf),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
                selClip->transitionInShaderPath = isfBuf;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                selClip->transitionInShaderPath = isfBuf;
            }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("...")) {
                std::string path = openFileDialog(
                    "ISF Shaders\0*.fs;*.frag;*.glsl\0All\0*.*\0");
                if (!path.empty()) {
                    selClip->transitionInShaderPath = path;
                    std::snprintf(isfBuf, sizeof(isfBuf), "%s", path.c_str());
                }
            }
            if (!selClip->transitionInShaderPath.empty()) {
                ImGui::SameLine(0, 4);
                if (ImGui::SmallButton("x##clearISF")) {
                    selClip->transitionInShaderPath.clear();
                    isfBuf[0] = '\0';
                }
            }
        }

        // Delete as a ghost pill (matches header family — no red fill).
        ImGui::SameLine(0, 14);
        {
            float h = ImGui::GetFrameHeight();
            const char* dl_ = "Delete";
            float w = ImGui::CalcTextSize(dl_).x + 22.0f;
            ImVec2 bp = ImGui::GetCursorScreenPos();
            bool clk = ImGui::InvisibleButton("##ClipDel", ImVec2(w, h));
            bool hov = ImGui::IsItemHovered();
            ImDrawList* d = ImGui::GetWindowDrawList();
            ImU32 bg = hov ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 15);
            ImU32 bd = hov ? IM_COL32(255, 70, 70, 220)  : IM_COL32(255, 255, 255, 80);
            ImU32 tx = hov ? IM_COL32(255, 100, 100, 255) : IM_COL32(235, 240, 250, 245);
            d->AddRectFilled(bp, ImVec2(bp.x + w, bp.y + h), bg, 5.0f);
            d->AddRect(bp, ImVec2(bp.x + w, bp.y + h), bd, 5.0f, 0, 1.0f);
            ImVec2 ts = ImGui::CalcTextSize(dl_);
            d->AddText(ImVec2(bp.x + (w - ts.x) * 0.5f, bp.y + (h - ts.y) * 0.5f),
                       tx, dl_);
            if (clk) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                if (s_multiSel.size() > 1) {
                    for (uint64_t sk : s_multiSel) {
                        uint32_t lid = (uint32_t)(sk >> 32);
                        uint32_t cid = (uint32_t)(sk & 0xFFFFFFFFu);
                        m_timeline.removeClip(lid, cid);
                    }
                    s_multiSel.clear();
                } else {
                    m_timeline.removeClip(s_ctxLayerId, s_ctxClipId);
                }
                s_ctxClipId = 0;
            }
        }

        // Show selection count when more than one clip is selected.
        if (s_multiSel.size() > 1) {
            ImGui::SameLine(0, 14);
            ImGui::TextDisabled("%d selected", (int)s_multiSel.size());
        }
        ImGui::PopID();
    }

    // ── In-timeline transition effect picker — opens on double-click of a
    // transition bar, anchored to the bar itself so it feels like editing in
    // place rather than a separate inspector.
    if (s_trPickerId != 0) {
        ImGui::SetNextWindowPos(s_trPickerPos);
        if (ImGui::BeginPopup("##TrPicker")) {
            if (auto* tr = m_timeline.findTransition(s_trPickerId)) {
                ImGui::TextDisabled("Effect (built-in)");
                ImGui::Separator();
                auto names = GLTransitionLibrary::instance().names();
                ImGui::PushItemWidth(180);
                for (const auto& n : names) {
                    bool sel = (tr->name == n && tr->shaderPath.empty());
                    if (ImGui::Selectable(n.c_str(), sel)) {
                        tr->name = n;
                        tr->shaderPath.clear(); // switching back to built-in
                    }
                }
                ImGui::PopItemWidth();

                // Custom ISF shader picker — any .fs file that exposes
                // `from`/`to`/`progress` uniforms becomes a transition. Lets
                // users drive the cross-layer blend with their own shaders.
                ImGui::Separator();
                ImGui::TextDisabled("Custom shader");
                if (!tr->shaderPath.empty()) {
                    // Show just the basename so the popup stays narrow.
                    std::string base = tr->shaderPath;
                    auto slash = base.find_last_of("/\\");
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.70f, 1.0f, 1.0f));
                    ImGui::TextWrapped("%s", base.c_str());
                    ImGui::PopStyleColor();
                }

                // Quick-pick from the ShaderClaw library when connected so
                // users don't have to browse to their shaders folder for
                // every transition. Picking sets shaderPath and clears the
                // built-in `name` so the ISF path takes precedence.
                if (m_shaderClaw.isConnected()) {
                    const auto& scList = m_shaderClaw.shaders();
                    if (!scList.empty()) {
                        ImGui::SetNextItemWidth(180);
                        if (ImGui::BeginCombo("##XitionSCPick", "From ShaderClaw...")) {
                            for (const auto& s : scList) {
                                const char* label = s.title.empty()
                                                    ? s.file.c_str()
                                                    : s.title.c_str();
                                if (ImGui::Selectable(label)) {
                                    tr->shaderPath = s.fullPath;
                                    tr->name.clear();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                }

                if (ImGui::Button(tr->shaderPath.empty() ? "Choose shader file..."
                                                         : "Change shader...",
                                  ImVec2(180, 0))) {
                    std::string path = openFileDialog(
                        "ISF Shaders\0*.fs;*.frag;*.glsl\0All\0*.*\0");
                    if (!path.empty()) {
                        tr->shaderPath = path;
                        tr->name.clear();
                    }
                }
                if (!tr->shaderPath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) tr->shaderPath.clear();
                }

                ImGui::Separator();
                float xd = (float)tr->duration;
                ImGui::SetNextItemWidth(180);
                if (ImGui::SliderFloat("Duration", &xd, 0.1f, 6.0f, "%.2fs")) {
                    tr->duration = (double)xd;
                }
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                if (ImGui::Selectable("Delete transition")) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                    m_timeline.removeTransition(s_trPickerId);
                    s_trPickerId = 0;
                }
                ImGui::PopStyleColor();
            } else {
                s_trPickerId = 0;
            }
            ImGui::EndPopup();
        } else {
            // Popup was dismissed — stop tracking which transition it targeted.
            s_trPickerId = 0;
        }
    }

    // ── Audio lane (beneath all tracks) ────────────────────────────────────
    // Signals "audio is part of the timeline and will be captured on export."
    // Visual: scrolling strip of the current AudioAnalyzer RMS — honest about
    // what we know (live levels), doesn't fake future waveforms.
    {
        const float audioH = 24.0f;
        ImVec2 audioOrigin = ImGui::GetCursorScreenPos();
        ImVec2 audioTrackOrigin(audioOrigin.x + gutterW, audioOrigin.y);
        ImGui::Dummy(ImVec2(gutterW + trackAreaW, audioH));

        // Clip-area strip background.
        dl->AddRectFilled(audioTrackOrigin,
                          ImVec2(audioTrackOrigin.x + trackAreaW, audioOrigin.y + audioH),
                          IM_COL32(255, 255, 255, 6), 8.0f);

        // Gutter cell — matches the track rows' column treatment.
        {
            dl->AddRectFilled(ImVec2(audioOrigin.x, audioOrigin.y + 2),
                              ImVec2(audioOrigin.x + gutterW - 4, audioOrigin.y + audioH - 2),
                              IM_COL32(255, 255, 255, 4), 6.0f);
            dl->AddLine(ImVec2(audioOrigin.x + gutterW - 2, audioOrigin.y + 4),
                        ImVec2(audioOrigin.x + gutterW - 2, audioOrigin.y + audioH - 4),
                        IM_COL32(255, 255, 255, 18), 1.0f);
            // Unified green palette — the swatch, label, envelope, and wave
            // all derive from the same base hue so the audio lane reads as a
            // single family instead of three near-matching greens.
            dl->AddRectFilled(ImVec2(audioOrigin.x + 28, audioOrigin.y + audioH * 0.5f - 5),
                              ImVec2(audioOrigin.x + 38, audioOrigin.y + audioH * 0.5f + 5),
                              IM_COL32(130, 220, 165, 220), 2.5f);
            dl->AddText(ImVec2(audioOrigin.x + 44, audioOrigin.y + (audioH - 14.0f) * 0.5f),
                        IM_COL32(180, 220, 195, 235), "Audio");
        }

        // Scrolling RMS history — right edge = now, scrolls left over ~4 seconds.
        static std::vector<float> s_rmsHistory(240, 0.0f);
        static double s_lastSample = 0.0;
        double now = glfwGetTime();
        if (now - s_lastSample > 0.016) {
            s_lastSample = now;
            s_rmsHistory.erase(s_rmsHistory.begin());
            s_rmsHistory.push_back(m_audioRMS);
        }
        float histX0 = audioTrackOrigin.x + 10.0f;
        float histX1 = audioTrackOrigin.x + trackAreaW - 10.0f;
        float histW  = histX1 - histX0;
        float cy     = audioOrigin.y + audioH * 0.5f;
        if (histW > 40.0f) {
            int n = (int)s_rmsHistory.size();
            float maxAmp = (audioH - 6.0f) * 0.5f;
            // Same green base as the swatch — alpha variation carries the hierarchy.
            const ImU32 envCol  = IM_COL32(130, 220, 165, 170);
            const ImU32 waveCol = IM_COL32(130, 220, 165, 255);

            // Symmetric envelope rails + traveling center wave. Amplitude
            // follows live RMS history; phase scrolls with time so the line
            // visibly moves like a soundwave as the song plays.
            std::vector<ImVec2> topPts, botPts, wavePts;
            topPts.reserve(n);
            botPts.reserve(n);
            wavePts.reserve(n);
            for (int i = 0; i < n; i++) {
                float rms = s_rmsHistory[i];
                if (rms > 1.0f) rms = 1.0f;
                float u   = (float)i / (float)(n - 1);
                float x   = histX0 + u * histW;
                float amp = rms * maxAmp;
                float phase = u * 42.0f - (float)now * 6.0f;
                float y = cy + sinf(phase) * (amp + 0.6f);
                topPts.emplace_back(x, cy - amp);
                botPts.emplace_back(x, cy + amp);
                wavePts.emplace_back(x, y);
            }
            dl->AddPolyline(topPts.data(),  (int)topPts.size(),
                            envCol,  ImDrawFlags_None, 1.0f);
            dl->AddPolyline(botPts.data(),  (int)botPts.size(),
                            envCol,  ImDrawFlags_None, 1.0f);
            dl->AddPolyline(wavePts.data(), (int)wavePts.size(),
                            waveCol, ImDrawFlags_None, 1.25f);
        }
    }

    // Apply drag state — handles both clip drags (dragLayerId = real layer id)
    // and transition drags (dragLayerId = 0xFFFFFFFF sentinel).
    if (dragClipId != 0 && dragLayerId == 0xFFFFFFFF) {
        // Transition drag
        if (!m_timeline.findTransition(dragClipId) || ImGui::IsMouseReleased(0)) {
            dragLayerId = dragClipId = 0;
            dragMode = 0;
        } else {
            float dx = ImGui::GetIO().MousePos.x - dragAnchor.x;
            double dt = xToTime(dx) - xToTime(0.0f);
            auto* tr = m_timeline.findTransition(dragClipId);
            if (tr) {
                if (dragMode == 0) {
                    tr->startTime = dragStartTime + dt;
                    if (tr->startTime < 0) tr->startTime = 0;
                } else if (dragMode == 1) {
                    double newStart = dragStartTime + dt;
                    double newDur   = dragStartDur - dt;
                    if (newDur < 0.1) { newDur = 0.1; newStart = dragStartTime + dragStartDur - 0.1; }
                    if (newStart < 0) { newDur -= (0.0 - newStart); newStart = 0; }
                    tr->startTime = newStart;
                    tr->duration  = newDur;
                } else if (dragMode == 2) {
                    double newDur = dragStartDur + dt;
                    if (newDur < 0.1) newDur = 0.1;
                    tr->duration = newDur;
                }
            }
        }
    } else if (dragClipId != 0) {
        // If the drag target was removed mid-drag (e.g. user deleted the layer
        // or the clip), bail out so the UI doesn't stay wedged in drag mode.
        if (!m_timeline.findClip(dragLayerId, dragClipId)) {
            dragLayerId = dragClipId = 0;
            dragMode = 0;
            s_dragStartOffsets.clear();
        } else if (ImGui::IsMouseReleased(0)) {
            dragLayerId = dragClipId = 0;
            dragMode = 0;
            s_dragStartOffsets.clear();
        } else {
            float dx = ImGui::GetIO().MousePos.x - dragAnchor.x;
            double dt = xToTime(dx) - xToTime(0.0f); // pixels → seconds
            if (auto* clip = m_timeline.findClip(dragLayerId, dragClipId)) {
                // Snap to playhead AND to other clip edges on any track,
                // pulling within ~6px.
                double ph = m_timeline.playhead();
                auto maybeSnap = [&](double& t) {
                    float dxp = timeToX(t) - timeToX(ph);
                    if (std::abs(dxp) < 6.0f) { t = ph; return; }
                    // Neighbor clip edges (skip the clip being dragged itself).
                    for (const auto& tr : m_timeline.tracks()) {
                        for (const auto& c : tr.clips) {
                            if (tr.layerId == dragLayerId && c.id == dragClipId) continue;
                            // Skip anything in the selected-set when batch-moving
                            // — snapping a group to itself locks it in place.
                            if (dragMode == 0 && s_multiSel.count(selKey(tr.layerId, c.id))) continue;
                            float dxs = timeToX(t) - timeToX(c.startTime);
                            if (std::abs(dxs) < 6.0f) { t = c.startTime; return; }
                            float dxe = timeToX(t) - timeToX(c.endTime());
                            if (std::abs(dxe) < 6.0f) { t = c.endTime();   return; }
                        }
                    }
                };
                double tlDur = m_timeline.duration();
                if (dragMode == 0) { // move
                    clip->startTime = dragStartTime + dt;
                    if (clip->startTime < 0) clip->startTime = 0;
                    // Keep the clip fully inside the timeline — trim tail
                    // never spills past the end.
                    if (clip->startTime + clip->duration > tlDur)
                        clip->startTime = tlDur - clip->duration;
                    if (clip->startTime < 0) clip->startTime = 0;
                    maybeSnap(clip->startTime);
                    // Batch-move every other selected clip by the actual delta
                    // the primary clip ended up with (after snap/clamp).
                    double actualDelta = clip->startTime - dragStartTime;
                    for (const auto& kv : s_dragStartOffsets) {
                        uint64_t sk = kv.first;
                        if (sk == selKey(dragLayerId, dragClipId)) continue;
                        uint32_t lid = (uint32_t)(sk >> 32);
                        uint32_t cid = (uint32_t)(sk & 0xFFFFFFFFu);
                        if (auto* c = m_timeline.findClip(lid, cid)) {
                            double ns = kv.second + actualDelta;
                            if (ns < 0) ns = 0;
                            if (ns + c->duration > tlDur) ns = tlDur - c->duration;
                            if (ns < 0) ns = 0;
                            c->startTime = ns;
                        }
                    }
                } else if (dragMode == 1) { // left trim
                    double newStart = dragStartTime + dt;
                    double newDur = dragStartDur - dt;
                    if (newDur < 0.1) { newDur = 0.1; newStart = dragStartTime + dragStartDur - 0.1; }
                    if (newStart < 0) { newDur -= (0.0 - newStart); newStart = 0; }
                    clip->startTime = newStart;
                    clip->duration = newDur;
                    maybeSnap(clip->startTime);
                } else if (dragMode == 2) { // right trim
                    double newDur = dragStartDur + dt;
                    if (newDur < 0.1) newDur = 0.1;
                    if (clip->startTime + newDur > tlDur)
                        newDur = tlDur - clip->startTime;
                    clip->duration = newDur;
                    double e = clip->startTime + clip->duration;
                    maybeSnap(e);
                    clip->duration = e - clip->startTime;
                    if (clip->duration < 0.1) clip->duration = 0.1;
                }
                // Keep every touched track sorted so subsequent edge-snap
                // lookups and clip-enter logic stay consistent.
                m_timeline.sortTrack(dragLayerId);
                if (dragMode == 0) {
                    for (const auto& kv : s_dragStartOffsets) {
                        uint32_t lid = (uint32_t)(kv.first >> 32);
                        if (lid != dragLayerId) m_timeline.sortTrack(lid);
                    }
                }
            }
        }
    }

    // --- Auto-scroll: keep the playhead on-screen during playback ---
    // Once the playhead passes the right 85% of the visible window, nudge the
    // horizontal scroll so the playhead lands near the left 15% of the view.
    // Only runs while playing — scrubbing and manual scroll stay unaffected.
    if (m_timeline.isPlaying()) {
        double visEnd = s_tlScroll + visibleDur;
        if (playhead > s_tlScroll + visibleDur * 0.85
            || playhead < s_tlScroll) {
            s_tlScroll = playhead - visibleDur * 0.15;
            if (s_tlScroll < 0.0) s_tlScroll = 0.0;
            if (s_tlScroll + visibleDur > duration) s_tlScroll = duration - visibleDur;
            if (s_tlScroll < 0.0) s_tlScroll = 0.0;
        }
        (void)visEnd;
    }

    // --- Playhead line (drawn on top) ---
    {
        float phX = rulerOrigin.x + timeToX(playhead);
        float phY0 = rulerOrigin.y;
        float phY1 = ImGui::GetCursorScreenPos().y + 4.0f;
        dl->AddLine(ImVec2(phX, phY0), ImVec2(phX, phY1),
                    IM_COL32(255, 200, 60, 230), 2.0f);
        // Playhead triangle handle on ruler
        dl->AddTriangleFilled(ImVec2(phX - 6, phY0),
                              ImVec2(phX + 6, phY0),
                              ImVec2(phX, phY0 + 10),
                              IM_COL32(255, 200, 60, 255));
        // Timecode readout is already shown in the transport row above — no
        // second label here. (Drawing it over the ruler collided with tick labels.)
    }

  } // end of if (!s_tlCollapsed)

    // Measure actual content height for next frame's auto-fit. CursorPosY is
    // window-local; add a bit of bottom padding to match the window style.
    s_tlMeasuredContentH = ImGui::GetCursorPosY() + 14.0f;

    ImGui::End();
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

// merged into renderTimelinePanel() — kept as a no-op so other callers
// (if any) don't break. The original body below is #if 0'd out.
void Application::renderTransportBar() {
    return;
#if 0
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
        // Idle: the button's red text is enough; no extra dot (it overlapped the label).
    } else {
        // Recording active: STOP replaces REC in place (same x), with the
        // pulse dot + elapsed timer rendered to the right so they don't
        // collide with the neighbouring System Audio dropdown.
        const float stopW = 64.0f;
        if (transportBtn("##StopRec", "STOP", curX, stopW, kRedDim, kRed)) {
            m_recorder.stop();
        }
        float pulse = 0.5f + 0.5f * sinf(time * 4.0f);
        float dotX = curX + stopW + 10.0f;
        draw->AddCircleFilled(ImVec2(dotX, cy + btnH * 0.5f), 5.0f, IM_COL32(255, 70, 70, (int)(pulse * 255)));
        int secs = (int)m_recorder.uptimeSeconds();
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
        draw->AddText(ImVec2(dotX + 10, cy + (btnH - ImGui::GetTextLineHeight()) * 0.5f), kText, timeBuf);
        // Advance curX past the stop button + timer block so downstream
        // widgets (System Audio dropdown) don't overlap STOP/timer.
        curX += stopW + 80.0f;
        // The common path below adds curX += 80; undo that so the total
        // advance matches our explicit advance here.
        curX -= 80.0f;
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
#endif // #if 0 (original body, merged into renderTimelinePanel)
}
#endif

void Application::renderMenuBar() {
    // Taller main menu row with even top/bottom padding around the text so
    // EDIT/FILE/LAYER/ZONE sit centred in the row next to the traffic-light
    // buttons. Slight font-scale-down (via SetWindowFontScale on the menu
    // window) makes the caps sit a notch smaller than body text.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
    if (ImGui::BeginMainMenuBar()) {
        if (ImGuiWindow* mw = ImGui::GetCurrentWindow()) mw->FontWindowScale = 0.88f;
#ifdef __APPLE__
        // Reserve space on the left for the macOS traffic-light buttons
        // (close / min / max) — the window title bar has been merged into
        // the content area so they now sit inside our ImGui row. In
        // native fullscreen AppKit hides the traffic-lights, so skip
        // the inset or the menu row gets an ugly empty gap on the left.
        if (!m_editorFullscreen) {
            const float kTrafficLightInset = 78.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kTrafficLightInset);
        } else {
            // Fullscreen: AppKit hides the traffic-lights, so push the menu
            // flush against the left edge (override the menu window padding).
            ImGui::SetCursorPosX(0.0f);
        }
#endif
        if (ImGui::BeginMenu("EDIT")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_undoStack.canUndo())) {
                m_undoStack.undo(m_layerStack, m_selectedLayer);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_undoStack.canRedo())) {
                m_undoStack.redo(m_layerStack, m_selectedLayer);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("FILE")) {
            if (ImGui::MenuItem("New Project")) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                while (m_layerStack.count() > 0) {
                    int li = m_layerStack.count() - 1;
                    uint32_t rid = m_layerStack[li] ? m_layerStack[li]->id : 0;
                    m_layerStack.removeLayer(li);
                    if (rid) m_timeline.removeTrackForLayer(rid);
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
            if (ImGui::MenuItem("Add Particle System")) {
                addParticles();
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

        if (ImGui::BeginMenu("LAYER")) {
            if (ImGui::MenuItem("Remove Selected") && m_selectedLayer >= 0) {
                m_undoStack.pushState(m_layerStack, m_selectedLayer);
                uint32_t rid = (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count() && m_layerStack[m_selectedLayer])
                    ? m_layerStack[m_selectedLayer]->id : 0;
                m_layerStack.removeLayer(m_selectedLayer);
                if (rid) m_timeline.removeTrackForLayer(rid);
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

        if (ImGui::BeginMenu("ZONE")) {
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

        // Fullscreen lives next to the composition chip in the viewport;
        // GO LIVE lives next to REC in the timeline transport. The menu bar
        // now carries only the Edit/File/Layer/Zone dropdowns so it fits
        // inside the unified title bar.
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
}

#ifdef HAS_FFMPEG
void Application::renderGoLiveButton() {
    const char* lbl = m_rtmpOutput.isActive() ? "END LIVE" : "GO LIVE";
    if (m_rtmpOutput.isActive()) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.06f, 0.06f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.10f, 0.10f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.14f, 0.14f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 0.55f, 0.55f, 1.00f));
    } else {
        bool hasKey = m_streamKeyBuf[0] != '\0';
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.07f, 0.22f, 0.36f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.32f, 0.52f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.40f, 0.62f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            hasKey ? ImVec4(0.38f, 0.76f, 1.00f, 1.00f)
                   : ImVec4(0.60f, 0.62f, 0.68f, 1.00f));
    }
    if (ImGui::Button(lbl)) {
        if (m_rtmpOutput.isActive()) {
            m_rtmpOutput.stop();
        } else {
            ImGui::OpenPopup("##GoLivePopup");
        }
    }
    ImGui::PopStyleColor(4);

    if (ImGui::BeginPopup("##GoLivePopup")) {
        ImGui::TextDisabled("Stream to RTMP");
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
        ImGui::Text("YouTube Stream Key");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("##StreamKeyMenu", m_streamKeyBuf,
                         sizeof(m_streamKeyBuf),
                         ImGuiInputTextFlags_Password);
        static const char* aspectNames[] = { "16:9", "4:3", "16:10", "Source" };
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
        ImGui::Text("Aspect");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(260);
        ImGui::Combo("##AspectMenu", &m_streamAspect, aspectNames, 4);
        ImGui::Separator();
        bool hasKey = m_streamKeyBuf[0] != '\0';
        ImGui::BeginDisabled(!hasKey);
        if (ImGui::Button("Start streaming", ImVec2(-1, 0))) {
            static const int aspectNums[] = { 16, 4, 16, 0 };
            static const int aspectDens[] = { 9,  3, 10, 0 };
            auto& z = activeZone();
            m_rtmpOutput.start(m_streamKeyBuf,
                               z.warpFBO.width(), z.warpFBO.height(),
                               aspectNums[m_streamAspect],
                               aspectDens[m_streamAspect], 30);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!hasKey) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
            ImGui::TextWrapped("Paste a YouTube stream key to enable.");
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
}
#endif

void Application::toggleEditorFullscreen() {
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

void Application::addParticles() {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<ParticleSource>();
    if (!src->init()) {
        std::cerr << "Failed to init ParticleSource" << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "Particle System";

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

#ifdef HAS_OPENCV
void Application::addWebcam(int cameraIndex) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<WebcamSource>();
    if (!src->open(cameraIndex)) {
        std::cerr << "Failed to open webcam index " << cameraIndex << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "Camera " + std::to_string(cameraIndex);

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

        // Transition fields (opacity fade + shader-based A→B swap)
        layerJson["transitionType"] = (int)layer->transitionType;
        layerJson["transitionDuration"] = layer->transitionDuration;
        if (!layer->transitionShaderPath.empty())
            layerJson["transitionShaderPath"] = layer->transitionShaderPath;

        layers.push_back(layerJson);
    }
    j["layers"] = layers;

    // Timeline: tracks, clips, playhead, duration, loop.
    j["timeline"] = m_timeline.toJson();

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
        uint32_t rid = m_layerStack[0] ? m_layerStack[0]->id : 0;
        m_layerStack.removeLayer(0);
        if (rid) m_timeline.removeTrackForLayer(rid);
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
            // Transition fields
            if (layerJson.contains("transitionType"))
                layer->transitionType = (TransitionType)layerJson["transitionType"].get<int>();
            layer->transitionDuration = layerJson.value("transitionDuration", 0.5f);
            layer->transitionShaderPath = layerJson.value("transitionShaderPath", std::string{});
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

    // Timeline (must come AFTER layer IDs are assigned so track.layerId resolves).
    if (j.contains("timeline")) {
        m_timeline.fromJson(j["timeline"]);
    } else {
        m_timeline.clear();
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
