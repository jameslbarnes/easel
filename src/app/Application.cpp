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

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, "Easel", nullptr, nullptr);
    if (!m_window) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // Set window icon
    {
        int iw, ih, ic;
        unsigned char* iconData = stbi_load("resources/icon.png", &iw, &ih, &ic, 4);
        if (iconData) {
            GLFWimage icon = { iw, ih, iconData };
            glfwSetWindowIcon(m_window, 1, &icon);
            stbi_image_free(iconData);
        }
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetDropCallback(m_window, Application::dropCallback);

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(0); // uncapped — NDI needs low-latency updates

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD" << std::endl;
        return false;
    }

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;

    if (!m_ui.init(m_window)) return false;

    // Create default output zone
    auto zone = std::make_unique<OutputZone>();
    if (!zone->init()) return false;
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
        const char* home = getenv("USERPROFILE");
        if (home) {
            std::string candidates[] = {
                std::string(home) + "\\Documents\\ShaderClaw3\\shaders",
                std::string(home) + "\\Documents\\ShaderClaw\\shaders",
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

    // Start with a blank project (no auto-load)
    std::cout << "[Easel] Starting with blank project" << std::endl;

    return true;
}

void Application::run() {
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        // Escape on main window closes all projectors
        if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS && !m_projectors.empty()) {
            for (auto& [idx, proj] : m_projectors) proj->destroy();
            m_projectors.clear();
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

        // Auto-detect monitor hotplug — set active zone to fullscreen on secondary
        if (m_projectorAutoConnect) {
            int monitorCount = (int)ProjectorOutput::enumerateMonitors().size();
            if (monitorCount != m_lastMonitorCount) {
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
            m_audioAnalyzer.setDevice(m_mosaicAudioDevice);
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
                saveProject("default.easel");
                lastAutoSave = now;
            }
        }

        glViewport(0, 0, m_windowWidth, m_windowHeight);
        glClearColor(0.055f, 0.063f, 0.082f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_ui.beginFrame();
        renderUI();
        m_ui.endFrame();

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
        std::string defaultPath = "default.easel";
        saveProject(defaultPath);
        std::cout << "[Easel] Auto-saved default project" << std::endl;
    }

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
                // Match shader resolution to composition size
                auto& zone = activeZone();
                shaderSrc->setResolution(zone.width, zone.height);
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
                    m_audioAnalyzer.beatDecay()
                );
                shaderSrc->setMouseState(normMX, normMY, mousePressed);

                // Refresh image input bindings (texture IDs may change each frame)
                for (auto& [name, binding] : shaderSrc->imageBindings()) {
                    if (binding.sourceLayerId == 0) continue;
                    for (int j = 0; j < m_layerStack.count(); j++) {
                        auto& srcLayer = m_layerStack[j];
                        if (srcLayer->id == binding.sourceLayerId && srcLayer->source) {
                            binding.textureId = srcLayer->source->textureId();
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

    // Re-render any dirty masks before compositing (shared across zones)
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

    zone.warpFBO.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (sourceTex) {
        if (zone.warpMode == ViewportPanel::WarpMode::CornerPin) {
            zone.cornerPin.render(sourceTex);
        } else if (zone.warpMode == ViewportPanel::WarpMode::MeshWarp) {
            zone.meshWarp.render(sourceTex);
        } else if (zone.warpMode == ViewportPanel::WarpMode::ObjMesh) {
            float aspect = (float)zone.width / (float)zone.height;
            zone.objMeshWarp.render(sourceTex, aspect);
        }
    }

    Framebuffer::unbind();

    // Edge blend post-process (if any edge has blend width > 0)
    bool hasEdgeBlend = zone.edgeBlendLeft > 0 || zone.edgeBlendRight > 0 ||
                        zone.edgeBlendTop > 0 || zone.edgeBlendBottom > 0;
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
        m_edgeBlendShader.setFloat("uBlendLeft", zone.edgeBlendLeft / (float)zone.width);
        m_edgeBlendShader.setFloat("uBlendRight", zone.edgeBlendRight / (float)zone.width);
        m_edgeBlendShader.setFloat("uBlendTop", zone.edgeBlendTop / (float)zone.height);
        m_edgeBlendShader.setFloat("uBlendBottom", zone.edgeBlendBottom / (float)zone.height);
        m_edgeBlendShader.setFloat("uGamma", zone.edgeBlendGamma);

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

void Application::presentOutputs() {
    glFinish(); // Ensure all zone FBOs are written before presenting

    // Track which monitor indices are still needed
    std::set<int> neededMonitors;

    // Per-zone output routing
    for (int i = 0; i < (int)m_zones.size(); i++) {
        auto& zone = *m_zones[i];

        if (zone.outputDest == OutputDest::Fullscreen && zone.outputMonitor >= 0) {
            // Verify monitor still exists before using it
            auto monitors = ProjectorOutput::enumerateMonitors();
            if (zone.outputMonitor >= (int)monitors.size()) {
                // Monitor disconnected -- clear destination to avoid crash
                zone.outputDest = OutputDest::None;
                zone.outputMonitor = -1;
                // Destroy any stale projector
                auto stale = m_projectors.find(zone.outputMonitor);
                if (stale != m_projectors.end()) {
                    stale->second->destroy();
                    m_projectors.erase(stale);
                }
            } else {
                neededMonitors.insert(zone.outputMonitor);
                // Ensure a projector exists on this monitor
                auto it = m_projectors.find(zone.outputMonitor);
                if (it == m_projectors.end() || !it->second->isActive()) {
                    auto proj = std::make_unique<ProjectorOutput>();
                    if (proj->create(m_window, zone.outputMonitor)) {
                        zone.resize(proj->projectorWidth(), proj->projectorHeight());
                        m_projectors[zone.outputMonitor] = std::move(proj);
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
                zone.ndiOutput.send(zone.warpFBO.textureId(), zone.warpFBO.width(), zone.warpFBO.height());
            }
        } else {
            // Destroy NDI sender if no longer needed
            if (zone.ndiOutput.isActive()) {
                zone.ndiOutput.destroy();
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
#ifdef HAS_NDI
    // Legacy global NDI output (composition toggle in NDI panel)
    if (m_ndiOutputEnabled && m_ndiOutput.isActive()) {
        m_ndiOutput.send(active.warpFBO.textureId(), active.warpFBO.width(), active.warpFBO.height());
    }
#endif

#ifdef HAS_FFMPEG
    if (m_rtmpOutput.isActive()) {
        m_rtmpOutput.sendFrame(active.warpFBO.textureId(), active.warpFBO.width(), active.warpFBO.height());
    }
    if (m_recorder.isActive()) {
        m_recorder.sendFrame(active.warpFBO.textureId(), active.warpFBO.width(), active.warpFBO.height());
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
    zone->init();
    m_zones.push_back(std::move(zone));
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
    z->warpMode = src.warpMode;
    z->showAllLayers = src.showAllLayers;
    z->visibleLayerIds = src.visibleLayerIds;
    z->outputDest = OutputDest::None; // user picks new output for copy
    z->outputMonitor = -1;

    z->init();

    // Copy warp points from source
    auto& dstCorners = z->cornerPin.corners();
    const auto& srcCorners = src.cornerPin.corners();
    for (int i = 0; i < 4; i++) dstCorners[i] = srcCorners[i];

    z->meshWarp.setGridSize(src.meshWarp.cols(), src.meshWarp.rows());
    auto& dstPts = z->meshWarp.points();
    const auto& srcPts = src.meshWarp.points();
    for (int i = 0; i < (int)srcPts.size() && i < (int)dstPts.size(); i++) {
        dstPts[i] = srcPts[i];
    }

    // Copy OBJ mesh state
    if (src.objMeshWarp.isLoaded()) {
        z->objMeshWarp.loadModel(src.objMeshWarp.meshPath());
        z->objMeshWarp.modelScale() = src.objMeshWarp.modelScale();
        z->objMeshWarp.modelPosition() = src.objMeshWarp.modelPosition();
        z->objMeshWarp.camera() = src.objMeshWarp.camera();
    }

    m_zones.push_back(std::move(z));
}

void Application::renderUI() {
    handleDroppedFiles();

#ifdef HAS_FFMPEG
    float transportBarH = 110.0f;
#else
    float transportBarH = 0.0f;
#endif
    m_ui.setupDockspace(transportBarH);
    renderMenuBar();

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
        m_viewportPanel.render(z.warpFBO.textureId(), z.cornerPin, z.meshWarp, z.warpMode, projAspect,
                               &m_zones, &m_activeZone, &monitors, ndiAvail, &z.objMeshWarp);
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
    m_layerPanel.render(m_layerStack, m_selectedLayer, &m_zones, m_activeZone);

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

    std::shared_ptr<Layer> selectedLayer;
    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
        selectedLayer = m_layerStack[m_selectedLayer];
    }
    MosaicAudioState mosaicAudio;
    mosaicAudio.selectedDevice = &m_mosaicAudioDevice;
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

    // Capture undo snapshot BEFORE the property panel modifies values
    SceneSnapshot preEditSnapshot;
    bool capturedPre = false;
    if (!m_propertyPanel.undoNeeded) {
        preEditSnapshot = UndoStack::captureSnapshot(m_layerStack, m_selectedLayer);
        capturedPre = true;
    }

    m_propertyPanel.render(selectedLayer, m_maskEditMode, &m_speechState, &mosaicAudio, (float)glfwGetTime(), &m_layerStack);

    // If a property widget was just activated, push the pre-edit state (before the widget changed it)
    if (m_propertyPanel.undoNeeded) {
        if (capturedPre) {
            m_undoStack.pushSnapshot(std::move(preEditSnapshot));
        }
        m_propertyPanel.undoNeeded = false;
    }

    // Set viewport edit mode based on current editing state
    if (m_maskEditMode && selectedLayer && selectedLayer->maskEnabled) {
        m_viewportPanel.setEditMode(ViewportPanel::EditMode::Mask);
        m_viewportPanel.renderMaskOverlay(selectedLayer->maskPath);
    } else {
        m_viewportPanel.setEditMode(ViewportPanel::EditMode::Normal);
        m_viewportPanel.renderLayerOverlay(m_layerStack, m_selectedLayer, zone.width, zone.height);
        m_maskEditMode = false;

        // Double-click corner/edge: enter mask edit mode
        if (m_viewportPanel.wantsMaskEdit() && selectedLayer) {
            m_viewportPanel.clearMaskEditSignal();
            selectedLayer->maskEnabled = true;
            m_maskEditMode = true;
            // If edge-click had a UV, add initial point
            glm::vec2 uv = m_viewportPanel.maskEditClickUV();
            if (uv.x > 0.001f || uv.y > 0.001f) {
                selectedLayer->maskPath.addPoint(uv);
            }
        }
    }

    auto prevWarpMode = zone.warpMode;
    m_warpEditor.render(zone.cornerPin, zone.meshWarp, zone.objMeshWarp, zone.warpMode);

    // Recreate warp FBO with/without depth when mode changes
    if (zone.warpMode != prevWarpMode) {
        bool needsDepth = (zone.warpMode == ViewportPanel::WarpMode::ObjMesh);
        zone.warpFBO.create(zone.width, zone.height, needsDepth);
    }

    // Handle OBJ load request
    if (m_warpEditor.wantsLoadOBJ()) {
        std::string path = openFileDialog("3D Models\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
        if (!path.empty()) {
            zone.objMeshWarp.loadModel(path);
        }
    }

    // Projector panel
    ImGui::Begin("Projector");
    {
        // --- Composition Resolution ---
        {
            static const char* presetLabels[] = {
                "1920x1080 (1080p)", "3840x2160 (4K)", "1280x720 (720p)",
                "2560x1440 (1440p)", "8000x2000 (Ultra-wide)", "1024x768", "Custom"
            };
            static const int presetW[] = { 1920, 3840, 1280, 2560, 8000, 1024, 0 };
            static const int presetH[] = { 1080, 2160, 720, 1440, 2000, 768, 0 };
            const int presetCount = 7;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Composition");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%dx%d", zone.width, zone.height);

            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##CompRes", &zone.compPreset, presetLabels, presetCount)) {
                if (zone.compPreset < presetCount - 1) {
                    int nw = presetW[zone.compPreset];
                    int nh = presetH[zone.compPreset];
                    zone.resize(nw, nh);
                }
            }

            // Custom resolution input
            if (zone.compPreset == presetCount - 1) {
                float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                ImGui::SetNextItemWidth(half);
                int cw = zone.width;
                if (ImGui::DragInt("##CW", &cw, 1, 128, 16384, "%d")) {
                    if (cw >= 128 && cw <= 16384) {
                        zone.resize(cw, zone.height);
                    }
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(half);
                int ch = zone.height;
                if (ImGui::DragInt("##CH", &ch, 1, 128, 16384, "%d")) {
                    if (ch >= 128 && ch <= 16384) {
                        zone.resize(zone.width, ch);
                    }
                }
            }

            ImGui::Dummy(ImVec2(0, 4));
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
            }
            ImGui::Dummy(ImVec2(0, 6));
        }

        // Edge Blend
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Edge Blend");
            ImGui::PopStyleColor();

            float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBL", &zone.edgeBlendLeft, 1.0f, 0.0f, (float)zone.width * 0.5f, "L %.0fpx");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBR", &zone.edgeBlendRight, 1.0f, 0.0f, (float)zone.width * 0.5f, "R %.0fpx");

            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBT", &zone.edgeBlendTop, 1.0f, 0.0f, (float)zone.height * 0.5f, "T %.0fpx");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            ImGui::DragFloat("##EBB", &zone.edgeBlendBottom, 1.0f, 0.0f, (float)zone.height * 0.5f, "B %.0fpx");

            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##EBGamma", &zone.edgeBlendGamma, 0.05f, 0.5f, 4.0f, "Gamma %.2f");

            ImGui::Dummy(ImVec2(0, 4));
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
            }
            ImGui::Dummy(ImVec2(0, 6));
        }

        // Output routing status (read-only — routing is done via Output dropdown in viewport)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Output");
        ImGui::PopStyleColor();

        if (zone.outputDest == OutputDest::None) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.6f));
            ImGui::Text("Preview only");
            ImGui::PopStyleColor();
        } else if (zone.outputDest == OutputDest::Fullscreen) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            auto monitors = ProjectorOutput::enumerateMonitors();
            if (zone.outputMonitor >= 0 && zone.outputMonitor < (int)monitors.size()) {
                ImGui::Text("Fullscreen: %s", monitors[zone.outputMonitor].name.c_str());
            } else {
                ImGui::Text("Fullscreen");
            }
            ImGui::PopStyleColor();
        } else if (zone.outputDest == OutputDest::NDI) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            ImGui::Text("NDI: %s", zone.ndiStreamName.empty() ? zone.name.c_str() : zone.ndiStreamName.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Checkbox("Auto-detect", &m_projectorAutoConnect);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("(?)");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically open projector when a\nsecond display is connected");
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
                const char* home = getenv("USERPROFILE");
                if (home) {
                    // Try known ShaderClaw locations
                    std::string candidates[] = {
                        std::string(home) + "\\Documents\\ShaderClaw3\\shaders",
                        std::string(home) + "\\Documents\\ShaderClaw\\shaders",
                        std::string(home) + "\\shader-claw3\\shaders",
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

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::SmallButton("+")) {
                    loadShader(shader.fullPath);
                }
                ImGui::PopStyleColor(4);

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                ImGui::Text("%s", shader.title.c_str());
                ImGui::PopStyleColor();

                if (!shader.description.empty()) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.8f));
                    std::string desc = shader.description;
                    if (desc.length() > 50) desc = desc.substr(0, 47) + "...";
                    ImGui::Text("- %s", desc.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::PopID();
            }
        }
    }
    ImGui::End();

    // Etherea panel — SSE connection for transcript + data
    ImGui::Begin("Etherea");
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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
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
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
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
    }
    ImGui::End();

#ifdef HAS_NDI
    if (NDIRuntime::instance().isAvailable()) {
        ImGui::Begin("NDI");
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
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
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
        ImGui::End();
    }
#endif

#ifdef HAS_FFMPEG
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
#endif

#ifdef HAS_OPENCV
    m_scanPanel.render(m_scanner, m_webcam);
#endif

#ifdef HAS_FFMPEG
    renderTransportBar();
#endif
}

#ifdef HAS_FFMPEG
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

void Application::cleanupAudioMeter() {
    if (m_audioMeterInfo) {
        ((IAudioMeterInformation*)m_audioMeterInfo)->Release();
        m_audioMeterInfo = nullptr;
    }
    if (m_audioMeterDevice) {
        ((IMMDevice*)m_audioMeterDevice)->Release();
        m_audioMeterDevice = nullptr;
    }
    m_meterDeviceIdx = -2;
}

void Application::updateAudioMeter() {
    // Reinit meter if selected device changed
    if (m_meterDeviceIdx != m_selectedAudioDevice) {
        cleanupAudioMeter();
        m_meterDeviceIdx = m_selectedAudioDevice;

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
                    // Microphone — get by ID from capture devices
                    std::wstring wid(dev.id.begin(), dev.id.end());
                    hr = enumerator->GetDevice(wid.c_str(), &device);
                } else {
                    // Output device for loopback — get by ID from render devices
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
                    device->Release();
                }
            }
            enumerator->Release();
        }
    }

    // Poll levels
    if (m_audioMeterInfo) {
        IAudioMeterInformation* meter = (IAudioMeterInformation*)m_audioMeterInfo;
        float peak = 0.0f;
        if (SUCCEEDED(meter->GetPeakValue(&peak))) {
            m_audioLevelPeak = peak;
        }

        UINT32 channelCount = 0;
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
    } else {
        m_audioLevelPeak = m_audioLevelL = m_audioLevelR = 0.0f;
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
    float barH = 110.0f;
    ImVec2 barPos(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - barH);
    ImVec2 barSize(vp->WorkSize.x, barH);

    ImGui::SetNextWindowPos(barPos);
    ImGui::SetNextWindowSize(barSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.040f, 0.055f, 1.0f));

    ImGui::Begin("##TransportBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto& zone = activeZone();
    float time = (float)ImGui::GetTime();

    // Update audio levels
    updateAudioMeter();

    // Top edge — subtle gradient line
    for (int i = 0; i < (int)barSize.x; i++) {
        float t = (float)i / barSize.x;
        int alpha = (int)(20.0f + 15.0f * sinf(t * 3.14159f));
        draw->AddLine(ImVec2(barPos.x + i, barPos.y),
                      ImVec2(barPos.x + i + 1, barPos.y),
                      IM_COL32(0, 200, 255, alpha));
    }
    draw->AddLine(ImVec2(barPos.x, barPos.y + 1),
                  ImVec2(barPos.x + barSize.x, barPos.y + 1),
                  IM_COL32(0, 0, 0, 100));

    // Sizes for the big buttons
    float bigBtnH = 60.0f;
    float bigBtnRound = 8.0f;
    float padTop = (barH - bigBtnH) * 0.5f;

    // ════════════════════════════════════════════════
    //  RECORD BUTTON (left)
    // ════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(20, padTop));

    if (!m_recorder.isActive()) {
        float btnW = 140.0f;
        ImVec2 btnMin = ImGui::GetCursorScreenPos();
        ImVec2 btnMax(btnMin.x + btnW, btnMin.y + bigBtnH);
        ImGui::InvisibleButton("##RecBtn", ImVec2(btnW, bigBtnH));
        bool hovered = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();

        // Gradient-like background
        ImU32 btnBg = held ? IM_COL32(180, 30, 30, 80) :
                      hovered ? IM_COL32(150, 25, 25, 55) :
                                IM_COL32(100, 12, 12, 35);
        draw->AddRectFilled(btnMin, btnMax, btnBg, bigBtnRound);
        draw->AddRect(btnMin, btnMax,
                      IM_COL32(220, 60, 60, hovered ? 140 : 70), bigBtnRound, 0, 1.5f);

        // Glow on hover
        if (hovered) {
            draw->AddRect(ImVec2(btnMin.x - 1, btnMin.y - 1), ImVec2(btnMax.x + 1, btnMax.y + 1),
                          IM_COL32(255, 60, 60, 25), bigBtnRound + 1, 0, 3.0f);
        }

        // Big red circle icon
        float iconX = btnMin.x + 30;
        float iconY = btnMin.y + bigBtnH * 0.5f;
        draw->AddCircleFilled(ImVec2(iconX, iconY), 12.0f, IM_COL32(220, 45, 45, 230));
        draw->AddCircle(ImVec2(iconX, iconY), 12.0f, IM_COL32(255, 100, 100, 80), 0, 1.5f);
        draw->AddCircle(ImVec2(iconX, iconY), 15.0f, IM_COL32(255, 60, 60, 30), 0, 1.0f);

        // Label
        const char* recLabel = "REC";
        ImVec2 textSize = ImGui::CalcTextSize(recLabel);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f,
                      ImVec2(btnMin.x + 52, btnMin.y + (bigBtnH - ImGui::GetFontSize() * 1.1f) * 0.5f),
                      IM_COL32(230, 85, 85, 255), recLabel);

        if (ImGui::IsItemClicked()) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
            localtime_s(&tm_buf, &t);
            char fname[128];
            strftime(fname, sizeof(fname), "recordings/%Y%m%d_%H%M%S.mp4", &tm_buf);
            m_recorder.setAudioDevice(m_selectedAudioDevice);
            m_recorder.start(fname, zone.warpFBO.width(), zone.warpFBO.height(), 30);
        }
    } else {
        // ── Recording active ──
        float pulse = 0.5f + 0.5f * sinf(time * 4.0f);

        // Flashing rec indicator
        float dotX = barPos.x + 35, dotY = barPos.y + barH * 0.5f;
        draw->AddCircleFilled(ImVec2(dotX, dotY), 10.0f, IM_COL32(255, 40, 40, (int)(pulse * 255)));
        draw->AddCircle(ImVec2(dotX, dotY), 13.0f, IM_COL32(255, 40, 40, (int)(pulse * 70)), 0, 1.5f);
        draw->AddCircle(ImVec2(dotX, dotY), 17.0f, IM_COL32(255, 40, 40, (int)(pulse * 25)), 0, 1.0f);

        // Timer text (large)
        int secs = (int)m_recorder.uptimeSeconds();
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
        float timerFontSize = ImGui::GetFontSize() * 1.3f;
        ImVec2 timerPos(dotX + 22, barPos.y + (barH - timerFontSize) * 0.5f);
        draw->AddText(ImGui::GetFont(), timerFontSize, timerPos,
                      IM_COL32(255, 90, 90, 255), timeBuf);
        float timerW = ImGui::GetFont()->CalcTextSizeA(timerFontSize, FLT_MAX, 0, timeBuf).x;

        // Stop button
        float stopX = timerPos.x + timerW + 20;
        ImGui::SetCursorScreenPos(ImVec2(stopX, barPos.y + padTop));
        float stopW = 100.0f;
        ImVec2 stopMin(stopX, barPos.y + padTop);
        ImVec2 stopMax(stopX + stopW, barPos.y + padTop + bigBtnH);
        ImGui::InvisibleButton("##StopRec", ImVec2(stopW, bigBtnH));
        bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
        ImU32 stopBg = sa ? IM_COL32(180, 30, 30, 80) : sh ? IM_COL32(140, 20, 20, 55) : IM_COL32(80, 12, 12, 35);
        draw->AddRectFilled(stopMin, stopMax, stopBg, bigBtnRound);
        draw->AddRect(stopMin, stopMax, IM_COL32(200, 60, 60, sh ? 140 : 70), bigBtnRound, 0, 1.5f);

        // Square stop icon
        float sqSz = 10.0f;
        float sqX = stopMin.x + 22, sqY = stopMin.y + (bigBtnH - sqSz) * 0.5f;
        draw->AddRectFilled(ImVec2(sqX, sqY), ImVec2(sqX + sqSz, sqY + sqSz),
                            IM_COL32(220, 70, 70, 230), 2.0f);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f,
                      ImVec2(stopMin.x + 40, stopMin.y + (bigBtnH - ImGui::GetFontSize() * 1.1f) * 0.5f),
                      IM_COL32(220, 95, 95, 255), "Stop");

        if (ImGui::IsItemClicked()) {
            m_recorder.stop();
        }
    }

    // ════════════════════════════════════════════════
    //  DIVIDER 1
    // ════════════════════════════════════════════════
    ImGui::SameLine(0, 20);
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float divTop = barPos.y + 18;
        float divBot = barPos.y + barH - 18;
        draw->AddLine(ImVec2(p.x, divTop), ImVec2(p.x, divBot), IM_COL32(255, 255, 255, 20));
        draw->AddLine(ImVec2(p.x + 1, divTop), ImVec2(p.x + 1, divBot), IM_COL32(0, 0, 0, 40));
        ImGui::Dummy(ImVec2(4, bigBtnH));
    }

    // ════════════════════════════════════════════════
    //  AUDIO SOURCE (center section)
    // ════════════════════════════════════════════════
    ImGui::SameLine(0, 16);

    // Lazy-init device list
    if (m_audioDevices.empty()) {
        m_audioDevices = VideoRecorder::enumerateAudioDevices();
    }

    std::string audioPreview = "System Audio";
    if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
        audioPreview = m_audioDevices[m_selectedAudioDevice].name;
        if (audioPreview.length() > 30) audioPreview = audioPreview.substr(0, 27) + "...";
    }

    // Audio section: speaker icon + combo, single row centered
    {
        ImVec2 sectionStart = ImGui::GetCursorScreenPos();
        float iconCY = barPos.y + barH * 0.5f;
        float iconX = sectionStart.x;

        // Speaker icon (larger)
        draw->AddRectFilled(ImVec2(iconX, iconCY - 6), ImVec2(iconX + 7, iconCY + 6),
                            IM_COL32(120, 140, 170, 200), 1.0f);
        draw->AddTriangleFilled(
            ImVec2(iconX + 7, iconCY - 6), ImVec2(iconX + 18, iconCY - 12), ImVec2(iconX + 18, iconCY + 12),
            IM_COL32(120, 140, 170, 200));
        draw->AddTriangleFilled(
            ImVec2(iconX + 7, iconCY - 6), ImVec2(iconX + 7, iconCY + 6), ImVec2(iconX + 18, iconCY + 12),
            IM_COL32(120, 140, 170, 200));
        draw->AddBezierQuadratic(
            ImVec2(iconX + 21, iconCY - 8), ImVec2(iconX + 27, iconCY), ImVec2(iconX + 21, iconCY + 8),
            IM_COL32(120, 140, 170, 120), 1.8f);
        draw->AddBezierQuadratic(
            ImVec2(iconX + 24, iconCY - 12), ImVec2(iconX + 32, iconCY), ImVec2(iconX + 24, iconCY + 12),
            IM_COL32(120, 140, 170, 70), 1.5f);

        // Combo next to icon, vertically centered
        float comboH = ImGui::GetFrameHeight() + 8; // with extra padding
        ImGui::SetCursorScreenPos(ImVec2(iconX + 38, barPos.y + (barH - comboH) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.09f, 0.10f, 0.14f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("##AudioSrc", audioPreview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("System Audio (loopback)", m_selectedAudioDevice == -1)) {
                m_selectedAudioDevice = -1;
            }
            for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                ImGui::PushID(i);
                std::string label = m_audioDevices[i].name;
                if (label.length() > 50) label = label.substr(0, 47) + "...";
                if (ImGui::Selectable(label.c_str(), m_selectedAudioDevice == i)) {
                    m_selectedAudioDevice = i;
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.78f, 1.0f, 0.8f));
            if (ImGui::Selectable("Refresh devices...")) {
                m_audioDevices = VideoRecorder::enumerateAudioDevices();
            }
            ImGui::PopStyleColor();
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        // ── Level meter (stereo bars) ──
        ImGui::SameLine(0, 14);
        {
            ImVec2 meterStart = ImGui::GetCursorScreenPos();
            float meterW = 160.0f;
            float meterTotalH = 20.0f; // total height for both bars
            float barGap = 3.0f;
            float singleH = (meterTotalH - barGap) * 0.5f;
            float meterY = barPos.y + (barH - meterTotalH) * 0.5f;

            // Background tracks
            ImU32 trackBg = IM_COL32(20, 22, 30, 200);
            draw->AddRectFilled(ImVec2(meterStart.x, meterY),
                                ImVec2(meterStart.x + meterW, meterY + singleH), trackBg, 2.0f);
            draw->AddRectFilled(ImVec2(meterStart.x, meterY + singleH + barGap),
                                ImVec2(meterStart.x + meterW, meterY + meterTotalH), trackBg, 2.0f);

            // Draw filled level for each channel
            auto drawBar = [&](float level, float y, float h) {
                float fillW = level * meterW;
                if (fillW < 1.0f) fillW = 1.0f;

                // Gradient: green -> yellow -> red
                int segments = (int)(fillW / 2.0f);
                if (segments < 1) segments = 1;
                for (int s = 0; s < segments; s++) {
                    float t = (float)s / (meterW / 2.0f);
                    float x0 = meterStart.x + s * 2.0f;
                    float x1 = x0 + 1.5f;
                    if (x1 > meterStart.x + fillW) break;

                    int r, g, b;
                    if (t < 0.6f) {
                        // Green
                        r = 30; g = 200; b = 80;
                    } else if (t < 0.8f) {
                        // Yellow
                        float blend = (t - 0.6f) / 0.2f;
                        r = (int)(30 + 220 * blend);
                        g = (int)(200 + 20 * blend);
                        b = (int)(80 - 40 * blend);
                    } else {
                        // Red
                        float blend = (t - 0.8f) / 0.2f;
                        r = (int)(250);
                        g = (int)(220 - 180 * blend);
                        b = (int)(40);
                    }
                    draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + h),
                                        IM_COL32(r, g, b, 220), 0.0f);
                }
            };

            drawBar(m_audioLevelSmoothL, meterY, singleH);
            drawBar(m_audioLevelSmoothR, meterY + singleH + barGap, singleH);

            // Thin border
            draw->AddRect(ImVec2(meterStart.x, meterY),
                          ImVec2(meterStart.x + meterW, meterY + singleH),
                          IM_COL32(60, 70, 90, 80), 2.0f);
            draw->AddRect(ImVec2(meterStart.x, meterY + singleH + barGap),
                          ImVec2(meterStart.x + meterW, meterY + meterTotalH),
                          IM_COL32(60, 70, 90, 80), 2.0f);

            // L/R labels
            draw->AddText(ImVec2(meterStart.x - 12, meterY - 1),
                          IM_COL32(100, 115, 140, 150), "L");
            draw->AddText(ImVec2(meterStart.x - 12, meterY + singleH + barGap - 1),
                          IM_COL32(100, 115, 140, 150), "R");

            ImGui::Dummy(ImVec2(meterW + 4, bigBtnH));
        }
    }

    // ════════════════════════════════════════════════
    //  DIVIDER 2
    // ════════════════════════════════════════════════
    ImGui::SameLine(0, 20);
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float divTop = barPos.y + 18;
        float divBot = barPos.y + barH - 18;
        draw->AddLine(ImVec2(p.x, divTop), ImVec2(p.x, divBot), IM_COL32(255, 255, 255, 20));
        draw->AddLine(ImVec2(p.x + 1, divTop), ImVec2(p.x + 1, divBot), IM_COL32(0, 0, 0, 40));
        ImGui::Dummy(ImVec2(4, bigBtnH));
    }

    // ════════════════════════════════════════════════
    //  STREAM / GO LIVE (right)
    // ════════════════════════════════════════════════
    ImGui::SameLine(0, 16);

    static const int aspectNums[] = { 16, 4, 16, 0 };
    static const int aspectDens[] = { 9,  3, 10, 0 };

    // Reset Y for the big button
    ImGui::SetCursorPosY(padTop);

    if (!m_rtmpOutput.isActive()) {
        bool hasKey = m_streamKeyBuf[0] != '\0';
        float btnW = 160.0f;

        ImVec2 btnMin = ImGui::GetCursorScreenPos();
        ImVec2 btnMax(btnMin.x + btnW, btnMin.y + bigBtnH);
        if (!hasKey) ImGui::BeginDisabled();
        ImGui::InvisibleButton("##LiveBtn", ImVec2(btnW, bigBtnH));
        bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        if (!hasKey) ImGui::EndDisabled();

        // Purple-tinted background
        ImU32 liveBg = act ? IM_COL32(70, 20, 110, 90) :
                       hov ? IM_COL32(55, 15, 85, 60) :
                             IM_COL32(35, 10, 55, 35);
        ImU32 liveBorder = hasKey ? IM_COL32(150, 70, 220, hov ? 150 : 80) :
                                    IM_COL32(80, 40, 100, 40);
        draw->AddRectFilled(btnMin, btnMax, liveBg, bigBtnRound);
        draw->AddRect(btnMin, btnMax, liveBorder, bigBtnRound, 0, 1.5f);

        if (hov && hasKey) {
            draw->AddRect(ImVec2(btnMin.x - 1, btnMin.y - 1), ImVec2(btnMax.x + 1, btnMax.y + 1),
                          IM_COL32(160, 80, 255, 25), bigBtnRound + 1, 0, 3.0f);
        }

        // Broadcast icon (larger concentric circles)
        float iconCX = btnMin.x + 32, iconCY = btnMin.y + bigBtnH * 0.5f;
        ImU32 iconCol = hasKey ? IM_COL32(180, 100, 240, 230) : IM_COL32(100, 60, 130, 100);
        draw->AddCircleFilled(ImVec2(iconCX, iconCY), 5.0f, iconCol);
        draw->AddCircle(ImVec2(iconCX, iconCY), 10.0f, hasKey ? IM_COL32(180, 100, 240, 120) : IM_COL32(100, 60, 130, 50), 0, 1.5f);
        draw->AddCircle(ImVec2(iconCX, iconCY), 15.0f, hasKey ? IM_COL32(180, 100, 240, 50) : IM_COL32(100, 60, 130, 25), 0, 1.0f);

        // Label
        ImU32 textCol = hasKey ? IM_COL32(210, 130, 255, 255) : IM_COL32(120, 70, 150, 140);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f,
                      ImVec2(btnMin.x + 54, btnMin.y + (bigBtnH - ImGui::GetFontSize() * 1.1f) * 0.5f),
                      textCol, "GO LIVE");

        if (ImGui::IsItemClicked() && hasKey) {
            m_rtmpOutput.start(m_streamKeyBuf,
                               zone.warpFBO.width(), zone.warpFBO.height(),
                               aspectNums[m_streamAspect], aspectDens[m_streamAspect], 30);
        }

        if (!hasKey && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Set stream key in the Stream tab");
        }
    } else {
        // ── Live active ──
        float pulse = 0.5f + 0.5f * sinf(time * 3.0f);
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float dotX = cur.x + 12, dotY = barPos.y + barH * 0.38f;

        // Pulsing broadcast dot
        draw->AddCircleFilled(ImVec2(dotX, dotY), 8.0f, IM_COL32(180, 80, 240, (int)(pulse * 255)));
        draw->AddCircle(ImVec2(dotX, dotY), 12.0f, IM_COL32(180, 80, 240, (int)(pulse * 60)), 0, 1.5f);

        // "LIVE" text
        float liveFontSz = ImGui::GetFontSize() * 1.3f;
        draw->AddText(ImGui::GetFont(), liveFontSz,
                      ImVec2(dotX + 18, dotY - liveFontSz * 0.5f),
                      IM_COL32(200, 100, 255, 255), "LIVE");
        float liveW = ImGui::GetFont()->CalcTextSizeA(liveFontSz, FLT_MAX, 0, "LIVE").x;

        // Timer
        int secs = (int)m_rtmpOutput.uptimeSeconds();
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
        draw->AddText(ImGui::GetFont(), liveFontSz,
                      ImVec2(dotX + 24 + liveW, dotY - liveFontSz * 0.5f),
                      IM_COL32(180, 185, 200, 220), timeBuf);
        float timerW = ImGui::GetFont()->CalcTextSizeA(liveFontSz, FLT_MAX, 0, timeBuf).x;

        if (m_rtmpOutput.droppedFrames() > 0) {
            char dropBuf[32];
            snprintf(dropBuf, sizeof(dropBuf), "%d dropped", m_rtmpOutput.droppedFrames());
            draw->AddText(ImVec2(dotX + 30 + liveW + timerW, dotY - ImGui::GetTextLineHeight() * 0.5f),
                          IM_COL32(255, 160, 60, 200), dropBuf);
        }

        // End Stream button below
        float endX = cur.x;
        float endY = barPos.y + barH * 0.58f;
        float endW = 120.0f, endBtnH = 30.0f;
        ImGui::SetCursorScreenPos(ImVec2(endX, endY));
        ImVec2 endMin(endX, endY), endMax(endX + endW, endY + endBtnH);
        ImGui::InvisibleButton("##EndStream", ImVec2(endW, endBtnH));
        bool eh = ImGui::IsItemHovered(), ea = ImGui::IsItemActive();
        draw->AddRectFilled(endMin, endMax,
                            ea ? IM_COL32(110, 30, 140, 90) : eh ? IM_COL32(85, 22, 110, 60) : IM_COL32(50, 15, 65, 35), 6.0f);
        draw->AddRect(endMin, endMax,
                      IM_COL32(170, 80, 220, eh ? 140 : 70), 6.0f, 0, 1.5f);
        const char* endLabel = "End Stream";
        ImVec2 endTextSz = ImGui::CalcTextSize(endLabel);
        draw->AddText(ImVec2(endMin.x + (endW - endTextSz.x) * 0.5f,
                             endMin.y + (endBtnH - endTextSz.y) * 0.5f),
                      IM_COL32(210, 120, 255, 230), endLabel);
        if (ImGui::IsItemClicked()) {
            m_rtmpOutput.stop();
        }
    }

    // ════════════════════════════════════════════════
    //  BPM SYNC (right side of transport bar)
    // ════════════════════════════════════════════════
    {
        float bpmX = barPos.x + barSize.x - 280;
        float bpmY = barPos.y + padTop;

        // BPM display
        ImGui::SetCursorScreenPos(ImVec2(bpmX, bpmY));
        char bpmBuf[32];
        float currentBPM = m_bpmSync.bpm();
        if (currentBPM > 0) {
            snprintf(bpmBuf, sizeof(bpmBuf), "%.1f", currentBPM);
        } else {
            snprintf(bpmBuf, sizeof(bpmBuf), "---");
        }

        // Big BPM number
        draw->AddText(ImGui::GetFont(), 28.0f,
                      ImVec2(bpmX, bpmY + 2),
                      currentBPM > 0 ? IM_COL32(0, 220, 255, 255) : IM_COL32(80, 90, 110, 180),
                      bpmBuf);
        draw->AddText(ImVec2(bpmX, bpmY + 34), IM_COL32(80, 90, 110, 140), "BPM");

        // Beat phase indicator (4 dots)
        float dotX = bpmX + 90;
        float dotY = bpmY + 12;
        for (int b = 0; b < 4; b++) {
            float dotCX = dotX + b * 18.0f;
            int beatInBar = m_bpmSync.beatCount() % 4;
            bool isCurrentBeat = (b == beatInBar) && currentBPM > 0;
            float pulse = isCurrentBeat ? m_bpmSync.beatPulse() : 0.0f;
            float r = 5.0f + pulse * 3.0f;
            ImU32 col = isCurrentBeat ? IM_COL32(0, 220, 255, (int)(150 + pulse * 105)) :
                        (b < beatInBar || !currentBPM) ? IM_COL32(60, 70, 90, 120) :
                                                          IM_COL32(60, 70, 90, 120);
            draw->AddCircleFilled(ImVec2(dotCX, dotY), r, col);
            if (isCurrentBeat && pulse > 0.1f) {
                draw->AddCircle(ImVec2(dotCX, dotY), r + 3, IM_COL32(0, 200, 255, (int)(pulse * 80)), 0, 1.5f);
            }
        }

        // TAP button
        float tapX = bpmX + 90;
        float tapY = bpmY + 28;
        float tapW = 60, tapH = 26;
        ImGui::SetCursorScreenPos(ImVec2(tapX, tapY));
        ImGui::InvisibleButton("##TapBPM", ImVec2(tapW, tapH));
        bool tapHov = ImGui::IsItemHovered();
        ImVec2 tapMin(tapX, tapY), tapMax(tapX + tapW, tapY + tapH);
        draw->AddRectFilled(tapMin, tapMax,
                            tapHov ? IM_COL32(0, 180, 235, 40) : IM_COL32(0, 140, 180, 15), 4.0f);
        draw->AddRect(tapMin, tapMax,
                      IM_COL32(0, 200, 255, tapHov ? 120 : 50), 4.0f, 0, 1.0f);
        ImVec2 tapSz = ImGui::CalcTextSize("TAP");
        draw->AddText(ImVec2(tapX + (tapW - tapSz.x) * 0.5f, tapY + (tapH - tapSz.y) * 0.5f),
                      IM_COL32(0, 200, 255, tapHov ? 255 : 150), "TAP");
        if (ImGui::IsItemClicked()) {
            m_bpmSync.tap();
        }

        // Manual BPM input
        float inputX = bpmX + 160;
        float inputY = bpmY + 8;
        ImGui::SetCursorScreenPos(ImVec2(inputX, inputY));
        ImGui::SetNextItemWidth(60);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.10f, 0.8f));
        float bpmVal = currentBPM;
        if (ImGui::DragFloat("##BPMInput", &bpmVal, 0.5f, 0.0f, 300.0f, "%.0f")) {
            m_bpmSync.setBPM(bpmVal);
        }
        ImGui::PopStyleColor();

        // Reset button
        float rstX = inputX + 65;
        ImGui::SetCursorScreenPos(ImVec2(rstX, inputY + 28));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.1f, 0.1f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.15f, 0.15f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.3f, 0.3f, 0.8f));
        if (ImGui::SmallButton("Reset")) {
            m_bpmSync.setBPM(0);
            m_bpmSync.resetPhase();
        }
        ImGui::PopStyleColor(3);
    }

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
                // Reset zones to single default
                m_zones.clear();
                auto zone = std::make_unique<OutputZone>();
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
    layer->id = m_nextLayerId++;
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
    layer->id = m_nextLayerId++;
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
    layer->id = m_nextLayerId++;
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
    layer->id = m_nextLayerId++;
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
    layer->id = m_nextLayerId++;
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
    layer->id = m_nextLayerId++;
    layer->source = source;
    // Extract just the sender name part (after "MACHINE/")
    size_t slash = senderName.find('/');
    layer->name = "NDI: " + ((slash != std::string::npos) ? senderName.substr(slash + 1) : senderName);

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}
#endif

#ifdef HAS_WHEP
void Application::addWHEPSource(const std::string& whepUrl) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<WHEPSource>();
    if (!source->connect(whepUrl)) {
        std::cerr << "[Scope] WHEP failed, falling back to RTMP\n";
        addScopeRTMP();
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    layer->name = "Scope Stream";

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
}

void Application::addScopeRTMP() {
    // Query Etherea status API for RTMP URL
    std::string statusJson = winHttpRequest("GET", "http://localhost:7860/api/scope/status", "", "");
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

    // Save zones
    json zonesJson = json::array();
    for (auto& zonePtr : m_zones) {
        auto& z = *zonePtr;
        json zj;
        zj["name"] = z.name;
        zj["width"] = z.width;
        zj["height"] = z.height;
        zj["warpMode"] = (int)z.warpMode;
        zj["showAllLayers"] = z.showAllLayers;

        json visIds = json::array();
        for (uint32_t id : z.visibleLayerIds) visIds.push_back(id);
        zj["visibleLayerIds"] = visIds;

        json corners = json::array();
        for (const auto& c : z.cornerPin.corners()) {
            corners.push_back({c.x, c.y});
        }
        zj["cornerPin"] = corners;

        zj["meshWarp"]["cols"] = z.meshWarp.cols();
        zj["meshWarp"]["rows"] = z.meshWarp.rows();
        json meshPoints = json::array();
        for (const auto& p : z.meshWarp.points()) {
            meshPoints.push_back({p.x, p.y});
        }
        zj["meshWarp"]["points"] = meshPoints;

        // OBJ mesh warp
        if (z.objMeshWarp.isLoaded()) {
            json objJson;
            objJson["path"] = z.objMeshWarp.objPath();
            objJson["modelScale"] = z.objMeshWarp.modelScale();
            objJson["modelPosition"] = {z.objMeshWarp.modelPosition().x,
                                         z.objMeshWarp.modelPosition().y,
                                         z.objMeshWarp.modelPosition().z};
            const auto& cam = z.objMeshWarp.camera();
            objJson["camera"]["azimuth"] = cam.azimuth;
            objJson["camera"]["elevation"] = cam.elevation;
            objJson["camera"]["distance"] = cam.distance;
            objJson["camera"]["fovDeg"] = cam.fovDeg;
            objJson["camera"]["target"] = {cam.target.x, cam.target.y, cam.target.z};

            // Save material textured state
            json matsJson = json::array();
            for (const auto& mg : z.objMeshWarp.materials()) {
                json mj;
                mj["name"] = mg.name;
                mj["textured"] = mg.textured;
                matsJson.push_back(mj);
            }
            objJson["materials"] = matsJson;

            zj["objMesh"] = objJson;
        }

        // Output routing
        // Edge blend
        if (z.edgeBlendLeft > 0 || z.edgeBlendRight > 0 || z.edgeBlendTop > 0 || z.edgeBlendBottom > 0) {
            zj["edgeBlendLeft"] = z.edgeBlendLeft;
            zj["edgeBlendRight"] = z.edgeBlendRight;
            zj["edgeBlendTop"] = z.edgeBlendTop;
            zj["edgeBlendBottom"] = z.edgeBlendBottom;
            zj["edgeBlendGamma"] = z.edgeBlendGamma;
        }

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

    // Helper to load warp state into a zone from a JSON object
    auto loadZoneWarp = [](OutputZone& z, const json& zj) {
        if (zj.contains("warpMode")) {
            z.warpMode = (ViewportPanel::WarpMode)zj["warpMode"].get<int>();
        }
        if (zj.contains("cornerPin")) {
            auto& corners = z.cornerPin.corners();
            const auto& cj = zj["cornerPin"];
            for (int i = 0; i < 4 && i < (int)cj.size(); i++) {
                corners[i] = {cj[i][0].get<float>(), cj[i][1].get<float>()};
            }
        }
        if (zj.contains("meshWarp")) {
            int cols = zj["meshWarp"]["cols"].get<int>();
            int rows = zj["meshWarp"]["rows"].get<int>();
            z.meshWarp.setGridSize(cols, rows);
            if (zj["meshWarp"].contains("points")) {
                auto& points = z.meshWarp.points();
                const auto& pj = zj["meshWarp"]["points"];
                for (int i = 0; i < (int)pj.size() && i < (int)points.size(); i++) {
                    points[i] = {pj[i][0].get<float>(), pj[i][1].get<float>()};
                }
            }
        }
        if (zj.contains("objMesh")) {
            const auto& oj = zj["objMesh"];
            if (oj.contains("path")) {
                z.objMeshWarp.loadModel(oj["path"].get<std::string>());
            }
            if (oj.contains("modelScale")) {
                z.objMeshWarp.modelScale() = oj["modelScale"].get<float>();
            }
            if (oj.contains("modelPosition")) {
                z.objMeshWarp.modelPosition() = {
                    oj["modelPosition"][0].get<float>(),
                    oj["modelPosition"][1].get<float>(),
                    oj["modelPosition"][2].get<float>()
                };
            }
            if (oj.contains("camera")) {
                auto& cam = z.objMeshWarp.camera();
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
            // Restore material textured state
            if (oj.contains("materials")) {
                auto& mats = z.objMeshWarp.materials();
                const auto& matsJson = oj["materials"];
                for (const auto& mj : matsJson) {
                    std::string name = mj.value("name", "");
                    bool tex = mj.value("textured", true);
                    for (auto& mg : mats) {
                        if (mg.name == name) {
                            mg.textured = tex;
                            break;
                        }
                    }
                }
            }
        }
        // Recreate FBO with depth if ObjMesh mode
        if (z.warpMode == ViewportPanel::WarpMode::ObjMesh) {
            z.warpFBO.create(z.width, z.height, true);
        }
    };

    int version = j.value("version", 1);

    if (version >= 2 && j.contains("zones")) {
        // v2 format: multiple zones
        m_zones.clear();
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
            loadZoneWarp(*z, zj);

            // Edge blend
            z->edgeBlendLeft = zj.value("edgeBlendLeft", 0.0f);
            z->edgeBlendRight = zj.value("edgeBlendRight", 0.0f);
            z->edgeBlendTop = zj.value("edgeBlendTop", 0.0f);
            z->edgeBlendBottom = zj.value("edgeBlendBottom", 0.0f);
            z->edgeBlendGamma = zj.value("edgeBlendGamma", 2.2f);

            // Output routing
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
        // v1 format: single zone — load warp directly into zone 0
        if (m_zones.empty()) {
            auto z = std::make_unique<OutputZone>();
            z->init();
            m_zones.push_back(std::move(z));
        }
        // Reset to single zone
        while (m_zones.size() > 1) m_zones.pop_back();
        m_activeZone = 0;
        loadZoneWarp(*m_zones[0], j);
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
