#include "app/Application.h"
#include "ui/ParamRow.h"
#include "ui/LucideIcons.h"
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
#include "sources/AudioPresetEngine.h"
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
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#include <unordered_set>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

// WhisperSpeech uses <filesystem> (already included above)

using json = nlohmann::json;

static void glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

// Strip a trailing ".fs" extension (case-insensitive) and uppercase the
// remaining string. Used by the ShaderClaw browser card titles, the
// LayerPanel name display for shader layers, and shader parameter labels —
// keeps the underlying manifest entry / layer->name intact while presenting
// an editorial, all-caps display string.
static std::string shaderDisplayName(const std::string& s) {
    std::string out = s;
    if (out.size() >= 3) {
        std::string tail = out.substr(out.size() - 3);
        for (auto& ch : tail) ch = (char)tolower((unsigned char)ch);
        if (tail == ".fs") out.erase(out.size() - 3);
    }
    for (auto& ch : out) ch = (char)toupper((unsigned char)ch);
    return out;
}

// Flat headline replacing ImGui::CollapsingHeader. No chevron, no
// dropdown — just a bigger bright headline + thin separator. Always
// returns true so the calling `if (...) { content }` pattern keeps
// rendering content. Matches the Properties-panel sectionHeader style.
static bool flatSection(const char* label) {
    ImGui::Dummy(ImVec2(0, 14));
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float headlineSize = ImGui::GetFontSize() * 1.4f;
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImGui::GetFont(), headlineSize, pos,
                IM_COL32(245, 248, 254, 255), label);
    ImGui::Dummy(ImVec2(w, headlineSize + 4.0f));
    float lineY = ImGui::GetCursorScreenPos().y + 2.0f;
    dl->AddLine(ImVec2(pos.x, lineY), ImVec2(pos.x + w, lineY),
                IM_COL32(255, 255, 255, 22), 1.0f);
    ImGui::Dummy(ImVec2(0, 6));
    return true;
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
extern "C" int  EaselMac_IsNativeFullScreen(GLFWwindow*);
extern "C" void EaselMac_ExitNativeFullScreen(GLFWwindow*);
extern "C" int  EaselMac_ConsumeZoomFullscreenRequest();
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

    // EASEL_WINDOW_POS="x,y" pins the editor window — companion to
    // EASEL_PROJECTOR_RECT on single-head appliances, parking the editor in
    // the framebuffer region the physical output doesn't scan out.
    if (const char* wp = getenv("EASEL_WINDOW_POS")) {
        int wx, wy;
        if (sscanf(wp, "%d,%d", &wx, &wy) == 2) glfwSetWindowPos(m_window, wx, wy);
    }

#ifdef __APPLE__
    // Unify the title bar so the ImGui main menu sits alongside the
    // traffic-light buttons (Figma / VS Code style), freeing the row the
    // OS would otherwise reserve for a separate title strip.
    EaselMac_UnifyTitleBar(m_window);
    // Disable macOS native (green-button) fullscreen on the editor window.
    // If the editor is in native fullscreen when a projector opens or an NDI
    // receiver triggers a window/display change, macOS force-exits fullscreen
    // and GLFW aborts on a transient zero content scale (SIGABRT). Easel uses
    // its own borderless fullscreen, so native fullscreen is unneeded here.
    extern void disableNativeFullscreen(GLFWwindow*);
    disableNativeFullscreen(m_window);
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
    // Editor vsync ON by default (see the frame-pacing block in run()): the
    // loop should run at the display's refresh, not free-run. Output pacing
    // is independent — NDI throttles itself, the projector present uses a
    // fence sync on its own context — so editor vsync doesn't gate them.
    glfwSwapInterval(1);
    m_appliedSwapInterval = 1;

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
    // Phase Q v4 — bloom pipeline shaders. Loaded best-effort: if the
    // assets are missing the bloom path silently disables itself.
    if (!m_bloomBrightShader.loadFromFiles("shaders/passthrough.vert",
                                           "shaders/bloom_brightpass.frag")) {
        m_bloomEnabled = false;
    }
    if (!m_bloomBlurShader.loadFromFiles("shaders/passthrough.vert",
                                         "shaders/bloom_blur.frag")) {
        m_bloomEnabled = false;
    }
    if (!m_bloomCompositeShader.loadFromFiles("shaders/passthrough.vert",
                                              "shaders/bloom_composite.frag")) {
        m_bloomEnabled = false;
    }
    // Linear-copy shader for the bloom pipeline's final blit-back. The
    // regular m_passthroughShader would apply ACES + clamp here, double-
    // tonemapping every shader (the fade users were noticing).
    if (!m_linearCopyShader.loadFromFiles("shaders/passthrough.vert",
                                          "shaders/linear_copy.frag")) {
        m_bloomEnabled = false;
    }
    // Warp 4× SS downsample shader. Loads with a graceful fallback: if
    // it fails to compile we'll still have a valid pipeline because the
    // SS path falls back to m_linearCopyShader's single bilinear sample.
    if (!m_warpDownsampleShader.loadFromFiles("shaders/passthrough.vert",
                                              "shaders/warp_downsample.frag")) {
        std::cerr << "Failed to load warp_downsample.frag — falling back "
                     "to linear_copy for warp SS downsample\n";
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

    // Generate white alignment grid texture. Shown INSTEAD of the test
    // pattern while a mask (canvas or layer) is being added/edited so the
    // user can see crisp alignment lines while mapping the masked surface.
    // Same 512x512 footprint as the test pattern so it stretches 1:1 onto
    // the mapped surface using the exact same convention.
    {
        const int gw = 512, gh = 512;       // matches m_testPattern footprint
        const int kGridCellPx = 32;         // cell size; matches test-pattern checker scale (16 cells)
        const int kGridLinePx = 1;          // thin ~1px white lines
        std::vector<uint8_t> pixels(gw * gh * 4);
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                int idx = (y * gw + x) * 4;
                bool onLine = (x % kGridCellPx < kGridLinePx) ||
                              (y % kGridCellPx < kGridLinePx) ||
                              (x >= gw - kGridLinePx) ||
                              (y >= gh - kGridLinePx); // outer border lines
                if (onLine) {
                    pixels[idx + 0] = 255;
                    pixels[idx + 1] = 255;
                    pixels[idx + 2] = 255;
                } else {
                    pixels[idx + 0] = 12;
                    pixels[idx + 1] = 12;
                    pixels[idx + 2] = 16;
                }
                pixels[idx + 3] = 255;
            }
        }
        m_maskGrid.createEmpty(gw, gh);
        m_maskGrid.updateData(pixels.data(), gw, gh);
    }

    // MAPPING-mode calibration patterns — high-res (1024²), black & white,
    // so projector-mapped surfaces show crisp alignment geometry. One texture
    // per dropdown option; order matches kPatternNames in WarpEditor.cpp.
    {
        const int pw = 1024, ph = 1024;
        std::vector<uint8_t> px(pw * ph * 4);
        auto fill = [&](int which) {
            for (int y = 0; y < ph; y++) {
                for (int x = 0; x < pw; x++) {
                    int idx = (y * pw + x) * 4;
                    float nx = x / (float)(pw - 1) * 2.0f - 1.0f;  // -1..1
                    float ny = y / (float)(ph - 1) * 2.0f - 1.0f;
                    bool on = false;
                    switch (which) {
                    case 0: { // Grid
                        const int cell = 64, line = 2;
                        on = (x % cell < line) || (y % cell < line) ||
                             (x >= pw - line) || (y >= ph - line);
                    } break;
                    case 1: { // Checkerboard
                        const int cell = 64;
                        on = (((x / cell) + (y / cell)) % 2) == 0;
                    } break;
                    case 2: { // Crosshair + diagonals + border
                        const int line = 2;
                        bool cross = (std::abs(x - pw / 2) < line) ||
                                     (std::abs(y - ph / 2) < line) ||
                                     (x < line) || (y < line) ||
                                     (x >= pw - line) || (y >= ph - line);
                        bool diag = (std::abs(x - y) < line) ||
                                    (std::abs(x - (pw - 1 - y)) < line);
                        on = cross || diag;
                    } break;
                    case 3: { // Concentric circles
                        float r = std::sqrt(nx * nx + ny * ny) * 8.0f;
                        on = (r - std::floor(r)) < 0.14f;
                    } break;
                    case 4: { // Dots
                        const int cell = 64;
                        int dx = x % cell - cell / 2, dy = y % cell - cell / 2;
                        on = (dx * dx + dy * dy) < 36;  // ~r6 dots
                    } break;
                    default: // Solid White
                        on = true;
                        break;
                    }
                    uint8_t v = on ? 255 : 12;
                    px[idx + 0] = v;
                    px[idx + 1] = v;
                    px[idx + 2] = on ? 255 : 16;
                    px[idx + 3] = 255;
                }
            }
        };
        for (int i = 0; i < kMapPatternCount; i++) {
            fill(i);
            m_mapPatterns[i].createEmpty(pw, ph);
            m_mapPatterns[i].updateData(px.data(), pw, ph);
        }
    }

    // Etherea client — WebSocket for real-time transcript, SSE for hints
    m_ethereaClient.setTranscriptCallback([this](const std::string& text, bool isFinal) {
        // Feeds etherea.latest (full segment), etherea.words (new-words delta),
        // and etherea.recent (rolling last-N-words FIFO).
        pushTranscript("etherea", m_ethereaFeed, text, isFinal);
        if (isFinal && !text.empty()) {
            // Accumulate full transcript (bounded — see DataBus::appendCapped).
            m_dataBus.appendCapped("etherea.transcript", text);
        }
        // Record time for voice decay
        m_voiceLastInputTime = glfwGetTime();
    });
    m_speechState.available = true;

    // Auto-connect to Etherea (no session ID — server gives us the active session)
    m_ethereaClient.connect("http://localhost:7860");

    // Cue: parallel realtime cue harness. Pushes transcript + actions into DataBus.
    m_cueClient.setTranscriptCallback([this](const std::string& text, bool isFinal, const std::string& /*speaker*/) {
        pushCueWords(text, isFinal);   // sets cue.latest + cue.words (new-words delta)
        if (isFinal && !text.empty()) {
            m_dataBus.appendCapped("cue.transcript", text);
        }
    });
    m_cueClient.setActionCallback([this](const CueAction& a) {
        m_dataBus.set("cue.action.type", a.type);
        m_dataBus.set("cue.action.payload", a.payload);
        // But Coach demo: surface the structured feedback fields to shaders/UI.
        if (a.type == "coach.feedback") {
            // payload looks like {"headline":"...", "quote":"...", "feedback":"...", "alternative":"...", "severity":"..."}
            auto extract = [](const std::string& json, const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":\"";
                size_t p = json.find(needle);
                if (p == std::string::npos) return "";
                p += needle.size();
                size_t e = json.find('"', p);
                return e == std::string::npos ? "" : json.substr(p, e - p);
            };
            m_dataBus.set("cue.coach.headline",    extract(a.payload, "headline"));
            m_dataBus.set("cue.coach.quote",       extract(a.payload, "quote"));
            m_dataBus.set("cue.coach.feedback",    extract(a.payload, "feedback"));
            m_dataBus.set("cue.coach.alternative", extract(a.payload, "alternative"));
            m_dataBus.set("cue.coach.severity",    extract(a.payload, "severity"));
        }
    });
    m_cueClient.setPromptCallback([this](const std::string& p, bool /*reset*/) {
        m_dataBus.set("cue.prompt", p);
    });
    m_cueClient.connect("http://localhost:8791", "easel");

    // Record initial monitor count and auto-connect if secondary exists
    m_lastMonitorCount = (int)ProjectorOutput::enumerateMonitors().size();
    if (getenv("EASEL_PROJECTOR_RECT")) {
        // Pinned projector rect (single-head appliance, see ProjectorOutput):
        // always open the projector output; no second monitor will ever appear.
        activeZone().outputDest = OutputDest::Fullscreen;
        activeZone().outputMonitor = 0;
    } else if (m_projectorAutoConnect && m_lastMonitorCount > 1) {
        int sec = ProjectorOutput::findSecondaryMonitor(m_window);
        if (sec >= 0) {
            activeZone().outputDest = OutputDest::Fullscreen;
            activeZone().outputMonitor = sec;
        }
    }

#ifdef HAS_NDI
    // Stash the (default) network selection before the FIRST init so the config
    // file + NDI_CONFIG_DIR exist before initialize(); a loaded project re-applies.
    NDIRuntime::setPendingNetworkSettings(m_ndiNetwork);
    NDIRuntime::instance().init();
    if (NDIRuntime::instance().isAvailable()) {
        // Create persistent finder — it accumulates sources over time via mDNS
        m_ndiFinder.create();
        m_ndiSources = m_ndiFinder.sources();
        // Auto-start composition output (suppressed on receive-only boxes:
        // daisy sets EASEL_NO_NDI_OUTPUT=1 so it never advertises NDI senders)
        if (!getenv("EASEL_NO_NDI_OUTPUT")) m_ndiOutput.create("Lu");
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
    m_shaderRatings.load();
    std::cout << "[ShaderRatings] " << m_shaderRatings.needsImprovementCount()
              << " shaders rated < 5 (improvement candidates)" << std::endl;
    m_shaderPresets.load();
        }
    }

    // Auto-load the default project when it exists. The landing page still
    // covers the blank-start case, but skipping auto-load entirely was a
    // DATA-LOSS trap for the live/agent workflow: a restart came up blank
    // and the 30s crash-recovery auto-save then overwrote default.easel
    // with the blank state, destroying the show project (2026-06-12).
    {
        std::string defaultPath = defaultProjectPath();
        if (std::filesystem::exists(defaultPath)) {
            loadProject(defaultPath);
            m_autoLoadedProject = true;
            std::cout << "[Easel] Auto-loaded default project" << std::endl;
        } else {
            std::cout << "[Easel] Starting blank (landing page active)" << std::endl;
        }
    }

    // Init 3D stage view
    m_stageView.init();
    // Wire the Stage Setup section into the Properties panel — surfaces
    // displays/projectors/surfaces inspector under "Setup" only when
    // sMode == Stage.
    m_propertyPanel.setStageView(&m_stageView);
    m_propertyPanel.setTimeline(&m_timeline);  // Phase C: enables keyframe diamonds
    // Surfaces LAYERS / + Add New Layer / current-layer nav at the top of
    // the parameters panel — reuses the LayerPanel signal flags + the
    // shared selected-layer index (no duplicated layer logic).
    m_propertyPanel.setLayerNav(&m_layerPanel, &m_selectedLayer);
    // Sticky source-quick-bar at the top of the Properties panel — needs
    // the UIManager pointer so its 4 icons (Shader / Mic / Cam / Win) can
    // switch the Sources dock tab via focusSourcesTab().
    m_propertyPanel.setUIManager(&m_ui);

    // Auto-start OSC receiver on port 9000
    m_oscManager.startReceiver(9000);
    m_oscManager.setSendTarget("127.0.0.1", 9001);

    // Pro DJ Link — discover Pioneer CDJs on the local network
    m_prodjlink.start();

    loadRecentProjectsList();

    return true;
}

void Application::run() {
    while (!glfwWindowShouldClose(m_window)) {
        double frameStart = glfwGetTime();
        glfwPollEvents();

        // ── Esc handling ──────────────────────────────────────────────
        // Edge-triggered, deferred-exit. Calling glfwSetWindowMonitor
        // synchronously inside the input branch was crashing on macOS
        // because AppKit tears down + rebuilds the NSWindow's content
        // view mid-frame, and the next ImGui pass touched dead GL/dock
        // state. Fix: when Esc is pressed, set a pending flag; at the
        // TOP of the next frame (before any rendering / dock setup),
        // run the actual fullscreen exit then `continue` so macOS
        // gets a clean frame to settle. We also suppress the handler
        // entirely while in macOS native fullscreen (green-button) —
        // Esc has no defined exit there and trying to call
        // glfwSetWindowMonitor against an AppKit-managed FS window is
        // exactly the case that was crashing.
        {
            // 1. Drain any pending fullscreen exit from the previous frame.
            if (m_pendingExitFullscreen) {
                m_pendingExitFullscreen = false;
                if (m_editorFullscreen) {
                    m_presentMode = false;
                    // Use the borderless toggle so geometry AND window
                    // decorations are restored (it set GLFW_DECORATED=false
                    // on the way in; a raw glfwSetWindowMonitor would leave
                    // the windowed app borderless).
                    toggleEditorFullscreen();
                }
                continue;
            }

            static bool escWasPressed = false;
            bool escNow = glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            bool escEdge = escNow && !escWasPressed;
            escWasPressed = escNow;

#ifdef __APPLE__
            // When AppKit owns the FS state, route Esc through AppKit's own
            // toggleFullScreen — glfwSetWindowMonitor against a native-FS
            // NSWindow races the AppKit transition and crashes.
            bool nativeFs = EaselMac_IsNativeFullScreen(m_window) != 0;
            if (nativeFs && escEdge && !ImGui::GetIO().WantTextInput) {
                EaselMac_ExitNativeFullScreen(m_window);
                escEdge = false;
                continue;
            }
            if (nativeFs) escEdge = false;
#endif

            if (escEdge && !ImGui::GetIO().WantTextInput) {
                if (m_presentMode) {
                    // First Esc out of presentation returns to fullscreen WITH UI.
                    m_presentMode = false;
                    continue;
                }
                if (m_editorFullscreen) {
                    // Defer one frame — actual exit runs at top of next loop.
                    m_pendingExitFullscreen = true;
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

        // F11       = toggle APP fullscreen — borderless, editor UI stays visible.
        // Shift+F11 = toggle presentation mode — active zone OUTPUT fills the
        //             screen with NO editor UI (the old F11 behavior). Entering
        //             present also goes fullscreen if it isn't already.
        // Esc steps out of present first, then out of fullscreen. F11 routes
        // through the borderless toggleEditorFullscreen() — same path as the
        // on-screen "Fullscreen" button — which avoids the exclusive-fullscreen
        // video-mode-set black flash/stall the old inline path caused.
        {
            static bool f11WasPressed = false;
            bool f11Now = glfwGetKey(m_window, GLFW_KEY_F11) == GLFW_PRESS;
            if (f11Now && !f11WasPressed) {
                bool shift = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                if (shift) {
                    if (!m_presentMode) {
                        if (!m_editorFullscreen) toggleEditorFullscreen();
                        m_presentMode = true;
                    } else {
                        m_presentMode = false;  // back to fullscreen WITH UI
                    }
                } else {
                    m_presentMode = false;       // plain fullscreen always shows UI
                    toggleEditorFullscreen();
                }
            }
            f11WasPressed = f11Now;
        }

#ifdef __APPLE__
        // Green traffic-light button → Easel's borderless app fullscreen (same
        // path as F11). Native macOS fullscreen stays disabled (it SIGABRTs
        // when a projector/display change force-exits it), so the green button
        // is routed here instead of doing a plain window zoom.
        if (EaselMac_ConsumeZoomFullscreenRequest()) {
            m_presentMode = false;
            toggleEditorFullscreen();
        }
#endif

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
        if (m_projectorAutoConnect && !getenv("EASEL_PROJECTOR_RECT")) {
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
                // Mixer mode: drain mixed mono from mixer thread → feed to analyzer.
                // The mixer is the sample source here, so the analyzer must not
                // also open its own ScreenCaptureKit/CoreAudio capture.
                m_audioAnalyzer.setWantsSystemAudio(false);
                float monoBuf[4096];
                int count = m_audioMixer.drainMixedMono(monoBuf, 4096);
                if (count > 0) {
                    m_audioAnalyzer.feedSamples(monoBuf, count);
                }
            } else {
                // Legacy single-device mode
                m_audioAnalyzer.setDevice(m_selectedAudioDevice);
#ifdef HAS_FFMPEG
                // Mic-vs-loopback level differences are handled by the
                // analyzer's dB-domain AGC now (EaselAudio §3), so device
                // changes just reset the manual trim to unity — the old 4x
                // mic seed is gone. The Gain slider remains a pure manual
                // trim in front of the AGC.
                {
                    static int sGainAppliedFor = -999;
                    if (m_selectedAudioDevice != sGainAppliedFor) {
                        m_audioAnalyzer.inputGain() = 1.0f;
                        sGainAppliedFor = m_selectedAudioDevice;
                    }
                }
                // m_audioDevices is only populated when FFmpeg is available
                // (see Application.h — lives in the HAS_FFMPEG block).
                if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)m_audioDevices.size()) {
                    bool isCapture = m_audioDevices[m_selectedAudioDevice].isCapture;
                    m_audioAnalyzer.setDeviceId(m_audioDevices[m_selectedAudioDevice].id,
                                                isCapture);
                    // Explicit mic device → CoreAudio capture path. Any other
                    // selection (loopback device) needs the ScreenCaptureKit
                    // system-audio path on macOS.
                    m_audioAnalyzer.setWantsSystemAudio(!isCapture);
                } else {
                    // "System Audio (loopback)" (-1, the default): there is no
                    // explicit capture device, so opt the analyzer into the
                    // ScreenCaptureKit system-audio path. Without this the macOS
                    // capture silently bails (cleanupCapture) and every audio
                    // uniform stays 0 — i.e. no audio reactivity at all.
                    m_audioAnalyzer.setDeviceId("", false);
                    m_audioAnalyzer.setWantsSystemAudio(true);
                }
#else
                m_audioAnalyzer.setDeviceId("", false);
                m_audioAnalyzer.setWantsSystemAudio(true);
#endif
            }
            m_audioAnalyzer.update(dt);
            m_audioRMS = m_audioAnalyzer.smoothedRMS();
            // EaselAudio: the detected tempo (autocorrelation + confidence)
            // phase-locks the BPMSync clock; tap tempo and OSC /easel/bpm
            // remain overrides. Detected onsets pull the phase (light PLL).
            m_bpmSync.feedDetected(m_audioAnalyzer.detectedBPM(),
                                   m_audioAnalyzer.detectedBPMConfidence());
            if (m_audioAnalyzer.beatDetected()) {
                m_bpmSync.beatHint(m_audioAnalyzer.detectedBPMConfidence());
            }
            m_bpmSync.update(dt);

            // ── EaselAudio → agent-SDK telemetry (spec §6, SDK issue 12) ──
            // Emit each bus float as OSC /easel/audio/<uniformName> toward
            // the SDK listener (send target set in init: 127.0.0.1:9001) at
            // ~20Hz — delivery decoupled from render rate. UDP fire-and-
            // forget: costs nothing when nobody listens.
            {
                static float sBusEmitAcc = 1.0f;   // emit immediately on start
                sBusEmitAcc += dt;
                constexpr float kBusEmitPeriod = 0.05f;   // 20 Hz
                if (sBusEmitAcc >= kBusEmitPeriod) {
                    sBusEmitAcc = std::fmod(sBusEmitAcc, kBusEmitPeriod);
                    AudioAnalyzer& a = m_audioAnalyzer;
                    auto emit = [&](const char* name, float v) {
                        m_oscManager.sendFloat(std::string("/easel/audio/") + name, v);
                    };
                    // Core (legacy quartet + existing bus — AGC'd)
                    emit("audioLevel", a.smoothedRMS());
                    emit("audioBass", a.bass());
                    emit("audioMid", (a.lowMid() + a.highMid()) * 0.5f);
                    emit("audioHigh", a.treble());
                    emit("audioSub", a.sub());
                    emit("audioTreble", a.treble());
                    emit("audioEnergy", a.energy());
                    emit("audioBrightness", a.brightness());
                    // Events
                    emit("audioPunch", a.punch());
                    emit("audioBeatPulse", m_bpmSync.beatPulse());
                    emit("audioOnset", a.onset());
                    emit("audioBeat", a.beatDecay());
                    // Temperament matrix
                    emit("audioBassHit", a.bassHit());
                    emit("audioMidHit", a.midHit());
                    emit("audioHighHit", a.highHit());
                    emit("audioBassPresence", a.bassPresence());
                    emit("audioMidPresence", a.midPresence());
                    emit("audioHighPresence", a.highPresence());
                    // Schema name for the mix presence is audioPresence; the
                    // GLSL uniform is audioLevelPresence (vec4 name clash) —
                    // emit both so schema-driven and shader-driven listeners
                    // each find their key.
                    emit("audioPresence", a.levelPresence());
                    emit("audioLevelPresence", a.levelPresence());
                    emit("audioBassTime", a.bassTime());
                    emit("audioMidTime", a.midTime());
                    emit("audioHighTime", a.highTime());
                    emit("audioTime", a.levelTime());
                    // Rhythm bus
                    {
                        bool manual = m_bpmSync.source() == BPMSync::Source::Manual;
                        float conf = manual ? 1.0f : a.detectedBPMConfidence();
                        emit("audioBPM", m_bpmSync.bpm());
                        emit("audioBPMConfidence", conf);
                        emit("audioBeatPhase", m_bpmSync.beatPhase());
                        emit("audioBarPhase", m_bpmSync.barPhase());
                        emit("audioPhase2", m_bpmSync.phaseN(2));
                        emit("audioPhase4", m_bpmSync.phaseN(4));
                        emit("audioPhase8", m_bpmSync.phaseN(8));
                        emit("audioPhase16", m_bpmSync.phaseN(16));
                        float t = std::min(std::max((conf - 0.25f) / 0.20f, 0.0f), 1.0f);
                        float lock01 = t * t * (3.0f - 2.0f * t);
                        emit("audioOnBeat", m_bpmSync.onBeat() * lock01 + a.onset() * (1.0f - lock01));
                        emit("audioToggleOnBeat", m_bpmSync.toggleOnBeat());
                    }
                    // Tier-1 pseudo-stems + temperaments
                    static const char* kStemNames[AudioAnalyzer::StemCount] = {
                        "stemBass", "stemDrums", "stemMelody", "stemAir", "stemVocal" };
                    static const char* kStemHitNames[AudioAnalyzer::StemCount] = {
                        "stemBassHit", "stemDrumsHit", "stemMelodyHit", "stemAirHit", "stemVocalHit" };
                    static const char* kStemPresNames[AudioAnalyzer::StemCount] = {
                        "stemBassPresence", "stemDrumsPresence", "stemMelodyPresence",
                        "stemAirPresence", "stemVocalPresence" };
                    for (int s = 0; s < AudioAnalyzer::StemCount; s++) {
                        emit(kStemNames[s], a.stem(s));
                        emit(kStemHitNames[s], a.stemHit(s));
                        emit(kStemPresNames[s], a.stemPresence(s));
                    }
                }
            }

            // Per-zone push-to-talk mics: multi-floor/multi-room installs can
            // give each zone its own independent mic input. Capture only runs
            // while a zone is both mic-enabled AND actively held (push-to-talk),
            // so the device is opened/closed around each talk-spurt rather than
            // sitting open for every configured zone all the time.
            for (auto& zonePtr : m_zones) {
                OutputZone& z = *zonePtr;
                if (z.micEnabled && z.pushToTalkActive) {
                    z.micAnalyzer.setDeviceId(z.micDeviceId, true);
                    z.micAnalyzer.setWantsSystemAudio(false);
                    z.micAnalyzer.update(dt);
                } else {
                    z.micAnalyzer.stopCapture();
                }
            }

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

            // Phase C: sample animation lanes once per frame, apply resolved
            // values back to the layer fields the lanes target. For v1 we
            // route only "opacity" — extending the dispatch table covers
            // position/scale/rotation/shader uniforms in v2.
            m_animatedParams.clear();
            m_timeline.sampleAnimatedParams(m_animatedParams);
            for (int li = 0; li < m_layerStack.count(); li++) {
                auto& lp = m_layerStack[li];
                if (!lp) continue;
                auto it = m_animatedParams.find(
                    Timeline::animKey(lp->id, "opacity"));
                if (it != m_animatedParams.end()) {
                    lp->opacity = std::max(0.0f, std::min(1.0f, it->second));
                }
            }

            // REC-button recording is indefinite/live (no Work-Area
            // auto-stop); the export flow below only fires for an explicit
            // startTimelineExport(), which sets m_timelineExporting.
            if (m_timelineExporting && m_timeline.playhead() >= m_timelineExportEnd - 1e-3) {
#ifdef HAS_FFMPEG
                if (m_recorder.isActive()) m_recorder.stop();
#endif
                m_timeline.pause();
                m_timelineExporting = false;
            }
        }

        // Park heavy sources (video decode threads, capture devices, NDI
        // receivers) that are reachable only from undo/redo snapshots —
        // without this a replaced/deleted layer's source stayed fully live
        // until 50 newer undo pushes rolled it off. Sources self-revive in
        // their update() if a restore puts them back in the live stack.
        m_undoStack.suspendOrphanedSources(m_layerStack);

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

        if (m_presentMode) {
            // Presentation mode: draw active zone output fullscreen, no UI.
            // (Plain app-fullscreen — m_editorFullscreen — falls through to the
            // else branch below so the full editor UI renders across the screen.)
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
            glClearColor(0x14/255.0f, 0x14/255.0f, 0x14/255.0f, 1.0f); // #141414 — matches Canvas WindowBg so the launch is one continuous tone
            glClear(GL_COLOR_BUFFER_BIT);

            m_ui.beginFrame();

            // Cmd/Ctrl + Shift + 0 — global "RESET EVERYTHING" escape hatch.
            // Lives OUTSIDE the WantTextInput gate so it works no matter
            // where keyboard focus is — this is the user's lifeline when
            // the editor preview is in an unrecoverable state.
            {
                static bool sResetAllPrev = false;
                bool gCtrl = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_LEFT_SUPER)   == GLFW_PRESS ||
                             glfwGetKey(m_window, GLFW_KEY_RIGHT_SUPER)  == GLFW_PRESS;
                bool gShift = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                              glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                bool gZero  = glfwGetKey(m_window, GLFW_KEY_0) == GLFW_PRESS;
                bool resetAllNow = gCtrl && gShift && gZero;
                if (resetAllNow && !sResetAllPrev) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer);
                    m_viewportPanel.resetZoom();
                    std::array<glm::vec2, 4> def = {{
                        {-1.0f, -1.0f}, {1.0f, -1.0f},
                        {1.0f,  1.0f}, {-1.0f, 1.0f}
                    }};
                    for (auto& mp : m_mappings) {
                        if (mp) mp->cornerPin.setCorners(def);
                    }
                    if (m_selectedLayer >= 0
                        && m_selectedLayer < m_layerStack.count()) {
                        if (auto l = m_layerStack[m_selectedLayer]) {
                            l->position = {0.0f, 0.0f};
                            l->scale    = {1.0f, 1.0f};
                            l->rotation = 0.0f;
                            l->anchor   = {0.0f, 0.0f};
                            l->flipH = false; l->flipV = false;
                        }
                    }
                }
                sResetAllPrev = resetAllNow;
            }

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
        m_cueClient.poll();

        // Push hints and prompt from Etherea into data bus
        {
            auto hints = m_ethereaClient.hints();
            for (int i = 0; i < 3 && i < (int)hints.size(); i++)
                m_dataBus.set("etherea.hint." + std::to_string(i), hints[i]);
            std::string prompt = m_ethereaClient.prompt();
            if (!prompt.empty())
                m_dataBus.set("etherea.prompt", prompt);
        }

#ifdef __APPLE__
        // Continuous mic: keep the recognizer always-on so the user can keep
        // talking and the transcript keeps growing in shaders bound to
        // cue.transcript. Two paths land here:
        //   1) onFinal set m_voiceRestartPending — Apple closed the task,
        //      we tear down and restart for the next utterance.
        //   2) Mic isn't running yet (initial boot before TCC granted, or
        //      a manual stop) — try to start once permission is available.
        if (m_voiceContinuous) {
            if (m_voiceRestartPending && m_voiceListening) {
                m_voiceRestartPending = false;
                stopVoiceRecording();
                startVoiceRecording();
            } else if (m_voiceListening && !m_voiceRecognizer.isRecording()) {
                // The engine start is async (it can block seconds inside
                // CoreAudio, so it runs off-thread); a failed start clears
                // the recognizer's flag — resync so the retry below runs.
                m_voiceListening = false;
            } else if (!m_voiceListening && m_voiceRecognizer.available() &&
                       glfwGetTime() >= m_voiceStartRetryAt) {
                startVoiceRecording();
                // Backoff so a mic that can't start doesn't get hammered
                // with a CoreAudio bring-up attempt every frame.
                m_voiceStartRetryAt = glfwGetTime() + 3.0;
            }
        }
#endif

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

        // Apply data bus bindings to shader text params. Skip when the
        // bound value is empty — otherwise the binding fires every frame
        // before any transcript arrives and blanks the shader's default
        // `msg` (e.g. text_clusters.fs's "FLUX MODULAR SOFT CELLS…"),
        // making the shader render nothing until the user starts the mic
        // or the cue session pushes its first event.
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
                    if (val.empty()) {
                        // No active message — reset age so shaders that
                        // gate on `msgAge >= 0` know to render empty.
                        if (paramName == "msg") shader->setMsgAge(-1.0f);
                        break;
                    }

                    // Typewriter reveal for `msg` text inputs on shaders
                    // that don't do their own reveal. New utterances type in
                    // char-by-char; partial transcript updates that extend
                    // the prior value continue the in-progress reveal
                    // (no restart). text_typewriter.fs is skipped — it
                    // animates the reveal in GLSL already.
                    if (paramName == "msg") {
                        std::string srcPath = shader->sourcePath();
                        size_t slash2 = srcPath.find_last_of("/\\");
                        std::string base = (slash2 != std::string::npos)
                            ? srcPath.substr(slash2 + 1) : srcPath;
                        // Per-dataKey typewriter state shared across shaders.
                        static std::unordered_map<std::string,
                            std::pair<std::string, double>> tw;
                        auto& slot = tw[dataKey];
                        const std::string& lastTarget = slot.first;
                        // Decide whether `val` is the SAME utterance as the
                        // last target (continue the in-progress reveal) or a
                        // genuinely NEW utterance (restart the timer).
                        //
                        // Live speech doesn't only extend partials — it
                        // *revises* the tail as recognition firms up
                        // ("I think" -> "I thought" -> "I think we"). A
                        // strict prefix test treats every revision as a new
                        // utterance, so the timer (and msgAge) resets on
                        // every partial and all text shaders strobe. Instead,
                        // compare the longest common prefix: if only the tail
                        // (last word or two) changed, it's the same utterance
                        // being re-recognized — keep the original start time.
                        // cue.latest *replaces* on a new utterance, so a real
                        // new utterance diverges at/near the start.
                        size_t cp = 0;
                        size_t cmpN = std::min(val.size(), lastTarget.size());
                        while (cp < cmpN && val[cp] == lastTarget[cp]) ++cp;
                        // Max tail length (chars) we still treat as an
                        // in-utterance revision rather than a new utterance
                        // (~3-4 words of recognizer churn).
                        constexpr size_t kTailReviseMax = 24;
                        bool sameUtterance =
                            !lastTarget.empty() && cp > 0 &&
                            (lastTarget.size() - cp) <= kTailReviseMax;
                        if (!sameUtterance) {
                            slot.second = glfwGetTime();
                        }
                        slot.first = val;

                        // Push msgAge to every shader bound on `msg`, so
                        // both typewriter and non-typewriter shaders can
                        // animate around the utterance window.
                        float age = (float)(glfwGetTime() - slot.second);
                        shader->setMsgAge(age);

                        if (base != "text_typewriter.fs") {
                            const double cps = 28.0;
                            size_t reveal = (size_t)((double)age * cps);
                            if (reveal > val.size()) reveal = val.size();
                            if (reveal == 0) break; // keep prior frame's msg
                            val = val.substr(0, reveal);
                        }
                    }

                    for (auto& c : val) c = (char)toupper((unsigned char)c);
                    shader->setText(paramName, val);
                    break;
                }
            }
        }

        // Frame pacing. "Uncapped (vsync)" (m_targetFPS == 0) engages vsync so
        // the loop runs at the editor display's refresh (~60Hz) instead of
        // free-running at 300+fps — that surplus was pure wasted GPU and heat,
        // and stepped GPU sims (the fluid solver's 20 pressure iterations) far
        // more often than the display could show, with no visual gain (the sim
        // is wall-clock-dt driven, so its speed is unchanged). An explicit FPS
        // target turns vsync OFF and busy-waits to that rate below.
        {
            int wantInterval = (m_targetFPS > 0.0f) ? 0 : 1;
            if (wantInterval != m_appliedSwapInterval) {
                glfwSwapInterval(wantInterval); // main context is current here
                m_appliedSwapInterval = wantInterval;
            }
        }

        glfwSwapBuffers(m_window);
        static bool s_maximized = false;
        if (!s_maximized) { glfwMaximizeWindow(m_window); s_maximized = true; }

        // Explicit FPS target (vsync off above): busy-wait the remainder of the
        // frame so we hold ~1/target sec.
        //
        // On Apple Silicon the GL-on-Metal compat layer ignores the swap
        // interval — flushBuffer submits and returns immediately, so vsync-by-
        // default doesn't actually block there and the loop free-runs: a full
        // core spent re-rendering an unchanged canvas, dropped frames with
        // nothing playing. Treat 0 as "pace to the display, capped at 60":
        // on a 120Hz ProMotion panel the fixed per-frame cost of the GL swap
        // submit alone eats most of an 8.3ms budget, so 120 pacing runs hot
        // with zero headroom. 60 is the output pipeline's native rate (NDI /
        // recorder); anyone who wants 120 can pick it in Canvas → Target.
        float effectiveFPS = m_targetFPS;
#ifdef __APPLE__
        if (effectiveFPS <= 0.0f) {
            static float sDisplayHz = [] {
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
                return (vm && vm->refreshRate > 0) ? (float)vm->refreshRate : 60.0f;
            }();
            effectiveFPS = std::min(sDisplayHz, 60.0f);
        }
#endif
        if (effectiveFPS > 0.0f) {
            double frameDur = 1.0 / (double)effectiveFPS;
            double remain   = frameDur - (glfwGetTime() - frameStart);
            if (remain > 0.0) {
                // Sleep most of it, then spin the last ~1ms for accuracy.
                if (remain > 0.002)
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(remain - 0.001));
                while (glfwGetTime() - frameStart < frameDur) { /* spin */ }
            }
        }
    }

    // ── Closing animation ────────────────────────────────────────────────────
    // User clicked the window close button. All blocking cleanup (thread joins,
    // network disconnect, file I/O) runs on a background thread so the window
    // stays alive and shows a spinner with a step label until everything is done.
    static const int kCloseSteps = 6;
    static const char* kCloseLabels[kCloseSteps] = {
        "Saving project...",
        "Stopping audio...",
        "Stopping video output...",
        "Disconnecting services...",
        "Releasing outputs...",
        "Finishing up...",
    };

    m_closing.store(true);
    m_closingStep.store(0);
    m_closingDone.store(false);
    m_closingThread = std::thread([this]() {
        m_closingStep.store(0);
        { std::string p = defaultProjectPath(); saveProject(p); }

        m_closingStep.store(1);
        m_audioMixer.stop();

        m_closingStep.store(2);
#ifdef HAS_FFMPEG
        m_recorder.stop();
        m_rtmpOutput.stop();
        cleanupAudioMeter();
#endif

        m_closingStep.store(3);
        m_ethereaClient.disconnect();
        m_cueClient.disconnect();

        m_closingStep.store(4);
#ifdef HAS_NDI
        for (int i = 0; i < m_layerStack.count(); i++)
            m_layerStack[i]->ndiSender.destroy();
        for (auto& zp : m_zones) zp->ndiOutput.destroy();
        m_ndiOutput.destroy();
        NDIRuntime::instance().shutdown();
#endif
#ifdef HAS_SPOUT
        for (auto& zp : m_zones) zp->spoutOutput.destroy();
        m_spoutOutput.destroy();
#endif

        m_closingStep.store(5);
#ifdef HAS_OPENCV
        m_scanner.cancelScan();
        m_webcam.close();
#endif

        m_closingDone.store(true);
    });

    while (!m_closingDone.load()) {
        glfwPollEvents();

        glViewport(0, 0, m_windowWidth, m_windowHeight);
        glClearColor(0.03f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_ui.beginFrame();

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* fg = ImGui::GetForegroundDrawList();

        // Dark overlay
        fg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(8, 10, 16, 230));

        float t  = (float)glfwGetTime();
        float cx = io.DisplaySize.x * 0.5f;
        float cy = io.DisplaySize.y * 0.5f - 28.0f;
        float r  = 26.0f;

        // ── Easel icon with progressive stick-figure painting ───────────────
        int step = m_closingStep.load();
        if (step < 0) step = 0;
        if (step >= kCloseSteps) step = kCloseSteps - 1;

        ImU32 iconCol  = IM_COL32(200, 218, 255, 235);
        ImU32 dimCol   = IM_COL32(140, 160, 210, 140);
        float sw  = 2.2f;
        float sz  = 52.0f;

        // ── Easel frame ──────────────────────────────────────────────────
        // Canvas (portrait): centered slightly above mid, shifted up for labels
        float canW = sz * 0.86f,  canH = sz * 1.08f;
        float cL = cx - canW * 0.5f, cR = cx + canW * 0.5f;
        float cT = cy - sz * 1.05f,  cB = cT + canH;
        float fY = cB + sz * 0.80f;   // floor

        // Subtle canvas fill so figure stands out
        fg->AddRectFilled(ImVec2(cL, cT), ImVec2(cR, cB), IM_COL32(18, 24, 40, 210), 3.0f);
        // Canvas border
        fg->AddRect(ImVec2(cL, cT), ImVec2(cR, cB), iconCol, 3.0f, 0, sw);

        // Top ledge / canvas rail
        float lipY = cT - sz * 0.06f;
        fg->AddLine(ImVec2(cL - sz * 0.06f, lipY), ImVec2(cR + sz * 0.06f, lipY), iconCol, sw * 1.15f);

        // A-frame back legs — start at ledge corners, spread to floor
        float legSpan = sz * 1.05f;
        float legTopL = cL - sz * 0.06f, legTopR = cR + sz * 0.06f;
        fg->AddLine(ImVec2(legTopL, lipY), ImVec2(cx - legSpan, fY), iconCol, sw);
        fg->AddLine(ImVec2(legTopR, lipY), ImVec2(cx + legSpan, fY), iconCol, sw);

        // Cross-brace at 46% down between back legs
        float bk = 0.46f;
        float cbLx = legTopL + bk * (cx - legSpan - legTopL);
        float cbRx = legTopR + bk * (cx + legSpan - legTopR);
        float cbY  = lipY    + bk * (fY - lipY);
        fg->AddLine(ImVec2(cbLx, cbY), ImVec2(cbRx, cbY), dimCol, sw * 0.9f);

        // Front support leg — single center line, slight forward angle
        fg->AddLine(ImVec2(cx, cB), ImVec2(cx + sz * 0.07f, fY), iconCol, sw);

        // Foot nubs
        float nub = sz * 0.05f;
        fg->AddLine(ImVec2(cx - legSpan - nub, fY), ImVec2(cx - legSpan + nub, fY), iconCol, sw * 1.2f);
        fg->AddLine(ImVec2(cx + legSpan - nub, fY), ImVec2(cx + legSpan + nub, fY), iconCol, sw * 1.2f);
        fg->AddLine(ImVec2(cx + sz*0.07f - nub, fY), ImVec2(cx + sz*0.07f + nub, fY), iconCol, sw * 1.2f);

        // ── Stick figure painted progressively onto canvas ───────────────
        float figH = cB - cT;
        // Step 1: head
        if (step >= 1) {
            float hR = figH * 0.115f, hCy = cT + figH * 0.19f;
            fg->AddCircle(ImVec2(cx, hCy), hR, iconCol, 18, sw);
        }
        // Step 2: body
        float bodyT = cT + figH * 0.32f, bodyB = cT + figH * 0.65f;
        if (step >= 2) fg->AddLine(ImVec2(cx, bodyT), ImVec2(cx, bodyB), iconCol, sw);
        // Step 3: arms (raised slightly — painting gesture)
        if (step >= 3) {
            float am   = cT + figH * 0.42f;
            float aLen = figH * 0.26f;
            fg->AddLine(ImVec2(cx, am), ImVec2(cx - aLen * 0.9f, am - aLen * 0.30f), iconCol, sw);
            fg->AddLine(ImVec2(cx, am), ImVec2(cx + aLen * 0.9f, am - aLen * 0.30f), iconCol, sw);
        }
        // Step 4: left leg
        if (step >= 4) {
            float lLen = figH * 0.28f;
            fg->AddLine(ImVec2(cx, bodyB), ImVec2(cx - lLen * 0.44f, bodyB + lLen), iconCol, sw);
        }
        // Step 5: right leg + feet
        if (step >= 5) {
            float lLen = figH * 0.28f;
            fg->AddLine(ImVec2(cx, bodyB), ImVec2(cx + lLen * 0.44f, bodyB + lLen), iconCol, sw);
            // small feet
            float fOff = sz * 0.04f;
            fg->AddLine(ImVec2(cx - lLen*0.44f, bodyB+lLen),
                        ImVec2(cx - lLen*0.44f - fOff, bodyB+lLen), iconCol, sw*0.9f);
            fg->AddLine(ImVec2(cx + lLen*0.44f, bodyB+lLen),
                        ImVec2(cx + lLen*0.44f + fOff, bodyB+lLen), iconCol, sw*0.9f);
        }

        // Step label
        const char* label = kCloseLabels[step];
        ImVec2 ts = ImGui::CalcTextSize(label);
        fg->AddText(ImVec2(cx - ts.x * 0.5f, fY + 14.0f),
                    IM_COL32(185, 200, 230, 200), label);

        // Thin progress bar
        float progress = (float)step / kCloseSteps;
        float bw = 210.0f, bh = 3.0f;
        float bx = cx - bw * 0.5f, by = fY + 36.0f;
        fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                          IM_COL32(255, 255, 255, 18), 2.0f);
        fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * progress, by + bh),
                          IM_COL32(110, 155, 255, 200), 2.0f);

        m_ui.endFrame();
        glfwSwapBuffers(m_window);
    }

    if (m_closingThread.joinable()) m_closingThread.join();
}

void Application::shutdown() {
    // GL/GLFW teardown — must run on the main thread after the render loop.
    // All blocking cleanup already completed in the closing animation in run().
    m_prodjlink.stop();
    for (auto& [idx, proj] : m_projectors) proj->destroy();
    m_projectors.clear();
    m_ui.shutdown();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Application::updateSources() {
    // Hot-reload any changed Shader-Claw shaders
    m_shaderClaw.update();

    // Real frame dt for per-binding audio smoothing (frame-rate independent).
    // Local static timer mirrors the audio-analyzer dt block in the run loop;
    // updateSources() is called exactly once per frame.
    static double s_lastBindTime = glfwGetTime();
    double s_nowBindTime = glfwGetTime();
    float audioBindDt = (float)(s_nowBindTime - s_lastBindTime);
    s_lastBindTime = s_nowBindTime;

    // ── "Listening" signals (time-aware musical dynamics) ─────────────────
    // Computed once per frame from the analyzer's structure layer, shared by
    // every layer's audio bindings. These let a binding follow the SONG's arc
    // (highs/lows/builds/drops/pauses) rather than the instantaneous level.
    // Always available — independent of the Audio→Shaders bus toggle.
    float audioSigEnergy   = m_audioAnalyzer.energy();                       // slow altitude
    float audioSigBuild    = m_audioAnalyzer.buildup();                      // riser progress
    float audioSigDrop     = m_audioAnalyzer.drop();                         // impact impulse
    float audioSigMomentum = std::min(std::max(m_audioAnalyzer.energyVel() * 0.5f + 0.5f,
                                                0.0f), 1.0f);                // <0.5 falling, >0.5 rising
    // Silence: rises as the track drops below a soft loudness floor (pauses).
    float audioSigSilence = 0.0f;
    {
        float r = m_audioAnalyzer.smoothedRMS();
        float q = std::min(std::max((r - 0.015f) / (0.070f - 0.015f), 0.0f), 1.0f);
        audioSigSilence = 1.0f - (q * q * (3.0f - 2.0f * q));   // smoothstep, inverted
    }

    // Get mouse state for interactive shaders (normalized 0-1)
    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);
    int winW, winH;
    glfwGetWindowSize(m_window, &winW, &winH);
    float normMX = (winW > 0) ? (float)(mx / winW) : 0.5f;
    float normMY = (winH > 0) ? 1.0f - (float)(my / winH) : 0.5f; // flip Y for GL
    bool mousePressed = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    // Layers bound as an image/texture input by another layer (Fluid3D, Fluid)
    // must keep updating even when hidden — otherwise the texture freezes on
    // one frame. Collect those source ids so the hide-skip below spares them.
    std::set<uint32_t> referencedSources;
    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& l = m_layerStack[i];
        if (!l->source) continue;
        if (l->source->typeName() == "Fluid3D") {
            uint32_t sid = static_cast<FluidSource3D*>(l->source.get())->imageSource().sourceLayerId;
            if (sid) referencedSources.insert(sid);
        } else if (l->source->typeName() == "Fluid") {
            uint32_t sid = static_cast<FluidSource*>(l->source.get())->imageSource().sourceLayerId;
            if (sid) referencedSources.insert(sid);
        }
    }

    // Layers exclusively assigned to a single zone (via that zone's explicit
    // visibleLayerIds — the per-zone bus/managed-layer convention) get that
    // zone's own push-to-talk mic feeding their audio uniforms instead of the
    // shared global bus. A layer visible in more than one zone's explicit list
    // is ambiguous and stays on the global bus untouched.
    std::unordered_map<uint32_t, OutputZone*> zoneAudioOwner;
    {
        std::set<uint32_t> ambiguous;
        for (auto& zonePtr : m_zones) {
            OutputZone& z = *zonePtr;
            for (uint32_t lid : z.visibleLayerIds) {
                auto it = zoneAudioOwner.find(lid);
                if (it == zoneAudioOwner.end()) zoneAudioOwner[lid] = &z;
                else if (it->second != &z) ambiguous.insert(lid);
            }
        }
        for (uint32_t lid : ambiguous) zoneAudioOwner.erase(lid);
        // Only keep owners whose mic is actually live right now.
        for (auto it = zoneAudioOwner.begin(); it != zoneAudioOwner.end(); ) {
            if (!(it->second->micEnabled && it->second->pushToTalkActive)) it = zoneAudioOwner.erase(it);
            else ++it;
        }
    }

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
                // Baked-in audio reactivity removed: shaders no longer receive
                // live audio in their GLSL uniforms (audioBass/Mid/High/Level/
                // FFT). Audio reactivity is now ONLY the explicit, editable
                // parameter-binding system below (per-param sparkle bindings +
                // the "Audio Reactivity On" quick-bind button), which reads the
                // analyzer directly and animates param VALUES. Feeding zeros
                // neutralizes every `audioReact`-style shader globally without
                // editing the shader files; they fall back to base visuals.
                // A layer exclusively owned by one mic-active zone (see
                // zoneAudioOwner above) reacts to that zone's own push-to-talk
                // mic instead of the shared global analyzer — both the Audio
                // Feature Bus below and the per-param bindings after it.
                auto ownerIt = zoneAudioOwner.find(layer->id);
                bool hasZoneMic = ownerIt != zoneAudioOwner.end();
                AudioAnalyzer& a = hasZoneMic ? ownerIt->second->micAnalyzer : m_audioAnalyzer;

                if (m_audioToShaders) {
                    // Assemble the Audio Feature Bus and feed it to the shader.
                    AudioFeatures af;
                    af.level   = a.smoothedRMS(); af.sub = a.sub(); af.bass = a.bass();
                    af.lowMid  = a.lowMid(); af.highMid = a.highMid(); af.treble = a.treble();
                    af.punch   = a.punch();
                    af.beat    = a.beatDecay();
                    af.beatPhase = m_bpmSync.beatPhase(); af.beatPulse = m_bpmSync.beatPulse();
                    af.barPhase  = m_bpmSync.barPhase();   af.bpm = m_bpmSync.bpm();
                    af.tempo01 = std::min(std::max((m_bpmSync.bpm() - 60.0f) / 120.0f, 0.0f), 1.0f);
                    af.brightness = a.brightness(); af.spread = a.spread(); af.rolloff = a.rolloff();
                    af.flatness = a.flatness(); af.flux = a.flux(); af.onset = a.onset();
                    af.onsetRate = a.onsetRate(); af.tilt = a.tilt(); af.zcr = a.zcr();
                    af.texture = a.texture();
                    // Tier 3 — affect
                    af.valence = a.valence(); af.arousal = a.arousal(); af.tension = a.tension();
                    af.warmth = a.warmth(); af.softness = a.softness();
                    af.roughness = a.roughness(); af.charm = a.charm();
                    // Tier 4 — structure
                    af.energy = a.energy(); af.energyVel = a.energyVel(); af.energyAcc = a.energyAcc();
                    af.buildup = a.buildup(); af.buildupRate = a.buildupRate(); af.drop = a.drop();
                    af.novelty = a.novelty(); af.sectionPhase = a.sectionPhase();
                    af.sectionAge = a.sectionAge(); af.layers = a.layers(); af.density = a.density();
                    for (int pi = 0; pi < 4; pi++) af.presence[pi] = a.presence()[pi];
                    af.flow[0] = a.flow()[0]; af.flow[1] = a.flow()[1];
                    // Tier 5 — palette (Oklch ramp) + harmony
                    for (int ci = 0; ci < 3; ci++) {
                        af.palShadow[ci] = a.palShadow()[ci]; af.palMid[ci] = a.palMid()[ci];
                        af.palHigh[ci]   = a.palHigh()[ci];   af.palAccent[ci] = a.palAccent()[ci];
                    }
                    af.palTemp = a.palTemp(); af.palSat = a.palSat();
                    af.dominantPitch = a.dominantPitch(); af.majorMinor = a.majorMinor();
                    for (int ci = 0; ci < 12; ci++) af.chroma[ci] = a.chroma()[ci];
                    af.fftTex = a.fftTexture();
                    // ── EaselAudio v1 — temperament matrix ─────────────
                    af.bassHit = a.bassHit(); af.midHit = a.midHit(); af.highHit = a.highHit();
                    af.bassPresence = a.bassPresence(); af.midPresence = a.midPresence();
                    af.highPresence = a.highPresence(); af.levelPresence = a.levelPresence();
                    af.bassTime = a.bassTime(); af.midTime = a.midTime();
                    af.highTime = a.highTime(); af.levelTime = a.levelTime();
                    // Rhythm bus: confidence is 1 when the user set/tapped a
                    // tempo (manual override), otherwise the detected tempo's
                    // confidence. Below 0.4 confidence the beat one-shot
                    // falls back to the level-driven onset (Synesthesia rule).
                    {
                        bool manual = m_bpmSync.source() == BPMSync::Source::Manual;
                        float conf = manual ? 1.0f : a.detectedBPMConfidence();
                        af.bpmConfidence = conf;
                        af.phase2  = m_bpmSync.phaseN(2);
                        af.phase4  = m_bpmSync.phaseN(4);
                        af.phase8  = m_bpmSync.phaseN(8);
                        af.phase16 = m_bpmSync.phaseN(16);
                        float t = std::min(std::max((conf - 0.25f) / 0.20f, 0.0f), 1.0f);
                        float lock01 = t * t * (3.0f - 2.0f * t);   // smoothstep 0.25..0.45
                        af.onBeat = m_bpmSync.onBeat() * lock01 + a.onset() * (1.0f - lock01);
                        af.toggleOnBeat = m_bpmSync.toggleOnBeat();
                    }
                    // Tier-1 pseudo-stems + temperaments
                    af.stemBass   = a.stem(AudioAnalyzer::StemBass);
                    af.stemDrums  = a.stem(AudioAnalyzer::StemDrums);
                    af.stemMelody = a.stem(AudioAnalyzer::StemMelody);
                    af.stemAir    = a.stem(AudioAnalyzer::StemAir);
                    af.stemVocal  = a.stem(AudioAnalyzer::StemVocal);
                    af.stemBassHit   = a.stemHit(AudioAnalyzer::StemBass);
                    af.stemDrumsHit  = a.stemHit(AudioAnalyzer::StemDrums);
                    af.stemMelodyHit = a.stemHit(AudioAnalyzer::StemMelody);
                    af.stemAirHit    = a.stemHit(AudioAnalyzer::StemAir);
                    af.stemVocalHit  = a.stemHit(AudioAnalyzer::StemVocal);
                    af.stemBassPresence   = a.stemPresence(AudioAnalyzer::StemBass);
                    af.stemDrumsPresence  = a.stemPresence(AudioAnalyzer::StemDrums);
                    af.stemMelodyPresence = a.stemPresence(AudioAnalyzer::StemMelody);
                    af.stemAirPresence    = a.stemPresence(AudioAnalyzer::StemAir);
                    af.stemVocalPresence  = a.stemPresence(AudioAnalyzer::StemVocal);
                    shaderSrc->setAudioFeatures(af);
                } else {
                    shaderSrc->setAudioState(0.0f, 0.0f, 0.0f, 0.0f, 0); // global neutralize
                }
                // Per-param "sparkle" bindings read the analyzer directly and
                // aren't gated by m_audioToShaders. The song-arc signals
                // (energy/build/drop/silence/momentum) default to the shared
                // global analyzer computed once above; a zone-owned layer gets
                // its own zone mic's version of the same signals instead.
                float sigEnergy = audioSigEnergy, sigBuild = audioSigBuild, sigDrop = audioSigDrop;
                float sigSilence = audioSigSilence, sigMomentum = audioSigMomentum;
                if (hasZoneMic) {
                    sigEnergy = a.energy();
                    sigBuild = a.buildup();
                    sigDrop = a.drop();
                    sigMomentum = std::min(std::max(a.energyVel() * 0.5f + 0.5f, 0.0f), 1.0f);
                    float r = a.smoothedRMS();
                    float q = std::min(std::max((r - 0.015f) / (0.070f - 0.015f), 0.0f), 1.0f);
                    sigSilence = 1.0f - (q * q * (3.0f - 2.0f * q));
                }
                shaderSrc->applyAudioBindings(
                    a.smoothedRMS(),
                    a.bass(),
                    (a.lowMid() + a.highMid()) * 0.5f,
                    a.treble(),
                    a.beatDecay(),
                    audioBindDt,
                    &m_midiManager,
                    sigEnergy, sigBuild, sigDrop,
                    sigSilence, sigMomentum
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

            // Audio/MIDI-reactive fluid params — same per-parameter binding
            // model as ShaderSource, applied to FluidSource's public config.
            if (layer->source->typeName() == "Fluid") {
                auto* fsrc = static_cast<FluidSource*>(layer->source.get());
                fsrc->applyAudioBindings(
                    m_audioAnalyzer.smoothedRMS(),
                    m_audioAnalyzer.bass(),
                    (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f,
                    m_audioAnalyzer.treble(),
                    m_audioAnalyzer.beatDecay(),
                    audioBindDt,
                    &m_midiManager,
                    audioSigEnergy, audioSigBuild, audioSigDrop,
                    audioSigSilence, audioSigMomentum
                );
                // Refresh the optional image-inject source from its bound
                // layer (mirrors the ShaderSource imageBindings refresh
                // above — texture IDs can change frame-to-frame for video
                // / NDI / shader sources). Clear if the bound layer
                // vanished or hasn't initialized a texture yet.
                auto& img = fsrc->imageSource();
                if (img.sourceLayerId != 0) {
                    bool found = false;
                    for (int j = 0; j < m_layerStack.count(); j++) {
                        auto& srcLayer = m_layerStack[j];
                        if (srcLayer->id == img.sourceLayerId && srcLayer->source) {
                            GLuint t = srcLayer->source->textureId();
                            if (t != 0) {
                                img.textureId = t;
                                img.width     = srcLayer->source->width();
                                img.height    = srcLayer->source->height();
                                img.flippedV  = srcLayer->source->isFlippedV();
                                found = true;
                            }
                            break;
                        }
                    }
                    if (!found) { img.textureId = 0; img.width = img.height = 0; }
                } else if (img.textureId != 0) {
                    img.textureId = 0; img.width = img.height = 0;
                }
            }

            // 3D SPH fluid — feed audio + refresh the optional image source.
            if (layer->source->typeName() == "Fluid3D") {
                auto* f3 = static_cast<FluidSource3D*>(layer->source.get());
                f3->applyAudioBindings(
                    m_audioAnalyzer.smoothedRMS(),
                    m_audioAnalyzer.bass(),
                    (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f,
                    m_audioAnalyzer.treble(),
                    m_audioAnalyzer.beatDecay(),
                    audioBindDt,
                    &m_midiManager,
                    audioSigEnergy, audioSigBuild, audioSigDrop,
                    audioSigSilence, audioSigMomentum
                );
                // Resolve the bound image layer's live texture each frame
                // (mirrors the 2D Fluid image refresh above).
                auto& img = f3->imageSource();
                if (img.sourceLayerId != 0) {
                    bool found = false;
                    for (int j = 0; j < m_layerStack.count(); j++) {
                        auto& srcLayer = m_layerStack[j];
                        if (srcLayer->id == img.sourceLayerId && srcLayer->source) {
                            GLuint t = srcLayer->source->textureId();
                            if (t != 0) {
                                img.textureId = t;
                                img.width     = srcLayer->source->width();
                                img.height    = srcLayer->source->height();
                                img.flippedV  = srcLayer->source->isFlippedV();
                                found = true;
                            }
                            break;
                        }
                    }
                    if (!found) { img.textureId = 0; img.width = img.height = 0; }
                } else if (img.textureId != 0) {
                    img.textureId = 0; img.width = img.height = 0;
                }
            }

            // Hologram-model layer — feed audio (drives the glitch) and honor
            // a "change model" request from its Properties panel.
            if (layer->source->typeName() == "Hologram Model") {
                auto* hm = static_cast<HologramModelSource*>(layer->source.get());
                float midAvg = (m_audioAnalyzer.lowMid() + m_audioAnalyzer.highMid()) * 0.5f;
                hm->setAudioState(m_audioAnalyzer.bass(), midAvg, m_audioAnalyzer.treble());
                if (hm->m_requestModelDialog) {
                    hm->m_requestModelDialog = false;
                    std::string path = openFileDialog(
                        "3D Models\0*.obj;*.gltf;*.glb\0All Files\0*.*\0");
                    if (!path.empty()) hm->loadModel(path);
                }
            }

            // Skip updating hidden layers — a hidden layer isn't composited, so
            // its (often expensive, e.g. 3D fluid) per-frame work is pure waste.
            // EXCEPT layers used as an image/texture input by another layer:
            // those must keep advancing so the texture stays animated.
            if (layer->visible || referencedSources.count(layer->id)) {
                layer->source->update();
                if (layer->shaderTransitionActive && layer->nextSource) {
                    layer->nextSource->update();
                }
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

#ifdef __APPLE__
    // MediaPipe-style body tracking → DataBus. Apple Vision runs on its
    // own capture session/queue; here we just pull the latest snapshot
    // and publish into the same numeric keys shaders bind to (see
    // DataBus::availableNumericKeys / vision.*).
    if (m_visionTracker.isRunning()) {
        auto vs = m_visionTracker.signals();
        m_dataBus.setNum("vision.hand.count",      vs.handCount);
        m_dataBus.setNum("vision.hand.left.x",     vs.leftHandX);
        m_dataBus.setNum("vision.hand.left.y",     vs.leftHandY);
        m_dataBus.setNum("vision.hand.right.x",    vs.rightHandX);
        m_dataBus.setNum("vision.hand.right.y",    vs.rightHandY);
        m_dataBus.setNum("vision.hand.pinch",      vs.pinch);
        m_dataBus.setNum("vision.pose.confidence", vs.poseConfidence);
        m_dataBus.setNum("vision.pose.head.x",     vs.headX);
        m_dataBus.setNum("vision.pose.head.y",     vs.headY);
        m_dataBus.setNum("vision.face.detected",   vs.faceDetected);
        m_dataBus.setNum("vision.face.smile",      vs.smile);
    }
#endif
}

void Application::compositeAndWarp() {
    m_compositeFrame++;
#ifdef HAS_OPENCV
    // During scanning, display the scan pattern instead of the normal composite
    if (m_scanner.isScanning()) {
        GLuint patternTex = m_scanner.currentPatternTexture();
        if (patternTex && activeZone().ensureGpu()) {
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

    // Composite each zone that actually has a consumer this frame. A zone
    // with no output routing whose texture isn't shown anywhere still burned
    // a full supersampled composite + warp pass per frame (and zones are
    // created on demand by agent OSC zone/ensure, so the per-frame cost
    // ratcheted up across show segments). Consumers:
    //   - any output routing (Fullscreen / NDI / Spout)
    //   - the active zone (viewport preview, present mode, global
    //     NDI/Spout/RTMP/recorder outputs all read activeZone())
    //   - zone 0 while the legacy global NDI sender is enabled (pinned feed)
    //   - all zones while a panel that shows every zone's texture is open
    //     (Properties' Stage Setup section, Stage 3D pre-viz)
    bool uiShowsAllZones = m_ui.isPanelVisible("Properties") ||
        (m_ui.isPanelVisible("Stage") &&
         UIManager::sMode == UIManager::WorkspaceMode::Stage);
    OutputZone* active = m_zones.empty() ? nullptr : &activeZone();
    for (int i = 0; i < (int)m_zones.size(); i++) {
        auto& zone = *m_zones[i];
        bool needed = uiShowsAllZones || &zone == active ||
                      zone.outputDest != OutputDest::None;
#ifdef HAS_NDI
        if (i == 0 && m_ndiOutputEnabled) needed = true;
#endif
        if (needed) compositeZone(zone);
        zone.releaseIdleScratch(m_compositeFrame);
    }
    // In spanned mode, also composite the wide span canvas (its own
    // compositor / visibility / warp at the user-defined resolution).
    if (m_outputMode == OutputMode::Spanned) {
        compositeZone(ensureSpanZone());
    }
    // Age the span zone's scratch UNCONDITIONALLY — m_spanZone isn't in
    // m_zones, so without this a show segment in Spanned mode would strand
    // its supersample buffer (~132 MB at the default 3840x1080 span) for
    // the rest of the show after switching back to Independent.
    if (m_spanZone) {
        m_spanZone->releaseIdleScratch(m_compositeFrame);
    }
}

void Application::compositeZone(OutputZone& zone) {
    if (!zone.ensureGpu()) return;
    // Filter layers by zone visibility.
    // "Show all" covers HUMAN-added layers only: agent-managed feed layers
    // (managedKey set — e.g. the FluxRT layer inside a published bus zone)
    // are plumbing, and join a zone only via explicit visibleLayerIds
    // membership. Without this, every managed FluxRT layer leaked into
    // show-all zones — including the composition that feeds Flux itself
    // (the 2026-06-10 Flux⇄Easel feedback loop).
    std::vector<std::shared_ptr<Layer>> layers;
    if (zone.showAllLayers) {
        for (int i = 0; i < m_layerStack.count(); i++) {
            auto& L = m_layerStack[i];
            if (!L) continue;
            if (L->managedKey.empty() || zone.visibleLayerIds.count(L->id)) {
                layers.push_back(L);
            }
        }
    } else {
        for (int i = 0; i < m_layerStack.count(); i++) {
            if (zone.visibleLayerIds.count(m_layerStack[i]->id)) {
                layers.push_back(m_layerStack[i]);
            }
        }
    }

    {
        // Zones with an active push-to-talk mic drive their own composite
        // blend/audio-reactivity from their own AudioAnalyzer instead of the
        // shared global one — that's the whole point of a per-zone mic.
        bool useZoneMic = zone.micEnabled && zone.pushToTalkActive;
        AudioAnalyzer& src = useZoneMic ? zone.micAnalyzer : m_audioAnalyzer;

        AudioState audio;
        audio.rms = src.smoothedRMS();
        audio.bass = src.bass();
        audio.lowMid = src.lowMid();
        audio.highMid = src.highMid();
        audio.treble = src.treble();
        audio.beatDecay = src.beatDecay();
        audio.beatDetected = src.beatDetected();
        audio.fftTexture = src.fftTexture();
        audio.time = (float)glfwGetTime();
        // BPM sync stays global — it's a shared musical clock, not a per-mic signal.
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
    // While a mask (canvas or layer) is being added/edited, swap the
    // displayed surface for a white alignment grid so the user can line up
    // the mapping. Reverts automatically the moment mask edit mode exits
    // (m_maskEditMode is cleared together with the active mask index).
    // Pure visual aid — masking math/mapping/persistence are untouched.
    if (m_maskEditMode) {
        sourceTex = m_maskGrid.id();
    }
    // MAPPING workspace: always show the selected black/white calibration
    // pattern as the warp source (overrides live content AND the color test
    // pattern) so the user aligns geometry to crisp lines. Takes precedence
    // over the mask-edit grid above since it's the deliberate mapping aid.
    if (UIManager::sMode == UIManager::WorkspaceMode::Mapping) {
        int pi = m_warpEditor.testPatternIndex();
        if (pi < 0) pi = 0;
        if (pi >= kMapPatternCount) pi = kMapPatternCount - 1;
        sourceTex = m_mapPatterns[pi].id();
    }

    // Canvas-level masks (MappingProfile). These now clip the FINAL warped
    // output ("what you see is what gets masked"), so here we only COMBINE the
    // mask shapes; the actual clip is applied after the warp+bloom pass below.
    auto*  mpMask         = mappingForZone(zone);
    int    maskValidCount = 0;
    GLuint maskCombinedTex = 0;
    float  maskFeather    = 0.0f;
    bool   maskInvert     = false;
    if (mpMask && !mpMask->masks.empty()) {
        GLuint singleMaskTex = 0;
        for (auto& mask : mpMask->masks) {
            if (mask.texture && mask.texture->id() && mask.path.count() >= 3) {
                maskValidCount++;
                singleMaskTex = mask.texture->id();
                if (maskValidCount == 1) {
                    maskFeather = mask.feather;
                    maskInvert  = mask.invert;
                }
            }
        }
        if (maskValidCount > 0) {
            maskCombinedTex = singleMaskTex;
            if (maskValidCount > 1) {
                // Union all mask shapes into the zone's union scratch (read later).
                if (zone.maskUnionFBO.width() != zone.width || zone.maskUnionFBO.height() != zone.height)
                    zone.maskUnionFBO.create(zone.width, zone.height);
                zone.scratchUsedFrame[3] = m_compositeFrame;
                zone.maskUnionFBO.bind();
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
                maskCombinedTex = zone.maskUnionFBO.textureId();
            }
        }
    }

    // Store composite texture for canvas preview (pre-warp, pre-mask)
    zone.canvasTexture = sourceTex;

    // ── Warp pass — 4× supersample + 4-tap explicit downsample ─────
    // Render the warp mesh into a 4× FBO (16× the pixel count of
    // warpFBO), then run a custom downsample shader that takes 4
    // bilinear samples at offset positions — each tap reads 4 source
    // texels, so the output pixel averages 16 source texels. That's
    // 16× effective MSAA on every silhouette pixel of the corner-pin
    // diamond, MeshWarp grid, or ObjMesh outline.
    //
    // Cost: ~16× fragment work in the warp shader (still cheap — it's a
    // passthrough sample with a homography on UVs) plus 4 texture reads
    // in the downsample. M-series GPUs are fine.
    // Supersample factor for warp-edge AA, scaled DOWN as the canvas grows so
    // the intermediate buffer never explodes. At 4K, 4× = 15360×8640 = 132 MP
    // cleared+rendered+downsampled EVERY frame (~37ms even with no layers) —
    // and native 4K is already crisp, so it gains nothing. Cap the supersample
    // buffer near ~4K on its long edge: 4K→1×, 1440/1080p→2×, smaller→4×.
    int kWarpSS = 4;
    int longEdge = std::max(zone.width, zone.height);
    // Keep at least 2× even at 4K: a warped/keystoned diagonal still aliases
    // hard at native 4K (the surface edge no longer falls on the pixel grid),
    // and 2× → a 4-tap downsample is the difference between crisp and stair-
    // stepped projector edges. 4K×2 = 33 MP, comfortable on M-series.
    if (longEdge >= 3840)      kWarpSS = 2;
    else if (longEdge >= 1920) kWarpSS = 2;
    // Per-zone supersample target. This was a single function-local static
    // shared by every zone — with two zones of different resolutions the
    // size check flip-flopped and the (huge, 16F) buffer was destroyed and
    // reallocated TWICE PER FRAME, indefinitely.
    const int ssW = zone.width  * kWarpSS;
    const int ssH = zone.height * kWarpSS;
    if (zone.warpSSFBO.width() != ssW || zone.warpSSFBO.height() != ssH) {
        zone.warpSSFBO.createHalfFloat(ssW, ssH);
    }
    zone.scratchUsedFrame[0] = m_compositeFrame;

    zone.warpSSFBO.bind();
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

    // 4-tap explicit-offset downsample: 4 bilinear samples × 4 source
    // texels each = 16 texels averaged per output pixel. Falls back to
    // the linear-copy shader (single bilinear sample) if the dedicated
    // downsample shader failed to compile.
    zone.warpFBO.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_warpDownsampleShader.id() != 0) {
        m_warpDownsampleShader.use();
        m_warpDownsampleShader.setInt ("uTexture",   0);
        m_warpDownsampleShader.setMat3("uTransform", glm::mat3(1.0f));
        m_warpDownsampleShader.setBool("uFlipV",     false);
        m_warpDownsampleShader.setVec2("uTexelSize",
            glm::vec2(1.0f / (float)ssW, 1.0f / (float)ssH));
    } else {
        m_linearCopyShader.use();
        m_linearCopyShader.setInt ("uTexture",   0);
        m_linearCopyShader.setMat3("uTransform", glm::mat3(1.0f));
        m_linearCopyShader.setBool("uFlipV",     false);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, zone.warpSSFBO.textureId());
    m_quad.draw();
    Framebuffer::unbind();

    // ── Phase Q v4 — bloom ─────────────────────────────────────────
    // Bright-pass + separable Gaussian + screen-blend back into warpFBO.
    // Pipeline: warpFBO → brightFBO → ping[0] (H blur) → ping[1] (V blur)
    // → … N passes → composite(warpFBO, ping[N]) → compositeFBO
    // → blit back to warpFBO.
    if (m_bloomEnabled && m_bloomStrength > 0.001f) {
        const int halfW = std::max(1, zone.width  / 2);
        const int halfH = std::max(1, zone.height / 2);
        // (Re)create FBOs lazily so resize is cheap.
        if (zone.bloomBrightFBO.width() != halfW || zone.bloomBrightFBO.height() != halfH) {
            zone.bloomBrightFBO.createHalfFloat(halfW, halfH);
            zone.bloomPingFBO[0].createHalfFloat(halfW, halfH);
            zone.bloomPingFBO[1].createHalfFloat(halfW, halfH);
        }
        if (zone.bloomCompositeFBO.width() != zone.width ||
            zone.bloomCompositeFBO.height() != zone.height) {
            zone.bloomCompositeFBO.createHalfFloat(zone.width, zone.height);
        }
        zone.scratchUsedFrame[1] = m_compositeFrame;

        // ── 1. Bright-pass ───────────────────────────────────────
        zone.bloomBrightFBO.bind();
        glViewport(0, 0, halfW, halfH);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        m_bloomBrightShader.use();
        m_bloomBrightShader.setInt   ("uTexture",  0);
        m_bloomBrightShader.setFloat ("uThreshold", m_bloomThreshold);
        m_bloomBrightShader.setFloat ("uKnee",      m_bloomKnee);
        m_bloomBrightShader.setMat3  ("uTransform", glm::mat3(1.0f));
        m_bloomBrightShader.setBool  ("uFlipV",     false);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
        m_quad.draw();

        // ── 2. Separable Gaussian — N ping-pong passes ───────────
        m_bloomBlurShader.use();
        m_bloomBlurShader.setInt  ("uTexture",  0);
        m_bloomBlurShader.setMat3 ("uTransform", glm::mat3(1.0f));
        m_bloomBlurShader.setBool ("uFlipV",     false);
        GLuint srcTex = zone.bloomBrightFBO.textureId();
        int    pp     = 0;
        const int passes = std::max(1, std::min(6, m_bloomBlurPasses));
        for (int i = 0; i < passes; i++) {
            // Horizontal
            zone.bloomPingFBO[pp].bind();
            glViewport(0, 0, halfW, halfH);
            glClear(GL_COLOR_BUFFER_BIT);
            m_bloomBlurShader.setVec2("uDirection", glm::vec2(1.0f / (float)halfW, 0.0f));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            m_quad.draw();
            srcTex = zone.bloomPingFBO[pp].textureId();
            pp ^= 1;
            // Vertical
            zone.bloomPingFBO[pp].bind();
            glClear(GL_COLOR_BUFFER_BIT);
            m_bloomBlurShader.setVec2("uDirection", glm::vec2(0.0f, 1.0f / (float)halfH));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            m_quad.draw();
            srcTex = zone.bloomPingFBO[pp].textureId();
            pp ^= 1;
        }

        // ── 3. Composite ─────────────────────────────────────────
        zone.bloomCompositeFBO.bind();
        glViewport(0, 0, zone.width, zone.height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        m_bloomCompositeShader.use();
        m_bloomCompositeShader.setInt  ("uBase",     0);
        m_bloomCompositeShader.setInt  ("uBloom",    1);
        m_bloomCompositeShader.setFloat("uStrength", m_bloomStrength);
        m_bloomCompositeShader.setFloat("uTint",     m_bloomTint);
        m_bloomCompositeShader.setFloat("uTime",     (float)glfwGetTime());
        m_bloomCompositeShader.setVec2 ("uResolution", glm::vec2((float)zone.width, (float)zone.height));
        m_bloomCompositeShader.setFloat("uFinish",   m_finishAmount);
        m_bloomCompositeShader.setMat3 ("uTransform", glm::mat3(1.0f));
        m_bloomCompositeShader.setBool ("uFlipV",     false);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        m_quad.draw();
        Framebuffer::unbind();

        // ── 4. Copy result back to warpFBO — linear, no tonemap ─
        // CRITICAL: must use linearCopyShader, not passthroughShader.
        // Passthrough applies ACES + clamps to 1.0 (Phase Q v3 final
        // present step). Using it here would double-tonemap everything
        // and cause a perceptible "opacity fade" across all shaders.
        zone.warpFBO.bind();
        glViewport(0, 0, zone.width, zone.height);
        m_linearCopyShader.use();
        m_linearCopyShader.setInt  ("uTexture",   0);
        m_linearCopyShader.setMat3 ("uTransform", glm::mat3(1.0f));
        m_linearCopyShader.setBool ("uFlipV",     false);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, zone.bloomCompositeFBO.textureId());
        m_quad.draw();
        Framebuffer::unbind();
        glActiveTexture(GL_TEXTURE0);
    }

    // Edge blend post-process (if any edge has blend width > 0)
    bool hasEdgeBlend = mp && (mp->edgeBlendLeft > 0 || mp->edgeBlendRight > 0 ||
                               mp->edgeBlendTop > 0 || mp->edgeBlendBottom > 0);
    if (hasEdgeBlend) {
        // Ensure temp FBO matches zone size
        if (zone.postFBO.width() != zone.width || zone.postFBO.height() != zone.height) {
            zone.postFBO.create(zone.width, zone.height);
        }
        zone.scratchUsedFrame[2] = m_compositeFrame;
        // Render warpFBO through edge blend shader into temp FBO
        zone.postFBO.bind();
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
        glBindTexture(GL_TEXTURE_2D, zone.postFBO.textureId());
        m_quad.draw();
        Framebuffer::unbind();
    }

    // ── Canvas mask — applied to the FINAL warped/bloomed/edge-blended output.
    // The mask clips in the SAME space the user drew it (over the projected
    // image): what you see is what gets masked. warpFBO -> tmp -> warpFBO.
    if (maskValidCount > 0 && maskCombinedTex) {
        Framebuffer& tmp = zone.postFBO; // reuse scratch (edge blend already done)
        if (tmp.width() != zone.width || tmp.height() != zone.height)
            tmp.create(zone.width, zone.height);
        zone.scratchUsedFrame[2] = m_compositeFrame;
        tmp.bind();
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
        m_passthroughShader.setFloat("uMaskFeather", maskFeather);
        m_passthroughShader.setBool("uMaskInvert", maskInvert);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, maskCombinedTex);
        m_quad.draw();
        glActiveTexture(GL_TEXTURE0);
        Framebuffer::unbind();

        // Copy the masked result back into warpFBO.
        zone.warpFBO.bind();
        glViewport(0, 0, zone.width, zone.height);
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
        glBindTexture(GL_TEXTURE_2D, tmp.textureId());
        m_quad.draw();
        Framebuffer::unbind();
    }
}

void Application::renderReadbackFBO(OutputZone& zone) {
    renderReadbackFBO(zone, zone.readbackFBO, zone.width, zone.height);
}

void Application::renderReadbackFBO(OutputZone& zone, Framebuffer& target, int width, int height) {
    // Created on demand: only NDI/RTMP/recorder readback needs this copy,
    // so zones never read back don't pay for it.
    if (target.width() != width || target.height() != height) {
        target.create(width, height, false);
    }
    target.bind();
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

ProjectorOutput* Application::ensureProjector(int monitorIndex) {
    if (monitorIndex < 0) return nullptr;
    auto it = m_projectors.find(monitorIndex);
    if (it != m_projectors.end() && it->second->isActive())
        return it->second.get();

    // Rate-limit creation retries so a persistently-failing monitor (e.g. the
    // editor's own) doesn't hammer glfwCreateWindow every frame. Retry no more
    // often than once per ~60 frames.
    static std::unordered_map<int, int> s_retryCountdown;
    int& countdown = s_retryCountdown[monitorIndex];
    if (countdown > 0) { countdown--; return nullptr; }

    auto proj = std::make_unique<ProjectorOutput>();
    if (proj->create(m_window, monitorIndex)) {
        ProjectorOutput* raw = proj.get();
        m_projectors[monitorIndex] = std::move(proj);
        return raw;
    }
    countdown = 60; // back off ~1s before next attempt
    return nullptr;
}

OutputZone& Application::ensureSpanZone() {
    if (!m_spanZone) {
        m_spanZone = std::make_unique<OutputZone>();
        m_spanZone->name = "Span";
        m_spanZone->init();
        // -1 = NO whole-canvas warp. The span canvas stays flat; each projector
        // slice applies its OWN corner-pin below. This also guarantees the span
        // canvas never touches the default zone's mapping profile.
        m_spanZone->mappingIndex = -1;
    }
    if (m_spanWidth < 16)  m_spanWidth = 16;
    if (m_spanHeight < 16) m_spanHeight = 16;
    if (m_spanZone->width != m_spanWidth || m_spanZone->height != m_spanHeight) {
        m_spanZone->resize(m_spanWidth, m_spanHeight);
    }
    if (m_spanSlices.empty()) {
        // Default: two equal halves (left / right) for a 2-screen wall.
        m_spanSlices = { SpanSlice{-1, 0.0f, 0.5f, -1}, SpanSlice{-1, 0.5f, 1.0f, -1} };
    }
    return *m_spanZone;
}

void Application::ensureSpanSliceResources() {
    int n = (int)m_spanSlices.size();
    if ((int)m_spanCropFBO.size() != n) m_spanCropFBO.resize(n);
    if ((int)m_spanWarpFBO.size() != n) m_spanWarpFBO.resize(n);
    // Each slice gets its own corner-pin profile in the SEPARATE m_spanMappings
    // store (never m_mappings — the normal mapping UI must not see these).
    // A slice with no valid profile REUSES slot i — slice reconfiguration
    // (Slices slider, /easel/span/slices|setup) resets every mappingIndex
    // to -1, and appending fresh profiles each time orphaned N profiles
    // (3 shader programs + warp meshes each) per reconfiguration forever.
    // Reuse also preserves each projector slot's alignment across reconfigs.
    for (int i = 0; i < n; i++) {
        if (m_spanSlices[i].mappingIndex < 0 ||
            m_spanSlices[i].mappingIndex >= (int)m_spanMappings.size()) {
            if (i < (int)m_spanMappings.size()) {
                m_spanSlices[i].mappingIndex = i;
            } else {
                auto mp = std::make_unique<MappingProfile>();
                mp->init();
                mp->name = "Projector " + std::to_string(i + 1);
                m_spanSlices[i].mappingIndex = (int)m_spanMappings.size();
                m_spanMappings.push_back(std::move(mp));
            }
        }
    }
    // Trim profiles no slice references anymore (only m_spanSlices ever
    // indexes m_spanMappings).
    int maxRef = -1;
    for (const auto& s : m_spanSlices) maxRef = std::max(maxRef, s.mappingIndex);
    if ((int)m_spanMappings.size() > maxRef + 1) {
        m_spanMappings.resize(maxRef + 1);
    }
}

void Application::layoutSpanSlices() {
    int n = (int)m_spanSlices.size();
    if (n <= 0) return;
    // Even left-to-right split across the canvas width.
    for (int i = 0; i < n; i++) {
        m_spanSlices[i].u0 = (float)i / (float)n;
        m_spanSlices[i].u1 = (float)(i + 1) / (float)n;
    }
}

void Application::presentOutputs() {
    // Cross-context sync with projector windows is handled inside
    // ProjectorOutput::presentCrop via glFenceSync + server-side glWaitSync.
    // The old approach — glFinish() here whenever any projector was attached
    // — drained the entire pipeline on the CPU every frame, serializing CPU
    // and GPU so frame time was their SUM for the whole show.

#ifdef HAS_NDI
    // Apply the shared NDI wire settings to every sender each frame (cheap)
    // so /easel/ndi/fps takes effect live on global + per-zone senders.
    {
        NDIOutputSettings ndiSettings;
        ndiSettings.targetFps = m_ndiTargetFps;
        m_ndiOutput.setSettings(ndiSettings);
        for (auto& zp : m_zones) {
            if (zp) zp->ndiOutput.setSettings(ndiSettings);
        }
    }
#endif

    // Track which monitor indices are still needed
    std::set<int> neededMonitors;

    // Per-zone output routing
    for (int i = 0; i < (int)m_zones.size(); i++) {
        auto& zone = *m_zones[i];

        // Independent mode only: each zone drives its own monitor. In Spanned
        // mode the projectors are driven by the span-slice pass below instead,
        // so we skip per-zone fullscreen routing here (NDI/Spout still run).
        if (m_outputMode == OutputMode::Independent &&
            zone.outputDest == OutputDest::Fullscreen && zone.outputMonitor >= 0) {
            // Verify monitor still exists before using it.
            auto monitors = ProjectorOutput::enumerateMonitors();
            // Latched per zone: the skip below is per-frame and must stay
            // quiet, but a PERSISTENT out-of-range route is the desktop-on-
            // the-projector failure (2026-07-15, again 2026-07-16/17) and
            // deserves exactly one line per episode.
            static std::set<std::string> s_presentSkipLogged;
            if (zone.outputMonitor >= (int)monitors.size()) {
                // Monitor index out of range this frame. GLFW can report a
                // transiently shrunk monitor list while new windows are being
                // created — do NOT wipe the zone's saved destination. Just
                // skip rendering this frame; it will recover once the monitor
                // list stabilises. Keep the projector in neededMonitors so the
                // cleanup pass below doesn't destroy it either.
                if (s_presentSkipLogged.insert(zone.name).second) {
                    ProjectorOutput::logEvent(
                        "present SKIPPED: zone '" + zone.name + "' routes fullscreen monitor " +
                        std::to_string(zone.outputMonitor) + " but only " +
                        std::to_string(monitors.size()) + " monitor(s) exist — retrying per "
                        "frame silently until the list recovers");
                }
                neededMonitors.insert(zone.outputMonitor);
            } else {
                if (s_presentSkipLogged.erase(zone.name)) {
                    ProjectorOutput::logEvent(
                        "present RECOVERED: zone '" + zone.name + "' monitor " +
                        std::to_string(zone.outputMonitor) + " is back in range");
                }
                neededMonitors.insert(zone.outputMonitor);
                ProjectorOutput* proj = ensureProjector(zone.outputMonitor);
                if (proj) {
                    // Size the zone canvas to the physical projector.
                    if (zone.width != proj->projectorWidth() ||
                        zone.height != proj->projectorHeight()) {
                        zone.resize(proj->projectorWidth(), proj->projectorHeight());
                    }
                    proj->present(zone.warpFBO.textureId());
                }
            }
        }

#ifdef HAS_NDI
        if (zone.outputDest == OutputDest::NDI && !getenv("EASEL_NO_NDI_OUTPUT")) {
            std::string wantName = (zone.rawNdiName && !zone.ndiStreamName.empty())
                ? zone.ndiStreamName
                : ("Easel - " + (zone.ndiStreamName.empty() ? zone.name : zone.ndiStreamName));
            // Recreate on rename — ensureZoneNdi (agent OSC) can flip
            // rawNdiName/ndiStreamName on a zone whose sender is already
            // live; without this the new name never reaches the wire.
            if (zone.ndiOutput.isActive() && zone.ndiActiveName != wantName) {
                zone.ndiOutput.destroy();
            }
            if (!zone.ndiOutput.isActive()) {
                zone.ndiOutput.create(wantName);
                zone.ndiActiveName = wantName;
            }
            // Only render the readback copy when someone is listening —
            // send() drops the frame anyway when there are no receivers.
            if (zone.ndiOutput.isActive() && zone.ndiOutput.hasReceivers()) {
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

    // Spanned mode: for each monitor slice, crop its [u0,u1] half out of the
    // FLAT span composite, warp it through THAT projector's own corner-pin,
    // then present it fullscreen. Per-projector warp = independent alignment.
    if (m_outputMode == OutputMode::Spanned && m_spanZone) {
        ensureSpanSliceResources();
        GLuint flat = m_spanZone->canvasTexture; // unwarped wide composite
        auto monitors = ProjectorOutput::enumerateMonitors();
        bool didCrop = false;
        for (int i = 0; i < (int)m_spanSlices.size(); i++) {
            auto& slice = m_spanSlices[i];
            if (slice.monitor < 0) continue;
            neededMonitors.insert(slice.monitor); // keep alive across transient list changes
            if (slice.monitor >= (int)monitors.size()) continue;
            ProjectorOutput* proj = ensureProjector(slice.monitor);
            if (!proj || !flat) continue;

            int pw = proj->projectorWidth();
            int ph = proj->projectorHeight();
            if (pw <= 0 || ph <= 0) continue;

            Framebuffer& cropFBO = m_spanCropFBO[i];
            Framebuffer& warpFBO = m_spanWarpFBO[i];
            if (cropFBO.width() != pw || cropFBO.height() != ph) cropFBO.create(pw, ph);
            if (warpFBO.width() != pw || warpFBO.height() != ph) warpFBO.create(pw, ph);

            // 1) Crop the projector's flat half into cropFBO at native res.
            float u0 = slice.u0;
            float w  = slice.u1 - slice.u0;
            if (w < 0.0f) w = 0.0f;
            cropFBO.bind();
            glViewport(0, 0, pw, ph);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            m_passthroughShader.use();
            m_passthroughShader.setInt("uTexture", 0);
            m_passthroughShader.setFloat("uOpacity", 1.0f);
            m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
            m_passthroughShader.setBool("uHasMask", false);
            m_passthroughShader.setBool("uFlipV", false);
            m_passthroughShader.setFloat("uTileX", 1.0f);
            m_passthroughShader.setFloat("uTileY", 1.0f);
            m_passthroughShader.setInt("uMosaicMode", 0);
            m_passthroughShader.setFloat("uMosaicTransition", 1.0f);
            m_passthroughShader.setFloat("uFeather", 0.0f);
            m_passthroughShader.setVec4("uCrop", glm::vec4(0.0f));
            m_passthroughShader.setVec2("uUVOffset", glm::vec2(u0, 0.0f));
            m_passthroughShader.setVec2("uUVScale",  glm::vec2(w, 1.0f));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, flat);
            m_quad.draw();
            Framebuffer::unbind();
            didCrop = true;

            // 2) Warp the cropped half through this slice's own mapping profile.
            MappingProfile* mp = (slice.mappingIndex >= 0 &&
                                  slice.mappingIndex < (int)m_spanMappings.size())
                                 ? m_spanMappings[slice.mappingIndex].get() : nullptr;
            warpFBO.bind();
            glViewport(0, 0, pw, ph);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            GLuint croppedTex = cropFBO.textureId();
            if (mp && mp->warpMode == ViewportPanel::WarpMode::CornerPin) {
                mp->cornerPin.render(croppedTex);
            } else if (mp && mp->warpMode == ViewportPanel::WarpMode::MeshWarp) {
                mp->meshWarp.render(croppedTex);
            } else if (mp && mp->warpMode == ViewportPanel::WarpMode::ObjMesh) {
                mp->objMeshWarp.render(croppedTex, (float)pw / (float)ph);
            } else {
                // No/unknown mapping — straight copy.
                m_passthroughShader.use();
                m_passthroughShader.setInt("uTexture", 0);
                m_passthroughShader.setFloat("uOpacity", 1.0f);
                m_passthroughShader.setMat3("uTransform", glm::mat3(1.0f));
                m_passthroughShader.setBool("uHasMask", false);
                m_passthroughShader.setBool("uFlipV", false);
                m_passthroughShader.setFloat("uTileX", 1.0f);
                m_passthroughShader.setFloat("uTileY", 1.0f);
                m_passthroughShader.setInt("uMosaicMode", 0);
                m_passthroughShader.setFloat("uMosaicTransition", 1.0f);
                m_passthroughShader.setFloat("uFeather", 0.0f);
                m_passthroughShader.setVec4("uCrop", glm::vec4(0.0f));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, croppedTex);
                m_quad.draw();
            }
            Framebuffer::unbind();

            // 3) Present the warped, aligned half to the projector.
            proj->present(warpFBO.textureId());
        }
        // The shared passthrough shader's UV-remap was set above; restore it to
        // identity so every OTHER user of m_passthroughShader is unaffected.
        if (didCrop) {
            m_passthroughShader.use();
            m_passthroughShader.setVec2("uUVOffset", glm::vec2(0.0f, 0.0f));
            m_passthroughShader.setVec2("uUVScale",  glm::vec2(1.0f, 1.0f));
        }
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
#ifdef HAS_FFMPEG
    if (m_rtmpOutput.isActive() || m_recorder.isActive()) needsReadback = true;
#endif
    if (needsReadback) {
        renderReadbackFBO(active);
    }

#ifdef HAS_NDI
    // Legacy global NDI output (composition toggle in NDI panel).
    // PINNED to zone 0 ("Main"), never the UI-selected zone: this sender is
    // the Flux input feed ("Lu"), and its content must not follow editor
    // tab clicks — selecting a zone that contains a FluxRT layer used to
    // feed Flux its own output (2026-06-10 feedback loop). Main is the
    // launch-sequence zone: default shader content only.
    if (m_ndiOutputEnabled && m_ndiOutput.isActive() && !m_zones.empty() &&
        m_ndiOutput.hasReceivers()) {
        OutputZone& luZone = *m_zones[0];
        constexpr int kFluxInputW = 768;
        constexpr int kFluxInputH = 432;
        renderReadbackFBO(luZone, m_ndiFluxInputFBO, kFluxInputW, kFluxInputH);
        m_ndiOutput.send(m_ndiFluxInputFBO.textureId(), kFluxInputW, kFluxInputH);
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

    // Free the zone's mapping profile (3 shader programs + warp meshes +
    // mask textures) unless another zone shares it. Erasing shifts later
    // indices, so re-point every zone past it — zone add/remove cycles used
    // to strand one profile each.
    int mi = zone.mappingIndex;
    if (mi >= 0 && mi < (int)m_mappings.size()) {
        bool shared = false;
        for (int i = 0; i < (int)m_zones.size(); i++) {
            if (i != index && m_zones[i] && m_zones[i]->mappingIndex == mi) {
                shared = true;
                break;
            }
        }
        if (!shared) {
            m_mappings.erase(m_mappings.begin() + mi);
            for (auto& zp : m_zones) {
                if (zp && zp->mappingIndex > mi) zp->mappingIndex--;
            }
        }
    }

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

// V1 voice-driven timeline — dispatch a parsed intent to the existing
// timeline / layer / shader APIs. Soft matching on layer name (case-insensitive
// substring) so "the magritte layer" finds "surrealism_magritte". Adds two
// keyframes for fade-in/out using the Phase C lane runtime; one for SetOpacity.
void Application::handleVoiceIntent(const easel::voice::VoiceIntent& intent) {
    using easel::voice::IntentKind;

    auto findLayerByName = [&](const std::string& name) -> std::shared_ptr<Layer> {
        if (name.empty()) return nullptr;
        std::string needle = name;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        for (int i = 0; i < m_layerStack.count(); i++) {
            const auto& lp = m_layerStack[i];
            if (!lp) continue;
            std::string hay = lp->name;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            if (hay.find(needle) != std::string::npos) return lp;
        }
        return nullptr;
    };

    auto echo = [&](const std::string& msg) {
        m_voiceLastEcho = msg;
        m_voiceLastEchoTime = glfwGetTime();
        m_voiceLog.push_front(msg);
        while (m_voiceLog.size() > 8) m_voiceLog.pop_back();
    };

    switch (intent.kind) {
        case IntentKind::Play:        m_timeline.play();   echo("▶ Play"); break;
        case IntentKind::Pause:       m_timeline.pause();  echo("⏸ Pause"); break;
        case IntentKind::Stop:        m_timeline.stop();   echo("⏹ Stop"); break;
        case IntentKind::ToggleLoop:  /* loop flag is internal — toggle via stop+play+m_loop later */
                                      echo("Loop · (wire toggle next)"); break;
        case IntentKind::Seek:
            m_timeline.seek(intent.value);
            echo("Seek → " + std::to_string((int)intent.value) + "s");
            break;
        case IntentKind::Skip:
            m_timeline.seek(m_timeline.playhead() + intent.value);
            echo(std::string(intent.value >= 0 ? "Skip +" : "Skip ") +
                 std::to_string((int)intent.value) + "s");
            break;
        case IntentKind::FadeIn:
        case IntentKind::FadeOut: {
            auto lp = findLayerByName(intent.target);
            if (!lp) { echo("? layer '" + intent.target + "' not found"); break; }
            // Two keyframes: now (start) and now+duration (end). Fade in goes
            // 0→1; fade out goes 1→0. Captures the current opacity as a baseline
            // so successive fades chain naturally (a fade-out from 0.7 ends at 0).
            double t0 = m_timeline.playhead();
            double t1 = t0 + (double)intent.value;
            float startV = (intent.kind == IntentKind::FadeIn) ? 0.0f : lp->opacity;
            float endV   = (intent.kind == IntentKind::FadeIn) ? 1.0f : 0.0f;
            // Toggle creates lane + first key from `currentValue`. Force start.
            m_timeline.toggleKeyframeAt(lp->id, "opacity", t0, startV);
            // Now place the end key — toggleKeyframeAt adds a new key with the
            // value we pass when no key exists at that time.
            m_timeline.toggleKeyframeAt(lp->id, "opacity", t1, endV);
            // Auto-play so the fade is immediately visible — match user intent.
            m_timeline.play();
            echo((intent.kind == IntentKind::FadeIn ? "Fade in " : "Fade out ") +
                 lp->name + " · " + std::to_string((int)intent.value) + "s");
            break;
        }
        case IntentKind::SetOpacity: {
            auto lp = findLayerByName(intent.target);
            if (!lp) { echo("? layer '" + intent.target + "' not found"); break; }
            float v = std::max(0.0f, std::min(1.0f, intent.value));
            m_timeline.toggleKeyframeAt(lp->id, "opacity", m_timeline.playhead(), v);
            // Also snap the live value so the change is visible even when the
            // playhead doesn't move (sampleAnimatedParams writes opacity on the
            // next tick anyway, but this keeps the slider in sync immediately).
            lp->opacity = v;
            echo("Set " + lp->name + " opacity → " +
                 std::to_string((int)(v * 100.0f)) + "%");
            break;
        }
        case IntentKind::Show:
        case IntentKind::Hide: {
            auto lp = findLayerByName(intent.target);
            if (!lp) { echo("? layer '" + intent.target + "' not found"); break; }
            lp->visible = (intent.kind == IntentKind::Show);
            echo((intent.kind == IntentKind::Show ? "Show " : "Hide ") + lp->name);
            break;
        }
        case IntentKind::AddShader:
        case IntentKind::PickTransition:
            echo("? '" + intent.raw + "' (V1.1 — not wired yet)");
            break;
        case IntentKind::Unknown:
        default:
            echo("? not understood: \"" + intent.raw + "\"");
            break;
    }
}

void Application::pushTranscript(const std::string& prefix, TranscriptFeed& feed,
                                 const std::string& text, bool isFinal) {
    // <prefix>.latest — full current segment. Unchanged contract: typewriter/
    // reveal text shaders bind `msg` to this and depend on the whole utterance.
    m_dataBus.set(prefix + ".latest", text);

    auto splitWords = [](const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0, n = s.size();
        while (i < n) {
            while (i < n && std::isspace((unsigned char)s[i])) ++i;
            size_t start = i;
            while (i < n && !std::isspace((unsigned char)s[i])) ++i;
            if (i > start) out.push_back(s.substr(start, i - start));
        }
        return out;
    };
    auto join = [](const std::vector<std::string>& w, size_t from) {
        std::string s;
        for (size_t i = from; i < w.size(); ++i) { if (!s.empty()) s += ' '; s += w[i]; }
        return s;
    };

    // Empty interim (recognizer silence) — leave the .words/.recent feeds
    // showing the last words rather than blanking. (Blanking would set
    // msgAge<0 and hide the text shader between utterances.)
    if (text.empty()) return;

    std::vector<std::string> cur = splitWords(text);

    // <prefix>.words — only the words newly appended since the previous segment.
    // WORD-boundary diff (not char): Deepgram interim results grow ("the quick"
    // -> "the quick brown") AND revise the tail ("I think" -> "I thought");
    // word diffing emits "thought" not a mid-word "ought". A brand-new utterance
    // diverges at the first word, so it's (correctly) all-new. Only publish a
    // non-empty delta so repeats/finals don't blank the shader (msgAge<0).
    std::vector<std::string> prev = splitWords(feed.prevSegment);
    size_t common = 0;
    while (common < prev.size() && common < cur.size() && prev[common] == cur[common])
        ++common;
    std::string delta = join(cur, common);
    if (!delta.empty()) m_dataBus.set(prefix + ".words", delta);

    // <prefix>.recent — a running FIFO of the last m_recentWordCap words that
    // slides as you speak. Window = committed words (past utterances) + the
    // current segment's words, trimmed to the last N. Because the current
    // segment replaces itself each interim, tail revisions never duplicate, and
    // the feed "keeps up" word-by-word during speech instead of waiting for the
    // sentence to finalize.
    int cap = m_recentWordCap > 0 ? m_recentWordCap : (int)cur.size();
    std::vector<std::string> window = feed.finalWords;
    window.insert(window.end(), cur.begin(), cur.end());
    if ((int)window.size() > cap)
        window.erase(window.begin(), window.end() - cap);
    m_dataBus.set(prefix + ".recent", join(window, 0));

    if (isFinal) {
        // Commit the finished utterance into the FIFO history. Bound the
        // history to a small buffer (>= any sane cap) so memory stays flat over
        // a long show while still surviving a runtime cap increase.
        feed.finalWords.insert(feed.finalWords.end(), cur.begin(), cur.end());
        constexpr size_t kFinalWordsMax = 128;
        if (feed.finalWords.size() > kFinalWordsMax)
            feed.finalWords.erase(feed.finalWords.begin(),
                                  feed.finalWords.end() - kFinalWordsMax);
        feed.prevSegment.clear();   // next utterance counts as all-new
    } else {
        feed.prevSegment = text;    // keep growing this utterance
    }
}

void Application::startVoiceRecording() {
#ifdef __APPLE__
    if (m_voiceListening) return;
    if (!m_voiceRecognizer.available()) {
        std::cerr << "[Voice] start: not available (auth pending or denied) — "
                     "open System Settings > Privacy > Speech Recognition / Microphone\n";
        m_voiceLastEcho = "Mic / speech permission needed (System Settings > Privacy)";
        m_voiceLastEchoTime = glfwGetTime();
        return;
    }
    m_voicePartial.clear();
    m_voiceRecognizer.onPartial = [this](const std::string& s) {
        m_voicePartial = s;
        // Stream partials into the same DataBus key the Cue transcript
        // callback uses so text_clusters.fs (and any other shader bound
        // to "cue.latest") sees live speech as the user is speaking.
        // Mirrors CueClient::setTranscriptCallback's path so the bubbles
        // light up whether the words come from local speech or a remote
        // Cue session.
        //
        // Empty partials (recognizer producing nothing during silence)
        // are ignored — same guard as the /cue/latest OSC handler. This
        // stops the mic from blanking a bridge-driven cue mid-show, and
        // stops it from racing test writes during the M7 unicode case.
        if (s.empty()) return;
        if (glfwGetTime() >= m_cueLatestSuppressUntil) {
            pushCueWords(s, /*isFinal=*/false);  // interim — emit only new words
        }
    };
    m_voiceRecognizer.onFinal = [this](const std::string& s) {
        m_voicePartial.clear();
        std::cerr << "[Voice] final: \"" << s << "\"\n";
        if (s.empty()) return;
        m_voiceLastEcho = std::string("\"") + s + "\"";
        m_voiceLastEchoTime = glfwGetTime();
        // Push the final to the DataBus too so it persists in the bubbles
        // after speech stops, and append to the running transcript.
        pushCueWords(s, /*isFinal=*/true);   // closes the utterance for cue.words
        m_dataBus.appendCapped("cue.transcript", s);
        auto intent = easel::voice::parse(s);
        std::cerr << "[Voice] parsed: " << easel::voice::intentName(intent.kind);
        if (!intent.target.empty()) std::cerr << " target=" << intent.target;
        std::cerr << "\n";
        handleVoiceIntent(intent);
        // Apple's SFSpeechRecognitionTask is single-shot — once isFinal fires
        // the task is done. To keep the mic continuously transcribing while
        // m_voiceContinuous is true, ask the main loop to tear down and
        // restart the session on its next tick (don't restart from inside
        // this callback; it runs on the Speech framework's queue).
        if (m_voiceContinuous) m_voiceRestartPending = true;
    };
    m_voiceRecognizer.start();
    m_voiceListening = m_voiceRecognizer.isRecording();
    std::cerr << "[Voice] start: listening=" << (m_voiceListening ? "yes" : "no") << "\n";
#endif
}

void Application::stopVoiceRecording() {
#ifdef __APPLE__
    if (!m_voiceListening) return;
    m_voiceRecognizer.stop();
    m_voiceListening = false;
    std::cerr << "[Voice] stop\n";
#endif
}

void Application::renderVoiceCommandBar() {
    if (!m_voiceBarOpen) return;
    // Auto-tuck: voice bar only appears when actively listening or when
    // there's recent transcript activity (last echo within 6s). The mic
    // button on the floating transport pill is the primary entry point;
    // this command bar is for typed fallback + log review and only needs
    // to surface when the user is in-flow.
    bool recentActivity = (m_voiceLastEchoTime > 0.0 &&
                           (glfwGetTime() - m_voiceLastEchoTime) < 6.0);
    if (!m_voiceListening && !recentActivity && m_voicePartial.empty())
        return;
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Voice##VoiceBar", &m_voiceBarOpen,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Type or speak — V1 dispatch (Etherea / SFSpeech feed in next).");

    ImGui::SetNextItemWidth(-90);
    bool submit = ImGui::InputText("##VoiceCmd", m_voiceTextInput,
                                   sizeof(m_voiceTextInput),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Run") || submit) {
        if (m_voiceTextInput[0] != '\0') {
            auto intent = easel::voice::parse(m_voiceTextInput);
            handleVoiceIntent(intent);
            m_voiceTextInput[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    if (!m_voiceLastEcho.empty()) {
        double age = glfwGetTime() - m_voiceLastEchoTime;
        float alpha = (float)std::max(0.0, 1.0 - (age - 4.0) / 2.0);
        if (alpha > 0.05f) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.84f, 0.88f, 0.94f, alpha));
            ImGui::TextWrapped("%s", m_voiceLastEcho.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (!m_voiceLog.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Recent");
        for (size_t i = 0; i < std::min((size_t)6, m_voiceLog.size()); i++) {
            ImGui::TextDisabled("· %s", m_voiceLog[i].c_str());
        }
    }

    if (ImGui::SmallButton("Clear log")) m_voiceLog.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("Try: \"play\", \"fade in fauvism over 3 seconds\", \"set magritte opacity to 50 percent\"");

    ImGui::End();
}

// Single source of truth for the timeline ⇄ bottom-nav slide animation.
// Called once per frame at the very top of renderUI() — BEFORE the dockspace
// and any panel renders — so the params panel (Fix 1), the slide of the
// timeline + bottom nav (Fix 2) and the left-rail thumbnails (Fix 3) all read
// the exact same geometry within a single frame.
void Application::updateTimelineAnim() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float kPillH = 56.0f;   // must match renderFloatingTransportPill pillH

    // Eased open factor. m_timelineMinimized==true → collapse toward 0,
    // else expand toward 1. ~200ms ease driven by frame DeltaTime.
    float dt    = ImGui::GetIO().DeltaTime;
    float speed = dt / 0.20f;                       // full travel in ~200ms
    float target = m_timelineMinimized ? 0.0f : 1.0f;
    if (m_timelineAnimT < target)
        m_timelineAnimT = std::min(target, m_timelineAnimT + speed);
    else if (m_timelineAnimT > target)
        m_timelineAnimT = std::max(target, m_timelineAnimT - speed);

    // smoothstep so the slide eases in and out instead of moving linearly.
    float e = m_timelineAnimT;
    float eased = e * e * (3.0f - 2.0f * e);

    // Resolve the fully-open height. m_timelineTargetH is settled from the
    // real measured content height once open (renderTimelinePanel), but on
    // the very first open frame — before any content has been measured — it
    // may still be the 220 default or a stale tiny value. Floor it at a sane
    // 240px so the timeline always animates to a clearly VISIBLE height, then
    // ceiling it so it can never exceed the viewport (leaving room for pill).
    float fullH = m_timelineTargetH;
    if (fullH < 240.0f)  fullH = 240.0f;                       // sane default
    float maxH = vp->Size.y - kPillH - 80.0f;
    if (maxH < 0.0f) maxH = 0.0f;       // degenerate viewport on the 1st frame
    if (fullH > maxH)    fullH = maxH;

    m_timelineCurH = eased * fullH;
    // m_timelineTopY = the bottom-nav PILL's top edge (== timeline TOP edge),
    // kept correct for the params panel / left-rail thumbnails that clamp to
    // it. NOTE: renderTimelinePanel() does NOT use this for the timeline
    // window's own position (that is derived directly from m_timelineCurH and
    // pinned to the viewport bottom) — using m_timelineTopY there placed the
    // timeline 56px too high, behind the pill, making it invisible.
    m_timelineTopY = vp->Pos.y + vp->Size.y - kPillH - m_timelineCurH;

    // Safety clamp — m_timelineTopY MUST always be an on-screen value so the
    // bottom-nav transport pill (renderFloatingTransportPill, which reads this
    // verbatim) is never placed at y≈0 / off-screen / behind the viewport.
    // Fall back to the original "flush at the bottom" position whenever the
    // computed value is non-positive or above the bottom-nav's valid band
    // (e.g. stale 0 init, a frame that skipped updateTimelineAnim, or a
    // degenerate viewport size before GLFW reports the framebuffer).
    float bottomNavTopMin = vp->Pos.y;                       // never above viewport top
    float bottomNavTopMax = vp->Pos.y + vp->Size.y - kPillH; // flush-at-bottom (timeline closed)
    if (!(m_timelineTopY > bottomNavTopMin) || m_timelineTopY > bottomNavTopMax) {
        m_timelineTopY = bottomNavTopMax;
    }
}

void Application::renderUI() {
    // Workspace-mode entry hooks. Fires once per transition (not every frame).
    // Entering PLAY pops the timeline open so the artist sees the whole show
    // the instant the workspace lights up — no second click required.
    if (UIManager::sMode != m_prevWorkspaceMode) {
        if (UIManager::sMode == UIManager::WorkspaceMode::Show) {
            m_timelineMinimized = false;
        }
        m_prevWorkspaceMode = UIManager::sMode;
    }

    // Recompute the shared timeline/bottom-nav slide geometry first so every
    // panel below reads consistent numbers (Fixes 1–3).
    updateTimelineAnim();
    // Hand the live timeline top edge to UIManager so the right params float
    // (Fix 1) and the left-rail thumbnail column (Fix 3) clamp to it.
    m_ui.setTimelineTopY(m_timelineTopY);

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
        // Auto-connect the first available controller (throttled ~1s) so MIDI
        // "just works" — no need to open it in the MIDI panel first. Skipped if
        // the user explicitly chose "None".
        static double s_midiScan = 0.0;
        double nowT = glfwGetTime();
        if (!m_midiManager.isOpen() && !m_midiUserDisconnected &&
            nowT - s_midiScan > 1.0) {
            s_midiScan = nowT;
            auto devs = m_midiManager.listDevices();
            if (!devs.empty()) m_midiManager.openDevice(0);
        }
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

    // M3 — Live re-publish tick. Every ~300ms, build the Play wire
    // payload and ship it ONLY if its content changed. Catches every UI
    // mutation (layer add via menu/drag, clip edits in the timeline panel,
    // BPM tap, marker add) without needing to instrument each mutation
    // site individually. Manual PUBLISH button + OSC trigger keep their
    // unconditional-send semantics.
    {
        double now = glfwGetTime();
        if (now >= m_nextPublishCheckAt) {
            m_nextPublishCheckAt = now + 0.30;
            publishPlayIfChanged();
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
            } else if (msg.address == "/easel/play/publish") {
                // Headless trigger for "publish Play to mobile" — same action
                // as the File > Publish to Mobile menu item.
                publishPlayToAgent();
            } else if (msg.address == "/easel/ndi/refresh") {
                // M5 — Re-poll the NDI finder for active senders. The
                // finder accumulates passively in the background; explicit
                // refresh ensures the next publish carries fresh state.
#ifdef HAS_NDI
                if (NDIRuntime::instance().isAvailable()) {
                    m_ndiSources = m_ndiFinder.sources();
                }
#endif
            } else if (msg.address == "/easel/workspace" && !msg.strings.empty()) {
                // Programmatic workspace switch — used by the bridge test
                // harness AND by the mobile control surface (M2). Accepts
                // case-insensitive "canvas" / "mapping" / "stage" / "play".
                std::string w = msg.strings[0];
                for (auto& c : w) c = (char)tolower((unsigned char)c);
                if (w == "canvas") UIManager::setMode(UIManager::WorkspaceMode::Canvas);
                else if (w == "mapping") UIManager::setMode(UIManager::WorkspaceMode::Mapping);
                else if (w == "stage") UIManager::setMode(UIManager::WorkspaceMode::Stage);
                else if (w == "zones") UIManager::setMode(UIManager::WorkspaceMode::Zones);
                else if (w == "play" || w == "show")
                    UIManager::setMode(UIManager::WorkspaceMode::Show);
            } else if (msg.address == "/easel/layer/add" && !msg.strings.empty()) {
                // Programmatic layer add — used by the bridge test harness
                // and by the mobile control surface (M2). Path resolution
                // follows the same dispatch loadShader/loadImage/loadVideo
                // use: extension picks the loader.
                const std::string& path = msg.strings[0];
                if (path == "__fluid__") {
                    addFluid();
                    // (early-out — special generator token, not a file)
                }
                if (path == "__fluid3d__") {
                    addFluid3D();
                    // (early-out — special generator token, not a file)
                }
                // Hologram model token: "__hologram__" opens the picker;
                // "__hologram__:/abs/model.glb" loads that model directly.
                bool isHolo = (path.rfind("__hologram__", 0) == 0);
                if (isHolo) {
                    std::string mp;
                    size_t c = path.find(':');
                    if (c != std::string::npos) mp = path.substr(c + 1);
                    addHologramModel(mp);
                }
                size_t dot = path.rfind('.');
                std::string ext = (dot == std::string::npos)
                    ? std::string{} : path.substr(dot);
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (path == "__fluid__" || path == "__fluid3d__" || isHolo) {
                    // handled above
                } else if (ext == ".fs" || ext == ".frag" || ext == ".glsl") {
                    loadShader(path);
                } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
                           || ext == ".bmp" || ext == ".tga") {
                    loadImage(path);
                } else if (ext == ".mp4" || ext == ".avi" || ext == ".mkv"
                           || ext == ".mov" || ext == ".webm") {
                    loadVideo(path);
                } else {
                    std::cerr << "[OSC] /easel/layer/add: unknown ext "
                              << ext << " for path " << path << std::endl;
                }
            } else if (msg.address == "/easel/layer/remove" && !msg.ints.empty()) {
                int idx = msg.ints[0];
                if (idx >= 0 && idx < m_layerStack.count()) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer);
                    uint32_t rid = m_layerStack[idx] ? m_layerStack[idx]->id : 0;
                    m_layerStack.removeLayer(idx);
                    if (rid) m_timeline.removeTrackForLayer(rid);
                    if (m_selectedLayer >= m_layerStack.count())
                        m_selectedLayer = m_layerStack.count() - 1;
                }
            } else if (msg.address == "/easel/layer/ensure/ndi"
                       && msg.strings.size() >= 2) {
                // Agent SDK managed-layer contract: idempotently ensure exactly
                // one NDI layer for the stable slot key. strings = [slot, source].
#ifdef HAS_NDI
                ensureManagedNDILayer(msg.strings[0], msg.strings[1]);
#else
                std::cerr << "[OSC] /easel/layer/ensure/ndi ignored: "
                             "built without HAS_NDI\n";
#endif
            } else if (msg.address == "/easel/layer/add/ndi"
                       && !msg.strings.empty()) {
                // strings = [source] (plain add) or [source, slot] (managed add,
                // idempotent by slot — matches the SDK CLI `layer add-ndi`).
#ifdef HAS_NDI
                if (msg.strings.size() >= 2 && !msg.strings[1].empty())
                    ensureManagedNDILayer(msg.strings[1], msg.strings[0]);
                else
                    addNDISource(msg.strings[0]);
#else
                std::cerr << "[OSC] /easel/layer/add/ndi ignored: "
                             "built without HAS_NDI\n";
#endif
            } else if (msg.address == "/easel/layer/clear-managed") {
                // Remove every agent-managed layer (non-empty managedKey).
                clearManagedLayers();
            } else if (msg.address == "/easel/project/save") {
                // Persist to the default project path so the agent SDK can read
                // the file back and confirm an applied route.
                saveProject(defaultProjectPath());
            } else if (msg.address == "/easel/layer/ensure/shader"
                       && msg.strings.size() >= 2) {
                // Managed shader overlay layer, keyed by slot. strings = [slot, path].
                ensureManagedShaderLayer(msg.strings[0], msg.strings[1]);
            } else if (msg.address == "/easel/layer/ensure/fluid"
                       && !msg.strings.empty()) {
                // Managed fluid layer, keyed by slot. strings = [slot, kind?];
                // kind "3d" = the volumetric FluidSource3D, absent/anything
                // else = the classic 2D sim. The built-in generators as
                // agent-managed, zone-assignable layers (so the SDK can stand
                // up a flux-input zone — and the phone can pick 3D Fluid —
                // end to end).
                bool threeD = msg.strings.size() >= 2 && msg.strings[1] == "3d";
                ensureManagedFluidLayer(msg.strings[0], threeD);
            } else if (msg.address == "/easel/layer/remove-managed"
                       && !msg.strings.empty()) {
                // Remove one managed layer by its key (drops an overlay/base).
                removeManagedLayer(msg.strings[0]);
            } else if (msg.address == "/easel/layer/param"
                       && msg.strings.size() >= 2) {
                // Set an ISF param on a managed shader layer by key.
                // strings = [managedKey, paramName]; value = float/int arg, or
                // strings[2] for text.
                setManagedLayerParam(msg.strings[0], msg.strings[1], msg);
            } else if (msg.address == "/easel/layer/audiopreset"
                       && msg.strings.size() >= 2) {
                // Master audio-reactivity recipe on a managed layer — the
                // remote face of the PropertyPanel's Reactivity/Character/
                // Shuffle/Off row. strings = [managedKey, command]; command ∈
                // intensity|character (floats[0] = value), shuffle, off.
                setManagedLayerAudioPreset(msg.strings[0], msg.strings[1], msg);
            } else if (msg.address == "/easel/layer/audiobind"
                       && msg.strings.size() >= 3) {
                // Bind ONE param of a managed shader layer to an audio signal
                // — the remote face of the PropertyPanel's per-slider bolt
                // popover. strings = [managedKey, paramName, signal]; signal ∈
                // off|level|bass|mid|high|beat|energy|build|drop|silence|
                // momentum. floats[0] = amount 0..1 (how far above the
                // param's current value audio may push it), floats[1] =
                // optional smoothing 0..1 (defaults to the house 0.85).
                setManagedLayerAudioBind(msg.strings[0], msg.strings[1],
                                         msg.strings[2], msg);
            } else if (msg.address == "/easel/layer/bind-image"
                       && msg.strings.size() >= 3) {
                // Point a managed shader layer's image INPUT at another layer's
                // texture — the remote twin of the desktop TEXTURE dropdown.
                // strings = [managedKey, inputName, sourceRef]; sourceRef is a
                // layer id, a managedKey, or a layer name. The per-frame
                // bindings refresh keeps the texture id current afterwards.
                // (Upstream's verb; the SDK's shader.texture action sends this.
                // The short-lived local /easel/layer/bindImage twin was dropped
                // in the merge — nothing ships against it.)
                bindManagedLayerImage(msg.strings[0], msg.strings[1], msg.strings[2]);
            } else if (msg.address == "/easel/zone/ensure"
                       && msg.strings.size() >= 2) {
                // Composite/bus: ensure an output zone that publishes a named NDI
                // feed. strings = [zoneName, feedName].
                ensureZoneNdi(msg.strings[0], msg.strings[1]);
            } else if (msg.address == "/easel/zone/layer"
                       && msg.strings.size() >= 2) {
                // Add a managed layer (by key) to a zone's composite.
                // strings = [zoneName, managedKey].
                addZoneLayerByKey(msg.strings[0], msg.strings[1]);
            } else if (msg.address == "/easel/zone/remove"
                       && !msg.strings.empty()) {
                // Tear down a composite zone (stops its NDI feed).
                removeZoneByName(msg.strings[0]);
            } else if (msg.address == "/easel/zone/output"
                       && msg.strings.size() >= 2) {
                // Set a zone's physical output at runtime (the render loop applies
                // it next frame, exactly like the GUI output combo — no display/NDI
                // setup needed here). strings = [zoneName, dest, arg3];
                // dest = none|fullscreen|ndi. For fullscreen the monitor index is
                // ints[0] if present else atoi(strings[2]); for ndi, strings[2] (if
                // any) is the feed name. This is the missing OSC verb that lets the
                // agent route a zone to a projector (Fullscreen+monitor), not just NDI.
                int mon = !msg.ints.empty() ? msg.ints[0]
                          : (msg.strings.size() >= 3 ? atoi(msg.strings[2].c_str()) : -1);
                std::string arg3 = msg.strings.size() >= 3 ? msg.strings[2] : std::string();
                setZoneOutput(msg.strings[0], msg.strings[1], mon, arg3);
            } else if (msg.address == "/easel/zone/activate" && !msg.ints.empty()) {
                int zi = msg.ints[0];
                if (zi >= 0 && zi < (int)m_zones.size()) {
                    m_activeZone = zi;
                }
            } else if (msg.address == "/easel/zone/showAll"
                       && msg.ints.size() >= 2) {
                int zi = msg.ints[0];
                bool flag = msg.ints[1] != 0;
                if (zi >= 0 && zi < (int)m_zones.size() && m_zones[zi]) {
                    m_zones[zi]->showAllLayers = flag;
                }
            } else if (msg.address == "/easel/zone/layerVisibility"
                       && msg.ints.size() >= 3) {
                // /easel/zone/layerVisibility <zoneIndex> <layerIndex> <0|1>
                int zi  = msg.ints[0];
                int li  = msg.ints[1];
                bool on = msg.ints[2] != 0;
                if (zi >= 0 && zi < (int)m_zones.size() && m_zones[zi]
                    && li >= 0 && li < m_layerStack.count()
                    && m_layerStack[li]) {
                    uint32_t lid = m_layerStack[li]->id;
                    if (on) m_zones[zi]->visibleLayerIds.insert(lid);
                    else    m_zones[zi]->visibleLayerIds.erase(lid);
                }
            } else if (msg.address == "/easel/layer/allzones"
                       && !msg.ints.empty()) {
                // /easel/layer/allzones <layerIndex> [solo 0|1]
                // Push one layer to every zone. solo=1 → each zone renders
                // ONLY this layer (the whole house matches); solo=0/absent →
                // the layer simply joins every zone's composite.
                int li = msg.ints[0];
                bool solo = msg.ints.size() >= 2 && msg.ints[1] != 0;
                if (li >= 0 && li < m_layerStack.count() && m_layerStack[li]) {
                    uint32_t lid = m_layerStack[li]->id;
                    if (solo) soloLayerAcrossZones(m_zones, lid);
                    else      showLayerInAllZones(m_zones, lid);
                }
            } else if (msg.address == "/easel/zone/mic/enable"
                       && msg.ints.size() >= 2) {
                // /easel/zone/mic/enable <zoneIndex> <enabled 0|1> [deviceId string]
                // Configures whether a zone uses its own independent mic at all;
                // capture itself only opens while push-to-talk is also active
                // (see /easel/zone/mic/ptt). Persisted with the project.
                int zi = msg.ints[0];
                bool on = msg.ints[1] != 0;
                if (zi >= 0 && zi < (int)m_zones.size() && m_zones[zi]) {
                    m_zones[zi]->micEnabled = on;
                    if (!msg.strings.empty()) m_zones[zi]->micDeviceId = msg.strings[0];
                    if (!on) {
                        m_zones[zi]->pushToTalkActive = false;
                        m_zones[zi]->micAnalyzer.stopCapture();
                    }
                }
            } else if (msg.address == "/easel/zone/mic/ptt"
                       && msg.ints.size() >= 2) {
                // /easel/zone/mic/ptt <zoneIndex> <down 0|1> — momentary; not
                // persisted. Sent by the mobile app / SDK on button press/release.
                int zi = msg.ints[0];
                bool down = msg.ints[1] != 0;
                if (zi >= 0 && zi < (int)m_zones.size() && m_zones[zi] && m_zones[zi]->micEnabled) {
                    m_zones[zi]->pushToTalkActive = down;
                }
            } else if (msg.address == "/easel/audio/toshaders" && !msg.ints.empty()) {
                // Global Audio -> Shaders switch (the panic off-switch).
                m_audioToShaders = (msg.ints[0] != 0);
#ifdef HAS_FFMPEG
            } else if (msg.address == "/easel/record/start") {
                // /easel/record/start [<path>] — headless recording, same code
                // path as the transport REC button. Optional string overrides
                // the timestamped recordings/ filename. Idempotent while active.
                if (!m_recorder.isActive()) {
                    startRecording(msg.strings.empty() ? std::string{} : msg.strings[0]);
                }
            } else if (msg.address == "/easel/record/stop") {
                if (m_recorder.isActive()) {
                    m_recorder.stop();
                    m_timelineExporting = false;
                }
            } else if (msg.address == "/easel/rtmp/start" && !msg.strings.empty()) {
                // /easel/rtmp/start <streamKeyOrUrl> — a bare YouTube stream key
                // uses the default ingest (start); a full rtmp(s):// URL streams
                // to a custom ingest (startCustom). Idempotent while active.
                if (!m_rtmpOutput.isActive()) {
                    auto& z = activeZone();
                    const std::string& dest = msg.strings[0];
                    bool isUrl = dest.rfind("rtmp://", 0) == 0 || dest.rfind("rtmps://", 0) == 0;
                    bool ok = isUrl
                        ? m_rtmpOutput.startCustom(dest, z.warpFBO.width(), z.warpFBO.height(), 16, 9, 30)
                        : m_rtmpOutput.start(dest, z.warpFBO.width(), z.warpFBO.height(), 16, 9, 30);
                    if (!ok) std::cerr << "[RTMP] OSC stream start failed\n";
                }
            } else if (msg.address == "/easel/rtmp/stop") {
                if (m_rtmpOutput.isActive()) m_rtmpOutput.stop();
#endif
            } else if (msg.address == "/easel/output/mode" && !msg.ints.empty()) {
                // /easel/output/mode <0=Independent | 1=Spanned>
                m_outputMode = (msg.ints[0] != 0) ? OutputMode::Spanned
                                                  : OutputMode::Independent;
                if (m_outputMode == OutputMode::Spanned) ensureSpanZone();
            } else if (msg.address == "/easel/span/res" && msg.ints.size() >= 2) {
                // /easel/span/res <width> <height>  (custom span-canvas resolution)
                m_spanWidth  = msg.ints[0];
                m_spanHeight = msg.ints[1];
                ensureSpanZone(); // applies the resize
            } else if (msg.address == "/easel/span/slices" && !msg.ints.empty()) {
                // /easel/span/slices <count>  — set slice count, auto split L->R
                int n = msg.ints[0];
                if (n < 1) n = 1;
                if (n > 16) n = 16;
                m_spanSlices.assign(n, SpanSlice{});
                layoutSpanSlices();
            } else if (msg.address == "/easel/span/slice" && msg.ints.size() >= 2) {
                // /easel/span/slice <sliceIndex> <monitor> [<u0> <u1>]
                ensureSpanZone();
                int si  = msg.ints[0];
                int mon = msg.ints[1];
                if (si >= 0 && si < (int)m_spanSlices.size()) {
                    m_spanSlices[si].monitor = mon;
                    if (msg.floats.size() >= 2) {
                        m_spanSlices[si].u0 = msg.floats[0];
                        m_spanSlices[si].u1 = msg.floats[1];
                    }
                }
            } else if (msg.address == "/easel/span/setup" && msg.ints.size() >= 4) {
                // /easel/span/setup <w> <h> <monLeft> <monRight>
                // One-shot: switch to spanned, set res, and assign two halves.
                m_spanWidth  = msg.ints[0];
                m_spanHeight = msg.ints[1];
                ensureSpanZone();
                m_spanSlices = { SpanSlice{ msg.ints[2], 0.0f, 0.5f },
                                 SpanSlice{ msg.ints[3], 0.5f, 1.0f } };
                m_outputMode = OutputMode::Spanned;
            } else if (msg.address == "/easel/ndi/fps"
                       && (!msg.floats.empty() || !msg.ints.empty())) {
                // /easel/ndi/fps <fps>  — wire-rate cap for ALL NDI senders
                // (global + per-zone); <= 0 = uncapped (send at render rate).
                m_ndiTargetFps = !msg.floats.empty() ? msg.floats[0]
                                                     : (float)msg.ints[0];
            } else if (msg.address == "/easel/clip/remove"
                       && msg.ints.size() >= 2) {
                // /easel/clip/remove <layerIndex> <clipId>
                int layerIdx = msg.ints[0];
                uint32_t clipId = (uint32_t)msg.ints[1];
                if (layerIdx >= 0 && layerIdx < m_layerStack.count()
                    && m_layerStack[layerIdx]) {
                    uint32_t lid = m_layerStack[layerIdx]->id;
                    m_timeline.removeClip(lid, clipId);
                }
            } else if (msg.address == "/easel/layer/move" && msg.ints.size() >= 2) {
                int from = msg.ints[0];
                int to   = msg.ints[1];
                if (from >= 0 && from < m_layerStack.count()
                    && to   >= 0 && to   < m_layerStack.count()
                    && from != to) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer);
                    m_layerStack.moveLayer(from, to);
                    if (m_selectedLayer == from) m_selectedLayer = to;
                }
            } else if (msg.address == "/easel/transport/play") {
                m_timeline.play();
            } else if (msg.address == "/easel/transport/pause") {
                m_timeline.pause();
            } else if (msg.address == "/easel/transport/stop") {
                m_timeline.stop();
            } else if (msg.address == "/easel/transport/toggle") {
                m_timeline.togglePlay();
            } else if (msg.address == "/easel/transport/seek" && !msg.floats.empty()) {
                m_timeline.seek((double)msg.floats[0]);
            } else if (msg.address.rfind("/easel/marker/", 0) == 0) {
                // /easel/marker/<id> — jump playhead to marker time and
                // recall its bound scene (if any). Mobile fires this when
                // the operator taps a marker dot on the timeline strip.
                uint32_t mid = (uint32_t)atoi(msg.address.c_str() + 14);
                if (auto* mk = m_timeline.findMarker(mid)) {
                    m_timeline.seek(mk->time);
                    if (!mk->sceneName.empty()) {
                        // SceneManager addresses by index — look up by name
                        // inline. Markers store the user-facing label of
                        // the scene to recall.
                        const auto& scenes = m_sceneManager.scenes();
                        for (int si = 0; si < (int)scenes.size(); si++) {
                            if (scenes[si].name == mk->sceneName) {
                                m_sceneManager.recallScene(si, m_layerStack);
                                break;
                            }
                        }
                    }
                }
            } else if (msg.address == "/cue/latest" && !msg.strings.empty()) {
                // Mobile -> agent (cue.observe) -> Easel via /cue/latest <s>.
                // Updates the cue.latest DataBus slot; the per-frame binding
                // loop (around line 990) restarts msgAge automatically when
                // the value diverges from the last utterance, so voice-native
                // shaders (text_clusters.fs and friends bound on `msg`)
                // typewriter-reveal the new cue on the desktop projection.
                //
                // M4 edge-case guards:
                //  - Empty payloads are ignored so an end-of-utterance event
                //    can't blank a non-empty shader mid-show. To explicitly
                //    clear, use the dedicated /cue/clear OSC below.
                //  - Cap the payload at 4 KB so a stuck recognizer or pasted
                //    novel doesn't bloat the per-frame binding loop or thrash
                //    the typewriter timer cache.
                //  - Extend the mic-suppress window so a hot room mic can't
                //    instantly clobber a bridge-driven cue (M7 unicode/control
                //    char cases). 1.5s window — long enough for the slow
                //    desktop SFSpeechRecognizer cadence, short enough that
                //    "talk to control" still feels live after a manual cue.
                std::string s = msg.strings[0];
                if (!s.empty()) {
                    if (s.size() > 4096) s.resize(4096);
                    // Each agent-pushed cue is a complete utterance, so treat
                    // it as final: cue.words carries the whole new cue (diffed
                    // against the prior one, then reset).
                    pushCueWords(s, /*isFinal=*/true);
                    m_cueLatestSuppressUntil = glfwGetTime() + 1.5;
                }
            } else if (msg.address == "/cue/clear") {
                // Explicit reset — the only path that blanks cue.latest from
                // the bridge. Use this when the show explicitly wants to
                // hide voice-driven text (intermission, scene change).
                // Suppress background writers (mic recognizer, cue WS) for
                // ~500ms so an intentional blank doesn't get instantly
                // overwritten by stray room audio.
                m_dataBus.set("cue.latest", "");
                m_dataBus.set("cue.words", "");
                m_dataBus.set("cue.recent", "");
                m_cueFeed = TranscriptFeed{};
                m_cueLatestSuppressUntil = glfwGetTime() + 0.5;
            } else if (msg.address.rfind("/easel/scene/", 0) == 0) {
                int idx = atoi(msg.address.c_str() + 13);
                m_sceneManager.recallScene(idx, m_layerStack);
            } else if (msg.address.rfind("/easel/layer/", 0) == 0) {
                // Parse /easel/layer/N/property OR /easel/layer/N/param/<name>
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
                    else if (prop.rfind("param/", 0) == 0) {
                        // M2 — live shader parameter writes from mobile.
                        // /easel/layer/N/param/<name> <float|int|string>
                        // Dispatches by ISFInput type so floats land in
                        // setFloat, strings in setText, ints (0/1) in setBool.
                        std::string paramName = prop.substr(6);
                        if (layer->source && layer->source->isShader()
                            && !paramName.empty()) {
                            auto* shader =
                                static_cast<ShaderSource*>(layer->source.get());
                            if (!msg.floats.empty()) {
                                shader->setFloat(paramName, msg.floats[0]);
                            } else if (!msg.strings.empty()) {
                                shader->setText(paramName, msg.strings[0]);
                            } else if (!msg.ints.empty()) {
                                shader->setBool(paramName, msg.ints[0] != 0);
                            }
                        }
                    }
                    else if (prop == "effect/add" && !msg.strings.empty()) {
                        // /easel/layer/N/effect/add <typeName>  (e.g. "Sharpen")
                        // Appends a default-configured effect to the chain.
                        const std::string& want = msg.strings[0];
                        for (int t = 0; t < (int)EffectType::COUNT; t++) {
                            if (want == effectTypeName((EffectType)t)) {
                                LayerEffect fx;
                                fx.type = (EffectType)t;
                                layer->effects.push_back(fx);
                                break;
                            }
                        }
                    }
                    else if (prop == "effect/clear") {
                        layer->effects.clear();
                    }
                    else if (prop.rfind("effect/", 0) == 0) {
                        // /easel/layer/N/effect/<idx>/<field> <float|int> —
                        // live-tune a param on an existing effect in the chain.
                        std::string r = prop.substr(7);
                        size_t sp = r.find('/');
                        if (sp != std::string::npos) {
                            int eIdx = atoi(r.substr(0, sp).c_str());
                            std::string field = r.substr(sp + 1);
                            if (eIdx >= 0 && eIdx < (int)layer->effects.size()) {
                                auto& fx = layer->effects[eIdx];
                                float fv = !msg.floats.empty() ? msg.floats[0]
                                         : (!msg.ints.empty() ? (float)msg.ints[0] : 0.0f);
                                int   iv = !msg.ints.empty() ? msg.ints[0] : (int)fv;
                                if      (field == "enabled")       fx.enabled = iv != 0;
                                else if (field == "blurRadius")    fx.blurRadius = fv;
                                else if (field == "pixelSize")     fx.pixelSize = fv;
                                else if (field == "glowIntensity") fx.glowIntensity = fv;
                                else if (field == "sharpenAmount") fx.sharpenAmount = fv;
                                else if (field == "sharpenRadius") fx.sharpenRadius = fv;
                                else if (field == "audioSignal")   fx.audioSignal = iv;
                                else if (field == "audioAmount")   fx.audioAmount = fv;
                            }
                        }
                    }
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
    // Floating transport pill is now a docked full-width bottom bar — must
    // match renderFloatingTransportPill's pillH so the dockspace shrinks and
    // its content (timeline, panels) doesn't render under the bar.
    // Mapping mode has no timeline or bottom transport nav, so don't reserve
    // the 56px bottom strip — let the output viewport fill all the way down.
    float transportBarH = (UIManager::sMode == UIManager::WorkspaceMode::Mapping)
                        ? 0.0f : 56.0f;
    // Main menu bar removed — the brand glyph + overflow menu now live in
    // the workspace nav row inside the viewport, eliminating the previous
    // 3-row chrome stack (macOS title bar + ImGui menu bar + workspace nav).
    // renderMenuBar() is kept as a no-op so any future caller doesn't break.
    m_ui.setupDockspace(transportBarH);
    // (renderVoiceCommandBar() removed — voice UI now lives in a popup
    // anchored to the mic icon next to System Audio in the transport bar.)

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
        // Viewport preview: always show the warped output so users on a single
        // display (common on Mac) can see corner-pin/mesh-warp changes live.
        // In mask edit mode the warp source is the white alignment grid
        // (sourceTex = m_maskGrid above), so warpFBO already holds the grid
        // deformed by the SAME corner-pin/mesh-warp transform — showing it
        // lets users align grid lines to physical geometry while dragging
        // the mapping handles. Mask points/mapping handles are screen-space
        // ImGui overlays drawn on top regardless of the preview texture.
        GLuint previewTex = z.warpFBO.textureId();
        if (!previewTex) previewTex = z.canvasTexture ? z.canvasTexture : z.compositor.resultTexture();
        if (!previewTex) previewTex = m_testPattern.id();
        m_viewportPanel.setLayerSelected(m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count());
        m_viewportPanel.setEditorFullscreen(m_editorFullscreen);
        // Tell the viewport the actually-visible region so it fits AND centers
        // the canvas within the area not hidden by floating chrome.
        {
            auto* vp2 = ImGui::GetMainViewport();
            float rpLeft   = m_ui.getRightPanelLeft();
            float visLeft  = UIManager::kLeftRailW;
            float visRight = rpLeft - vp2->WorkPos.x;
            float visW     = visRight - visLeft;
            if (visW < 100.0f) { visLeft = 0.0f; visW = 0.0f; }
            // Visible height: from panel top down to the timeline top edge.
            // m_timelineTopY is in screen coords; subtract WorkPos.y to get
            // panel-relative, then subtract workspaceBarHeight for window padding.
            float visH = 0.0f;
            if (m_timelineTopY > vp2->WorkPos.y + 40.0f) {
                visH = m_timelineTopY - vp2->WorkPos.y;
                if (visH < 100.0f) visH = 0.0f;
            }
            m_viewportPanel.setVisibleRegion(visLeft, visW, visH);
        }
        m_viewportPanel.render(previewTex, mappingForZone(z), projAspect,
                               &m_zones, &m_activeZone, &monitors, ndiAvail, editorMon, &m_mappings,
                               nullptr,  // inlineSetupSection (Canvas mode doesn't need it)
                               [this]() { renderNavBarPrefix(); });
        // ── Drag the window by its custom title-bar (the top nav row) ──────
        // The native macOS title bar is transparent + merged into the content
        // view (EaselMac_UnifyTitleBar), so the OS no longer gives us a
        // draggable strip. Re-implement it: pressing an EMPTY part of the nav
        // row and dragging moves the window, exactly like dragging any other
        // app's title bar. Interactive nav items (tabs, gear, right cluster)
        // are skipped via the hover/active checks, and the left inset where
        // the AppKit traffic-lights live is excluded so their clicks pass
        // through. Disabled in fullscreen (no title bar to grab, and moving a
        // borderless-fullscreen window makes no sense).
        if (!m_editorFullscreen) {
            ImGuiViewport* mvp = ImGui::GetMainViewport();
            const float kNavRowH = 28.0f;
            const float kLeftInset = 78.0f;   // clear the traffic-light cluster
            ImVec2 navMin(mvp->Pos.x + kLeftInset, mvp->Pos.y);
            ImVec2 navMax(mvp->Pos.x + mvp->Size.x, mvp->Pos.y + kNavRowH);

            static bool   s_titleDragging = false;
            static double s_grabOffX = 0.0, s_grabOffY = 0.0;  // cursor pos within window at grab

            bool overEmptyNav =
                ImGui::IsMouseHoveringRect(navMin, navMax, false) &&
                !ImGui::IsAnyItemHovered() &&
                !ImGui::IsAnyItemActive() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

            if (!s_titleDragging && overEmptyNav &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                glfwGetCursorPos(m_window, &s_grabOffX, &s_grabOffY);
                s_titleDragging = true;
            }
            if (s_titleDragging) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // Keep the grabbed point pinned under the cursor: new
                    // window origin = current screen-cursor − grab offset.
                    int wx, wy; glfwGetWindowPos(m_window, &wx, &wy);
                    double cx, cy; glfwGetCursorPos(m_window, &cx, &cy);
                    glfwSetWindowPos(m_window,
                                     wx + (int)(cx - s_grabOffX),
                                     wy + (int)(cy - s_grabOffY));
                } else {
                    s_titleDragging = false;
                }
            }
        }
        // Tell UIManager where the canvas image lives at zoom=1 — the
        // BASE bounds, not the zoomed ones, so panning/zooming the canvas
        // doesn't drag the right Control Panel float along with it.
        {
            glm::vec2 io = m_viewportPanel.baseImageOrigin();
            glm::vec2 isz = m_viewportPanel.baseImageSize();
            if (isz.y > 0) {
                m_ui.setCanvasBoundsY(io.y, io.y + isz.y);
            }
        }

        // ── Interactive fluid splatting (mouse drag + vision hand) ────────
        // Paint directly into Fluid layers. Left-drag over the canvas in
        // Normal edit mode (Pavel's pointer-driven splat), and/or a pinch
        // from hand tracking. Splats are QUEUED on the source and applied
        // inside FluidSource::update() so this never touches live GL state.
        {
            static glm::vec2 s_prevMouseUV(0.5f, 0.5f); static bool s_mouseSplat = false;
            static glm::vec2 s_prevHandUV(0.5f, 0.5f);  static bool s_handSplat = false;
            const float kSplatForce = 6000.0f;

            std::vector<FluidSource*> fluids;
            for (int i = 0; i < m_layerStack.count(); i++) {
                auto& L = m_layerStack[i];
                if (L->source && L->source->typeName() == "Fluid")
                    fluids.push_back(static_cast<FluidSource*>(L->source.get()));
            }

            glm::vec2 vio = m_viewportPanel.imageOrigin();
            glm::vec2 vsz = m_viewportPanel.imageSize();
            if (!fluids.empty() && vsz.x > 0.0f && vsz.y > 0.0f) {
                // 1) Mouse pointer — left-drag over the canvas in Normal mode.
                bool normalMode = (m_viewportPanel.editMode() == ViewportPanel::EditMode::Normal);
                bool spaceHeld  = ImGui::IsKeyDown(ImGuiKey_Space);
                ImVec2 mp = ImGui::GetMousePos();
                float u = (mp.x - vio.x) / vsz.x;
                float v = (mp.y - vio.y) / vsz.y;
                bool over = (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
                glm::vec2 uv(u, 1.0f - v);   // GL uv: origin bottom-left
                bool painting = normalMode && !spaceHeld && over &&
                                m_viewportPanel.isHovered() &&
                                ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                                !ImGui::GetIO().WantTextInput;
                if (painting) {
                    if (!s_mouseSplat) { s_prevMouseUV = uv; s_mouseSplat = true; }
                    glm::vec2 d = uv - s_prevMouseUV;
                    for (auto* f : fluids)
                        f->queuePointerSplat(uv.x, uv.y, d.x * kSplatForce, d.y * kSplatForce);
                    s_prevMouseUV = uv;
                } else {
                    s_mouseSplat = false;
                }

                // 2) Vision hand — a pinch drives a splat at the hand position.
                float pinch = m_dataBus.getNum("vision.hand.pinch", 0.0f);
                if (pinch > 0.5f) {
                    float hx = m_dataBus.getNum("vision.hand.left.x", 0.5f);
                    float hy = m_dataBus.getNum("vision.hand.left.y", 0.5f);
                    glm::vec2 huv(hx, 1.0f - hy);  // flip to GL origin
                    if (!s_handSplat) { s_prevHandUV = huv; s_handSplat = true; }
                    glm::vec2 d = huv - s_prevHandUV;
                    for (auto* f : fluids)
                        f->queuePointerSplat(huv.x, huv.y, d.x * kSplatForce, d.y * kSplatForce);
                    s_prevHandUV = huv;
                } else {
                    s_handSplat = false;
                }
            }
        }
        // Floating zone + OUTPUT dock — visible only in Canvas mode.
        m_viewportPanel.renderZoneOutputDock(&m_zones, &m_activeZone,
                                             &monitors, ndiAvail, editorMon);
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
    {
        // Snapshot count before so we can detect a successful add and (when
        // requested) auto-bind the new layer to a Fluid's image source.
        const int prevCount = m_layerStack.count();
        auto bindNewLayerToFluid = [&]() {
            bool any = m_layerPanel.postCreateBindFluidImage ||
                       m_layerPanel.postCreateBindFluid3DImage;
            if (!any) return;
            if (m_layerStack.count() <= prevCount) {
                m_layerPanel.postCreateBindFluidImage = nullptr;
                m_layerPanel.postCreateBindFluid3DImage = nullptr;
                return;
            }
            uint32_t newId = m_layerStack[m_layerStack.count() - 1]->id;
            if (m_layerPanel.postCreateBindFluidImage) {
                m_layerPanel.postCreateBindFluidImage->imageSource().sourceLayerId = newId;
                m_layerPanel.postCreateBindFluidImage->m_imageEnabled = true;
                m_layerPanel.postCreateBindFluidImage = nullptr;
            }
            if (m_layerPanel.postCreateBindFluid3DImage) {
                m_layerPanel.postCreateBindFluid3DImage->imageSource().sourceLayerId = newId;
                m_layerPanel.postCreateBindFluid3DImage->m_imageEnabled = true;
                m_layerPanel.postCreateBindFluid3DImage = nullptr;
            }
        };
        if (m_layerPanel.wantsAddImage) {
            std::string path = openFileDialog("Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0");
            if (!path.empty()) loadImage(path);
            bindNewLayerToFluid();
        }
        if (m_layerPanel.wantsAddVideo) {
            std::string path = openFileDialog("Videos\0*.mp4;*.avi;*.mkv;*.mov;*.webm\0All Files\0*.*\0");
            if (!path.empty()) loadVideo(path);
            bindNewLayerToFluid();
        }
        if (m_layerPanel.wantsAddShader) {
            std::string path = openFileDialog("ISF Shaders\0*.fs;*.frag;*.glsl\0All Files\0*.*\0");
            if (!path.empty()) loadShader(path);
            bindNewLayerToFluid();
        }
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

    // --- Zones control panel (the ZONES workspace tab): every output zone
    // at a glance. The zone on the main display is highlighted; each card
    // shows the projector/output routing and the zone's mic input. Clicking
    // a card makes that zone active (same selection the zone bar drives).
    if (m_ui.isPanelVisible("Zones")) {
        ImGui::Begin("        ###Zones");
        ImDrawList* zdl = ImGui::GetWindowDrawList();
        for (int zi = 0; zi < (int)m_zones.size(); zi++) {
            OutputZone& z = *m_zones[zi];
            bool isMain   = (z.outputDest == OutputDest::Fullscreen) || m_zones.size() == 1;
            bool isActive = (zi == m_activeZone);
            ImGui::PushID(zi);
            float cardW = ImGui::GetContentRegionAvail().x;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Indent(10);
            ImGui::TextUnformatted(z.name.c_str());
            if (isMain) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 1, 1, 0.95f), " MAIN DISPLAY");
            }
            ImGui::TextDisabled("%dx%d", z.width, z.height);
            switch (z.outputDest) {
            case OutputDest::Fullscreen:
                ImGui::Text("Output: fullscreen (monitor %d)", z.outputMonitor);
                break;
            case OutputDest::NDI:
                ImGui::Text("Output: NDI \"%s\"",
                            (z.ndiStreamName.empty() ? z.name : z.ndiStreamName).c_str());
                break;
            case OutputDest::Spout:
                ImGui::TextUnformatted("Output: Spout");
                break;
            default:
                ImGui::TextDisabled("Output: none");
                break;
            }
            if (z.micEnabled) {
                ImGui::Text("Mic: %s%s",
                            z.micDeviceId.empty() ? "system default" : z.micDeviceId.c_str(),
                            z.pushToTalkActive ? "  (LIVE)" : "");
            } else {
                ImGui::TextDisabled("Mic: off");
            }
            if (z.showAllLayers) ImGui::TextDisabled("Layers: all");
            else                 ImGui::TextDisabled("Layers: %d shown", (int)z.visibleLayerIds.size());
            ImGui::Unindent(10);
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::EndGroup();
            ImVec2 p1 = ImVec2(p0.x + cardW, ImGui::GetItemRectMax().y);
            // Main display = bright 2px border + subtle fill; the active
            // (selected) zone gets a mid accent; everything else a hairline.
            ImU32 border = isMain   ? IM_COL32(255, 255, 255, 220)
                         : isActive ? IM_COL32(200, 208, 220, 160)
                                    : IM_COL32(255, 255, 255, 40);
            if (isMain) zdl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14), 8.0f);
            zdl->AddRect(p0, p1, border, 8.0f, 0, isMain ? 2.0f : 1.0f);
            ImGui::SetCursorScreenPos(p0);
            if (ImGui::InvisibleButton("##zoneCard", ImVec2(cardW, p1.y - p0.y)))
                m_activeZone = zi;
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PopID();
        }
        if (m_zones.empty()) ImGui::TextDisabled("No zones yet.");
        ImGui::End();
    }

    // --- Masks section (lives inside the Mapping panel as a collapsible
    // dropdown BELOW the mapping parameters). Closed by default — expand to
    // tweak canvas/layer masks.
    if (m_ui.isPanelVisible("Mapping")) {
    ImGui::Begin("        ###Mapping");
    if (flatSection("Masks"))
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
        ImGui::Text("Clips the final projected output");
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
                // Empty path — user clicks on the canvas to add points,
                // then clicks the first point again (or hits the Save
                // banner) to close the polygon. Matches the prior
                // free-draw mask flow.
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
    m_speechState.recentWordCap = &m_recentWordCap;

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
        m_propertyPanel.render(selectedLayer, m_maskEditMode, &m_speechState, &mosaicAudio, (float)glfwGetTime(), &m_layerStack, &m_bpmSync, &m_sceneManager, &m_mosaicAudioDevice, &m_midiManager, &activeZone(), &m_targetFPS);
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
            // (Mode-transition Alpha fade removed — translucency exposed
            // the GL backbuffer as a white flash during the cross-fade.)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Stage", nullptr,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
                                             &monitors, ndiAvail, editorMon,
                                             [this]() { renderNavBarPrefix(); });
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

            // Reserve right-side width for the anchored Properties+Mapping
            // dock so the 3D viewport reads as the left ~75% of the work
            // area instead of running underneath the right rail. The
            // floating right host width comes from UIManager so the two
            // stay in sync (any margin/host-padding adjustments are made
            // there). We add a small gap so the viewport's border doesn't
            // butt right up against the rail's left edge.
            float panelW    = ImGui::GetContentRegionAvail().x;
            float rightRail = m_ui.rightRailWidth();
            float gap       = 18.0f;
            float viewportW = std::max(240.0f, panelW - rightRail - gap);

            // --- 3D viewport (first) ---
            ImGui::BeginChild("##StageViewport", ImVec2(viewportW, viewportH), false);
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
        if (m_ui.isPanelVisible("Play") && UIManager::sMode == UIManager::WorkspaceMode::Show) {
            // Cmd/Ctrl + = / - / 0 zoom shortcuts for the Show preview.
            // Mirrors the canvas mouse-wheel zoom logic (1.1x per step,
            // clamped to [0.05, 20]). Use io.KeySuper on macOS, Ctrl
            // elsewhere — matches how UIManager::handleZoom dispatches
            // the global UI scale shortcut.
            {
                ImGuiIO& io = ImGui::GetIO();
            #ifdef __APPLE__
                bool zmod = io.KeySuper;
            #else
                bool zmod = io.KeyCtrl;
            #endif
                if (zmod && ImGui::IsKeyPressed(ImGuiKey_Equal)) {
                    m_showZoom = std::min(20.0f, m_showZoom * 1.1f);
                }
                if (zmod && ImGui::IsKeyPressed(ImGuiKey_Minus)) {
                    m_showZoom = std::max(0.05f, m_showZoom / 1.1f);
                }
                if (zmod && ImGui::IsKeyPressed(ImGuiKey_0)) {
                    m_showZoom = 1.0f;
                }
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Play", nullptr,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
                                             &monitors, ndiAvail, editorMon,
                                             [this]() { renderNavBarPrefix(); });
                if (m_viewportPanel.wantsFullscreenToggle()) {
                    m_viewportPanel.clearFullscreenSignal();
                    toggleEditorFullscreen();
                }
            }

            // PLAY workspace top toolbar — zoom + PUBLISH split across
            // two rows. PUBLISH gets its OWN row, left-aligned, so it
            // doesn't collide with the floating right-cluster (audio/
            // OUTPUT panels) that overlays the top-right corner of the
            // workspace area.
            const float kPad = 14.0f;

            // Single toolbar row — zoom controls on the left, a quiet
            // "Publish" affordance on the right. The publish action used
            // to be a loud green pill that fought the chrome; it now reads
            // as part of the toolbar (ghost background, muted text, accent
            // only on hover / brief confirmation flash).
            {
                ImGui::Dummy(ImVec2(0, 4));
                float rowTopY = ImGui::GetCursorPosY();
                float contentRightX = ImGui::GetCursorPosX()
                                    + ImGui::GetContentRegionAvail().x;

                ImGui::Indent(kPad);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
                if (ImGui::SmallButton("-##showzoomout")) {
                    m_showZoom = std::max(0.05f, m_showZoom / 1.1f);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+##showzoomin")) {
                    m_showZoom = std::min(20.0f, m_showZoom * 1.1f);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Fit##showzoomfit")) {
                    m_showZoom = 1.0f;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%.0f%%", m_showZoom * 100.0f);

                // Quiet publish affordance — left-aligned right after the
                // zoom controls so it reads as one toolbar cluster and
                // never collides with the floating audio/OUTPUT panel on
                // the right. Ghost background, muted text, faint green
                // accent only on hover / during the post-publish flash.
                (void)rowTopY; (void)contentRightX;
                ImGui::SameLine(0, 22);
                bool flashing = glfwGetTime() < m_publishFlashUntil;
                const char* pubLabel = flashing
                    ? "\xe2\x9c\x93 Published"           // ✓ Published
                    : "\xe2\x86\x91 Publish to Mobile";  // ↑ Publish to Mobile
                ImGui::PushStyleColor(ImGuiCol_Button,
                                       IM_COL32(255, 255, 255, 10));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                       IM_COL32(0x6E, 0xE7, 0x55, 38));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                       IM_COL32(0x6E, 0xE7, 0x55, 60));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    flashing ? IM_COL32(0x6E, 0xE7, 0x55, 235)
                             : IM_COL32(150, 156, 168, 235));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                // SmallButton inherits the zoom row's FramePadding so it
                // sits flush on the same baseline as - / + / Fit.
                if (ImGui::SmallButton(pubLabel)) {
                    publishPlayToAgent();
                    m_publishFlashUntil = glfwGetTime() + 1.6;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Push this Show (layers + timeline + "
                                       "markers + bpm)\n"
                                       "to paired iPhones via the agent.");
                }
                ImGui::PopStyleVar();      // FrameRounding
                ImGui::PopStyleColor(4);

                ImGui::PopStyleVar();      // FramePadding (zoom row)
                ImGui::Unindent(kPad);
            }

            // Live output preview — shows the active zone's composited
            // output so the performer can see what the audience sees.
            ImGui::Dummy(ImVec2(0, 4));
            ImVec2 fullAvail = ImGui::GetContentRegionAvail();

            // Subtract the floating right-panel width so monitors don't
            // render underneath it (same pattern as the Stage viewport).
            float rightRailW  = m_ui.rightRailWidth();
            float usableW     = std::max(120.0f, fullAvail.x - rightRailW - 18.0f);

            float aspect = 16.0f / 9.0f;
            if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size()) {
                auto& z = *m_zones[m_activeZone];
                if (z.warpFBO.width() > 0 && z.warpFBO.height() > 0)
                    aspect = (float)z.warpFBO.width() / (float)z.warpFBO.height();
            }
            GLuint showOutTex = 0, showPrvTex = 0;
            if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size()) {
                auto& z  = *m_zones[m_activeZone];
                showOutTex = z.warpFBO.textureId();
                showPrvTex = z.canvasTexture ? z.canvasTexture : z.compositor.resultTexture();
            }
            if (!showOutTex) showOutTex = m_testPattern.id();
            if (!showPrvTex) showPrvTex = m_testPattern.id();

            // Each panel gets exactly half the usable width, no gap between them.
            // Height derived from aspect ratio; capped to leave at least 130px for the deck.
            const float kMonPad   = 4.0f;
            const float kDeckMinH = 130.0f;
            float monPW  = floorf(usableW * 0.5f);
            float imgW   = monPW - kMonPad * 2.0f;
            float imgH   = imgW / aspect;
            float hdrH   = ImGui::GetTextLineHeightWithSpacing() + 5.0f + kMonPad;
            float monH   = imgH + hdrH + kMonPad * 2.0f;
            float maxMonH = fullAvail.y - kDeckMinH - 8.0f;
            if (monH > maxMonH) monH = std::max(60.0f, maxMonH);

            auto drawMonitor = [&](const char* id, const char* label, GLuint tex) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMonPad, kMonPad));
                if (ImGui::BeginChild(id, ImVec2(monPW, monH), false,
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                    ImGui::PopStyleVar();
                    ImGui::TextDisabled("%s", label);
                    ImGui::Separator();
                    ImVec2 ia = ImGui::GetContentRegionAvail();
                    float iw = ia.x, ih = iw / aspect;
                    if (ih > ia.y) { ih = ia.y; iw = ih * aspect; }
                    float px = std::max(0.0f, (ia.x - iw) * 0.5f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px);
                    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(iw, ih), ImVec2(0,1), ImVec2(1,0));
                } else { ImGui::PopStyleVar(); }
                ImGui::EndChild();
            };
            drawMonitor("##ShowComp", "Program",  showOutTex);
            ImGui::SameLine(0, 0);
            drawMonitor("##ShowPrev", "Preview",  showPrvTex);

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
    // Icon-pad + ###ID — empty label space gives drawInspectorTabIcons()
    // room to paint the Sources icon over the tab.
    PropertyPanel::PushPanelStyle();  // shared inset so tabs don't jump
    bool sourcesOpen = sourcesVisible && ImGui::Begin("        ###Sources");
    PropertyPanel::PopPanelStyle();
    // 6-pill nav is rendered at the right-dock host level (one bar total).
    // Tab bar — pinned at uniform icon-only width. Reorderable was the
    // culprit behind the "first tab wide / rest clustered" spacing
    // bug: combined with ImGui's tab-cache it preserved a stretched
    // width for the originally-leading tab even after subsequent layouts
    // wanted them all the same size. Disabling Reorderable + forcing
    // ItemSpacing.x to 0 nails the tabs to a tight, uniform horizontal
    // rhythm.
    // The inner Sources tab strip used to live here as an ImGui TabBar
    // wrapping each Shaders / Etherea / Camera / Display section. The
    // 6-pill pinned nav at the top of every right-dock panel
    // (UIManager::renderRightDockNavBar) is now the single source of
    // truth for which source sub-panel is visible, so the strip is gone
    // — it had been rendering as a stranded row of empty "..." ellipsis
    // stubs once its icon overlay was removed. `sourcesTabsOpen` stays
    // as a simple alias of sourcesOpen so the per-section branches keep
    // the same shape and selection now reads from m_ui.activeSourcesTab().
    bool sourcesTabsOpen = sourcesOpen;
    (void)sourcesTabsOpen;
    using ST = UIManager::SourceTab;
    ST sourcesActiveSub = m_ui.activeSourcesTab();

    // ── VOICE tab ──────────────────────────────────────────────────────
    // Mic toggle, audio device picker, decay slider, live transcript.
    // The voice transcript drives `cue.transcript` / `cue.latest` in
    // DataBus, which is what text-shader `msg` inputs auto-bind to.
    // Voice tab moved out of the Sources panel — voice controls are now
    // reachable from the bottom-nav mic button (planned popup). Gate the
    // tab body behind `false` so the voice settings code stays in source
    // history while the tab disappears from the UI.
    if (false && sourcesTabsOpen && ImGui::BeginTabItem("Voice")) {
        ImGui::Dummy(ImVec2(0, 6));

        // Live transcript display
        std::string words = m_dataBus.get("cue.latest");
        if (words.empty()) words = m_dataBus.get("etherea.latest");
        ImVec2 fp = ImGui::GetCursorScreenPos();
        float fw = ImGui::GetContentRegionAvail().x;
        float fh = 56.0f;
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        tdl->AddRectFilled(fp, ImVec2(fp.x + fw, fp.y + fh),
                           IM_COL32(20, 22, 28, 220), 8.0f);
        tdl->AddRect(fp, ImVec2(fp.x + fw, fp.y + fh),
                     IM_COL32(255, 255, 255, 28), 8.0f, 0, 1.0f);
        const char* placeholder = "START TALKING..";
        const char* shown = words.empty() ? placeholder : words.c_str();
        ImU32 textCol = words.empty() ? IM_COL32(110, 118, 130, 220)
                                      : IM_COL32(232, 238, 250, 240);
        ImVec2 tts = ImGui::CalcTextSize(shown);
        tdl->AddText(ImVec2(fp.x + 14.0f, fp.y + (fh - tts.y) * 0.5f),
                     textCol, shown);
        ImGui::Dummy(ImVec2(0, fh + 10.0f));

#ifdef __APPLE__
        // Mic level meter (driven by m_audioRMS)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("MIC LEVEL");
            ImGui::PopStyleColor();
            ImVec2 mp = ImGui::GetCursorScreenPos();
            float mw = ImGui::GetContentRegionAvail().x;
            float mh = 6.0f;
            tdl->AddRectFilled(mp, ImVec2(mp.x + mw, mp.y + mh),
                               IM_COL32(255, 255, 255, 22), mh * 0.5f);
            float lvl = std::min(1.0f, m_audioRMS * 4.0f);
            tdl->AddRectFilled(mp, ImVec2(mp.x + mw * lvl, mp.y + mh),
                               IM_COL32(255, 90, 110, 240), mh * 0.5f);
            ImGui::Dummy(ImVec2(0, mh + 10.0f));
        }

        // Mic toggle
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("MIC");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            float w = ImGui::GetContentRegionAvail().x;
            float switchW = 44.0f, switchH = 22.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - switchW));
            ImVec2 sp = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##miconoff", ImVec2(switchW, switchH));
            ImU32 trackCol = m_voiceContinuous
                ? IM_COL32(255, 90, 110, 220)
                : IM_COL32(255, 255, 255, 28);
            tdl->AddRectFilled(sp, ImVec2(sp.x + switchW, sp.y + switchH),
                               trackCol, switchH * 0.5f);
            float knobR = switchH * 0.5f - 3.0f;
            float knobX = m_voiceContinuous ? sp.x + switchW - knobR - 3.0f : sp.x + knobR + 3.0f;
            tdl->AddCircleFilled(ImVec2(knobX, sp.y + switchH * 0.5f), knobR,
                                 IM_COL32(240, 244, 250, 255));
            if (clicked) {
                m_voiceContinuous = !m_voiceContinuous;
                if (m_voiceContinuous) {
                    if (!m_voiceListening) startVoiceRecording();
                } else {
                    if (m_voiceListening) stopVoiceRecording();
                    m_voiceRestartPending = false;
                }
            }
            ImGui::Dummy(ImVec2(0, 14));
        }
#endif

#ifdef HAS_FFMPEG
        // Audio device picker
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("AUDIO DEVICE");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const char* preview = "Default";
            if (m_selectedAudioDevice >= 0 &&
                m_selectedAudioDevice < (int)m_audioDevices.size()) {
                preview = m_audioDevices[m_selectedAudioDevice].name.c_str();
            }
            if (ImGui::BeginCombo("##voice_audio", preview)) {
                if (ImGui::Selectable("Default", m_selectedAudioDevice == -1)) {
                    m_selectedAudioDevice = -1;
                    m_recorder.setAudioDevice(-1);
                }
                for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                    if (!m_audioDevices[i].isCapture) continue;
                    if (ImGui::Selectable(m_audioDevices[i].name.c_str(),
                                          m_selectedAudioDevice == i)) {
                        m_selectedAudioDevice = i;
                        m_recorder.setAudioDevice(i);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Dummy(ImVec2(0, 10));
        }
#endif

        // Decay slider
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("DECAY");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##voice_decay", &m_voiceDecayDuration,
                               0.5f, 10.0f, "%.1fs");
        }

        ImGui::EndTabItem();
    }

    // ShaderClaw tab
    if (sourcesTabsOpen && sourcesActiveSub == ST::Shader) {
    {
        PropertyPanel::PanelSectionHeader("Shaders", /*firstSection=*/true);
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
            // Connected — show shader browser straight away. The
            // "Connected" status text + Disconnect pill + Refresh icon
            // were removed from the panel header per UX request; those
            // controls live under FILE → Sources in the menu bar instead.
            ImGui::Dummy(ImVec2(0, 6));

            // The Particles entry used to be a full-width button here;
            // it now lives as the first tile in the VFX gallery grid
            // below so it reads as "another shader" instead of a
            // separate command.

            // (Animated hover preview disabled — was running a live shader
            // per hover, which felt like "too much preview" stacked on top
            // of the static thumbnails. Tiles now show only the static
            // thumbnail; hover is just the highlight.)
            bool previewValid = false;

            // Generate static thumbnails (one shader per frame to avoid lag).
            // Rendered at 160x160 for the gallery grid — bigger than the old
            // 48x48 list cell so thumbnails read even at 2x DPI.
            const int kThumbRes = 160;

            // Evict thumbnails for shaders that left the manifest — Refresh
            // used to leave stale entries (GPU textures) until Disconnect.
            if (m_scThumbnails.size() > m_shaderClaw.shaders().size()) {
                std::unordered_set<std::string> live;
                for (const auto& sh : m_shaderClaw.shaders()) live.insert(sh.fullPath);
                for (auto it = m_scThumbnails.begin(); it != m_scThumbnails.end(); ) {
                    if (!live.count(it->first)) it = m_scThumbnails.erase(it);
                    else ++it;
                }
            }

            if (m_scThumbRenderer) {
                m_scThumbRenderer->setResolution(kThumbRes, kThumbRes);
                m_scThumbRenderer->update();
                m_scThumbRenderFrame++;
                if (m_scThumbRenderFrame > 3) {
                    auto& entry = m_scThumbnails[m_scThumbRenderPath];
                    if (!entry.texture) entry.texture = std::make_shared<Texture>();
                    GLint prevFBO;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                    GLuint thumbTex = m_scThumbRenderer->textureId();
                    if (thumbTex != 0) {
                        // GPU-side copy into the cached thumbnail texture.
                        // (This was a synchronous glReadPixels into a CPU
                        // vector — a full pipeline drain mid-show — followed
                        // by a destroy/recreate + re-upload of the texture.)
                        GLuint tempFBO;
                        glGenFramebuffers(1, &tempFBO);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, tempFBO);
                        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, thumbTex, 0);
                        if (!entry.texture->id() ||
                            entry.texture->width() != kThumbRes ||
                            entry.texture->height() != kThumbRes) {
                            entry.texture->createEmpty(kThumbRes, kThumbRes);
                        }
                        glBindTexture(GL_TEXTURE_2D, entry.texture->id());
                        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, kThumbRes, kThumbRes);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                        glDeleteFramebuffers(1, &tempFBO);
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
                        // Size to thumb res BEFORE loadFromFile so the FBO +
                        // any multi-pass ping-pong buffers allocate at 160px
                        // instead of the default 1920x1080. Multi-pass shaders
                        // (e.g. voronoi_growth_text) otherwise spiked ~33MB of
                        // full-res RGBA16F buffers per thumbnail. setResolution
                        // pre-init just stores the dims (no FBO ops yet).
                        m_scThumbRenderer->setResolution(kThumbRes, kThumbRes);
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
            static int s_scSubTab = 0; // 0 = VFX, 1 = Text, 2 = 3D
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
                        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.94f, 0.95f, 0.97f, 1.0f)); // white, visible (2026-07-11)
                    }
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
                    if (ImGui::Button(label, ImVec2(0, 0))) s_scSubTab = idx;
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);
                };
                subTabBtn("VFX",  0);
                ImGui::SameLine(0, 6);
                subTabBtn("Text", 1);
                ImGui::SameLine(0, 6);
                subTabBtn("3D",   2);

                // Right-aligned "Reload" pill — rescans the shader library
                // (manifest + files) so freshly-added shaders appear without
                // a restart. Sits just left of the "+" import pill.
                {
                    const char* reloadLbl = "Reload";
                    float plusW   = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    float reloadW = ImGui::CalcTextSize(reloadLbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    ImGui::SameLine(0, 0);
                    float reloadX = ImGui::GetWindowContentRegionMax().x - plusW - 6.0f - reloadW;
                    if (reloadX > ImGui::GetCursorPosX() + 6.0f) ImGui::SetCursorPosX(reloadX);
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.20f));
                    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.94f, 0.95f, 0.97f, 1.0f)); // white, visible (2026-07-11)
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
                    if (ImGui::Button(reloadLbl) && m_shaderClaw.isConnected()) {
                        m_shaderClaw.refreshManifest();
                        std::cout << "[ShaderClaw] Library refreshed ("
                                  << m_shaderClaw.shaders().size() << " shaders)\n";
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh shader library");
                }

                // Right-aligned "+" pill — same visual treatment as the
                // unselected VFX/Text/3D pills, just sits at the right
                // edge of the row.
                ImGui::SameLine(0, 0);
                const char* refreshLbl = "\xE2\x86\xBA"; // ↺
                const char* importLbl  = "+";
                const float pillGap = 6.0f;
                float framePadX = ImGui::GetStyle().FramePadding.x * 2.0f;
                float refreshW  = ImGui::CalcTextSize(refreshLbl).x + framePadX;
                float importW   = ImGui::CalcTextSize(importLbl).x  + framePadX;
                float clusterX  = ImGui::GetWindowContentRegionMax().x
                                - (refreshW + pillGap + importW);
                if (clusterX > ImGui::GetCursorPosX() + 6.0f)
                    ImGui::SetCursorPosX(clusterX);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.94f, 0.95f, 0.97f, 1.0f)); // white, visible (2026-07-11)
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);

                // Refresh pill — re-reads the manifest / re-scans the shaders
                // directory so shaders added since connecting show up. The
                // file watcher only hot-reloads already-loaded files, so newly
                // added files need this explicit rescan.
                if (ImGui::Button("\xE2\x86\xBA##scRefresh")) {
                    m_shaderClaw.refreshManifest();
                }
                if (ImGui::IsItemHovered())
                    ParamRow::Tooltip("Re-scan for newly added shaders");
                ImGui::SameLine(0, pillGap);

                if (ImGui::Button(importLbl)) {
                    std::string picked = openFileDialog("Fragment shader\0*.fs;*.frag;*.glsl\0\0");
                    if (!picked.empty() && m_shaderClaw.isConnected()) {
                        namespace fs = std::filesystem;
                        try {
                            fs::path src(picked);
                            std::string base = src.filename().string();
                            std::string ext  = src.extension().string();
                            // Force .fs extension so manifest + ShaderSource recognise it
                            if (ext != ".fs") {
                                base = src.stem().string() + ".fs";
                            }
                            fs::path dst = fs::path(m_shaderClaw.shadersDir()) / base;
                            // Don't clobber — append numeric suffix if collision
                            int suffix = 1;
                            while (fs::exists(dst)) {
                                std::string stem = fs::path(base).stem().string();
                                dst = fs::path(m_shaderClaw.shadersDir()) /
                                      (stem + "_" + std::to_string(suffix) + ".fs");
                                suffix++;
                            }
                            fs::copy_file(src, dst);

                            // Append a manifest entry.
                            fs::path manifestPath =
                                fs::path(m_shaderClaw.shadersDir()) / "manifest.json";
                            nlohmann::json manifest = nlohmann::json::array();
                            if (fs::exists(manifestPath)) {
                                std::ifstream mf(manifestPath);
                                if (mf) mf >> manifest;
                            }
                            int nextId = 1000;
                            for (auto& entry : manifest) {
                                if (entry.contains("id") && entry["id"].is_number())
                                    nextId = std::max(nextId, entry["id"].get<int>() + 1);
                            }
                            std::string title = dst.stem().string();
                            // Title-case underscores: "my_shader" → "My Shader"
                            std::string pretty;
                            bool capNext = true;
                            for (char c : title) {
                                if (c == '_' || c == '-') { pretty += ' '; capNext = true; }
                                else if (capNext) { pretty += (char)toupper(c); capNext = false; }
                                else pretty += c;
                            }
                            nlohmann::json entry;
                            entry["id"]          = nextId;
                            entry["title"]       = pretty;
                            entry["description"] = "Imported shader.";
                            entry["type"]        = "generator";
                            entry["categories"]  = nlohmann::json::array({"Imported", "Generator"});
                            entry["file"]        = dst.filename().string();
                            manifest.push_back(entry);

                            std::ofstream out(manifestPath);
                            out << manifest.dump(2, ' ', false,
                                    nlohmann::json::error_handler_t::replace);
                            out.close();

                            m_shaderClaw.refreshManifest();
                            std::cout << "[ShaderClaw] Imported "
                                      << dst.string() << " (id " << nextId << ")\n";
                        } catch (const std::exception& e) {
                            std::cerr << "[ShaderClaw] Import failed: " << e.what() << "\n";
                        }
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);

                ImGui::Dummy(ImVec2(0, 6));
            }

            // ─── Destination: where a clicked shader goes ────────────────
            // Quiet Apple-style row: dim caps label + a rounded white-text
            // dropdown listing Whole House and every zone. Click a tile and
            // the shader lands exactly there — Whole House layers it into
            // every zone; a named zone gets it exclusively.
            static int s_addDestZone = -1;   // -1 = Whole House, else zone idx
            if (s_addDestZone >= (int)m_zones.size()) s_addDestZone = -1;
            {
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.65f, 1.0f));
                ImGui::TextUnformatted("ADD TO");
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 10);
                const char* destPreview = "Whole House";
                if (s_addDestZone >= 0 && m_zones[s_addDestZone])
                    destPreview = m_zones[s_addDestZone]->name.c_str();
                ImGui::SetNextItemWidth(-1);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.12f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(1, 1, 1, 0.18f));
                ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
                if (ImGui::BeginCombo("##shaderAddDest", destPreview)) {
                    if (ImGui::Selectable("Whole House", s_addDestZone < 0))
                        s_addDestZone = -1;
                    for (int zi = 0; zi < (int)m_zones.size(); zi++) {
                        if (!m_zones[zi]) continue;
                        ImGui::PushID(zi);
                        if (ImGui::Selectable(m_zones[zi]->name.c_str(),
                                              s_addDestZone == zi))
                            s_addDestZone = zi;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Where a clicked shader is added.\n"
                                      "Whole House = every zone; a named zone\n"
                                      "gets the shader exclusively.");
                ImGui::Dummy(ImVec2(0, 6));
            }

            // Place a layer so it renders ONLY in one zone: freeze implicit
            // all-layer zones, pull the layer out everywhere, then set it.
            // Shared by the destination dropdown and the tile ... menu.
            auto placeInZoneOnly = [&](uint32_t lid, int zi) {
                if (zi < 0 || zi >= (int)m_zones.size() || !m_zones[zi]) return;
                for (auto& zz : m_zones) {
                    if (!zz) continue;
                    if (zz->showAllLayers) {
                        zz->showAllLayers = false;
                        for (int li = 0; li < m_layerStack.count(); li++)
                            zz->visibleLayerIds.insert(m_layerStack[li]->id);
                    }
                    zz->visibleLayerIds.erase(lid);
                }
                m_zones[zi]->visibleLayerIds.insert(lid);
            };
            // Add a shader routed to the current destination.
            auto addShaderRouted = [&](const std::string& fullPath) {
                int before = m_layerStack.count();
                loadShader(fullPath);
                if (m_layerStack.count() > before && s_addDestZone >= 0)
                    placeInZoneOnly(m_layerStack[m_layerStack.count() - 1]->id,
                                    s_addDestZone);
            };
            // Retag a shader's gallery group (VFX/Text/3D) in the manifest.
            auto setShaderGroup = [&](const std::string& file, const char* group) {
                namespace fs = std::filesystem;
                try {
                    fs::path manifestPath =
                        fs::path(m_shaderClaw.shadersDir()) / "manifest.json";
                    if (!fs::exists(manifestPath)) return;
                    nlohmann::json manifest = nlohmann::json::array();
                    { std::ifstream mf(manifestPath); if (mf) mf >> manifest; }
                    if (!manifest.is_array()) return;
                    for (auto& e : manifest) {
                        if (e.value("file", std::string()) != file) continue;
                        nlohmann::json cats = nlohmann::json::array();
                        for (auto& c : e.value("categories", nlohmann::json::array()))
                            if (c != "Text" && c != "3D") cats.push_back(c);
                        if (strcmp(group, "Text") == 0) cats.push_back("Text");
                        if (strcmp(group, "3D") == 0)   cats.push_back("3D");
                        e["categories"] = cats;
                        break;
                    }
                    std::ofstream out(manifestPath);
                    out << manifest.dump(2, ' ', false,
                            nlohmann::json::error_handler_t::replace);
                    out.close();
                    m_shaderClaw.refreshManifest();
                } catch (const std::exception& e) {
                    std::cerr << "[ShaderClaw] Group retag failed: " << e.what() << "\n";
                }
            };

            // ─── Gallery grid ───────────────────────────────────────────
            // Big thumbnail tiles laid out in a responsive grid (auto-sizes
            // columns to panel width). Each tile = thumbnail + title. Click a
            // thumbnail to add that shader as a layer; hover to see it
            // animated. Works like a shader-picker gallery.
            std::string hoveredPath;
            const float cellPad     = 8.0f;
            const float starsH      = 16.0f;   // stars row beneath title
            const float labelH      = 22.0f + starsH;
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
                bool is3D   = false;
                for (const auto& c : sh.categories) {
                    if (c == "Text") isText = true;
                    if (c == "3D")   is3D   = true;
                }
                if (!isText && sh.file.rfind("text_", 0) == 0) isText = true;
                bool keep;
                if      (s_scSubTab == 1) keep = isText;
                else if (s_scSubTab == 2) keep = is3D;
                else                       keep = !isText && !is3D; // VFX = neither
                if (keep) visibleIdx.push_back(i);
            }
            // Running grid position so the Particles tile (VFX only) and
            // shader tiles share the same wrap rules.
            int gridPos = 0;

            // Particles tile — only on the VFX sub-tab. Drawn with the
            // same rect / hover treatment as a shader tile so it reads
            // as one of the cards instead of a separate command.
            if (s_scSubTab == 0) {
                ImGui::PushID("particles-card");
                ImVec2 cellPos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##ptile", ImVec2(thumbSize, cellH));
                bool hov = ImGui::IsItemHovered();
                ImDrawList* d = ImGui::GetWindowDrawList();

                ImU32 tileBg   = hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
                ImU32 tileEdge = hov ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 255, 255, 50);
                d->AddRectFilled(cellPos,
                                 ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                                 tileBg, 6.0f);
                d->AddRect(cellPos,
                           ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                           tileEdge, 6.0f, 0, 1.0f);

                ImVec2 thumbMin(cellPos.x + 4, cellPos.y + 4);
                ImVec2 thumbMax(cellPos.x + thumbSize - 4, cellPos.y + thumbSize - 4);
                d->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 18, 30, 255), 4.0f);

                // Drifting dot field — animated so the tile reads as
                // a live shader preview instead of a static icon.
                float t = (float)ImGui::GetTime();
                float thumbW = thumbMax.x - thumbMin.x;
                float thumbH = thumbMax.y - thumbMin.y;
                for (int s = 0; s < 36; s++) {
                    uint32_t h = (uint32_t)s * 2654435761u;
                    float fx = ((h >>  8) & 0xFFFF) / 65535.0f;
                    float fy = ((h >> 16) & 0xFFFF) / 65535.0f;
                    fy = fmodf(fy + t * (0.04f + ((s & 7) / 200.0f)), 1.0f);
                    ImVec2 p(thumbMin.x + fx * thumbW,
                             thumbMin.y + fy * thumbH);
                    float r = 1.2f + ((s % 5) * 0.45f);
                    float a = 0.5f + 0.5f * sinf(t * 1.3f + s * 0.71f);
                    ImU32 col = IM_COL32(220, 230, 255, (int)(120 + 110 * a));
                    d->AddCircleFilled(p, r, col, 8);
                }

                // "+" sigil so the card reads as "create".
                {
                    ImVec2 c((thumbMin.x + thumbMax.x) * 0.5f,
                             (thumbMin.y + thumbMax.y) * 0.5f);
                    float arm = thumbW * 0.18f;
                    float th  = std::max(2.0f, arm * 0.22f);
                    ImU32 plus = hov ? IM_COL32(255, 255, 255, 240)
                                     : IM_COL32(235, 240, 255, 200);
                    d->AddRectFilled(ImVec2(c.x - arm,        c.y - th * 0.5f),
                                     ImVec2(c.x + arm,        c.y + th * 0.5f),
                                     plus, th * 0.5f);
                    d->AddRectFilled(ImVec2(c.x - th * 0.5f,  c.y - arm),
                                     ImVec2(c.x + th * 0.5f,  c.y + arm),
                                     plus, th * 0.5f);
                }

                const char* title = "Particles";
                ImVec2 ts = ImGui::CalcTextSize(title);
                d->AddText(ImVec2(cellPos.x + (thumbSize - ts.x) * 0.5f,
                                  cellPos.y + thumbSize - 2),
                           hov ? IM_COL32(255, 255, 255, 255)
                               : IM_COL32(220, 226, 235, 220),
                           title);

                if (hov) ParamRow::Tooltip("Particles\n\nGPU particle system layer");
                if (clicked) addParticles();

                ImGui::PopID();
                gridPos++;
            }

            // Fluid tile — VFX-only, sits beside the Particles card. Adds the
            // native GPU fluid-simulation generator (same as the File-menu
            // "Add Fluid Simulation" / OSC __fluid__), so it's pickable here.
            if (s_scSubTab == 0) {
                if (gridPos % cols != 0) ImGui::SameLine(0, cellPad);
                ImGui::PushID("fluid-card");
                ImVec2 cellPos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##ftile", ImVec2(thumbSize, cellH));
                bool hov = ImGui::IsItemHovered();
                ImDrawList* d = ImGui::GetWindowDrawList();

                ImU32 tileBg   = hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
                ImU32 tileEdge = hov ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 255, 255, 50);
                d->AddRectFilled(cellPos,
                                 ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                                 tileBg, 6.0f);
                d->AddRect(cellPos,
                           ImVec2(cellPos.x + thumbSize, cellPos.y + cellH),
                           tileEdge, 6.0f, 0, 1.0f);

                ImVec2 thumbMin(cellPos.x + 4, cellPos.y + 4);
                ImVec2 thumbMax(cellPos.x + thumbSize - 4, cellPos.y + thumbSize - 4);
                d->AddRectFilled(thumbMin, thumbMax, IM_COL32(8, 12, 24, 255), 4.0f);

                // Flowing wavy bands — reads as a live fluid preview.
                d->PushClipRect(thumbMin, thumbMax, true);
                float t  = (float)ImGui::GetTime();
                float tw = thumbMax.x - thumbMin.x;
                float th = thumbMax.y - thumbMin.y;
                const ImU32 band[3] = { IM_COL32( 90, 200, 255, 200),
                                        IM_COL32(255,  90, 200, 200),
                                        IM_COL32(150, 255, 180, 200) };
                for (int b = 0; b < 3; b++) {
                    float baseY = thumbMin.y + th * (0.32f + 0.18f * b);
                    ImVec2 prev(0, 0);
                    for (int s = 0; s <= 24; s++) {
                        float fx = s / 24.0f;
                        float y = baseY + sinf(fx * 6.2831f * 1.5f +
                                               t * (1.0f + 0.3f * b) + b) * th * 0.12f;
                        ImVec2 p(thumbMin.x + fx * tw, y);
                        if (s > 0) d->AddLine(prev, p, band[b], 2.0f);
                        prev = p;
                    }
                }
                d->PopClipRect();

                // "+" sigil so the card reads as "create".
                {
                    ImVec2 c((thumbMin.x + thumbMax.x) * 0.5f,
                             (thumbMin.y + thumbMax.y) * 0.5f);
                    float arm = tw * 0.18f;
                    float thk = std::max(2.0f, arm * 0.22f);
                    ImU32 plus = hov ? IM_COL32(255, 255, 255, 240)
                                     : IM_COL32(235, 240, 255, 200);
                    d->AddRectFilled(ImVec2(c.x - arm,       c.y - thk * 0.5f),
                                     ImVec2(c.x + arm,       c.y + thk * 0.5f),
                                     plus, thk * 0.5f);
                    d->AddRectFilled(ImVec2(c.x - thk * 0.5f, c.y - arm),
                                     ImVec2(c.x + thk * 0.5f, c.y + arm),
                                     plus, thk * 0.5f);
                }

                const char* title = "Fluid";
                ImVec2 ts = ImGui::CalcTextSize(title);
                d->AddText(ImVec2(cellPos.x + (thumbSize - ts.x) * 0.5f,
                                  cellPos.y + thumbSize - 2),
                           hov ? IM_COL32(255, 255, 255, 255)
                               : IM_COL32(220, 226, 235, 220),
                           title);

                if (hov) ParamRow::Tooltip("Fluid\n\nGPU fluid-simulation layer");
                if (clicked) addFluid();

                ImGui::PopID();
                gridPos++;

                // ── 3D Fluid (SPH) tile ──────────────────────────────────
                if (gridPos % cols != 0) ImGui::SameLine(0, cellPad);
                ImGui::PushID("fluid3d-card");
                ImVec2 cellPos3 = ImGui::GetCursorScreenPos();
                bool clicked3 = ImGui::InvisibleButton("##f3tile", ImVec2(thumbSize, cellH));
                bool hov3 = ImGui::IsItemHovered();
                ImDrawList* d3 = ImGui::GetWindowDrawList();

                ImU32 tileBg3   = hov3 ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
                ImU32 tileEdge3 = hov3 ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 255, 255, 50);
                d3->AddRectFilled(cellPos3,
                                  ImVec2(cellPos3.x + thumbSize, cellPos3.y + cellH),
                                  tileBg3, 6.0f);
                d3->AddRect(cellPos3,
                            ImVec2(cellPos3.x + thumbSize, cellPos3.y + cellH),
                            tileEdge3, 6.0f, 0, 1.0f);

                ImVec2 thumbMin3(cellPos3.x + 4, cellPos3.y + 4);
                ImVec2 thumbMax3(cellPos3.x + thumbSize - 4, cellPos3.y + thumbSize - 4);
                d3->AddRectFilled(thumbMin3, thumbMax3, IM_COL32(6, 9, 22, 255), 4.0f);

                // A violet orb with a soft rim — reads as a 3D liquid blob.
                {
                    ImVec2 c((thumbMin3.x + thumbMax3.x) * 0.5f,
                             (thumbMin3.y + thumbMax3.y) * 0.5f);
                    float rad = (thumbMax3.x - thumbMin3.x) * 0.30f;
                    for (int r = 6; r >= 1; r--) {
                        float rr = rad * (float)r / 6.0f;
                        int a = (int)(40 + 160 * (1.0f - (float)r / 6.0f));
                        d3->AddCircleFilled(c, rr, IM_COL32(107, 90, 254, a), 24);
                    }
                    d3->AddCircleFilled(ImVec2(c.x - rad * 0.3f, c.y - rad * 0.35f),
                                        rad * 0.28f, IM_COL32(180, 200, 255, 150), 16);
                }

                // "+" sigil (create affordance), upper corner so it doesn't
                // hide the orb.
                {
                    ImVec2 c(thumbMax3.x - (thumbMax3.x - thumbMin3.x) * 0.16f,
                             thumbMin3.y + (thumbMax3.y - thumbMin3.y) * 0.16f);
                    float arm = (thumbMax3.x - thumbMin3.x) * 0.10f;
                    float thk = std::max(2.0f, arm * 0.28f);
                    ImU32 plus = hov3 ? IM_COL32(255, 255, 255, 240)
                                      : IM_COL32(235, 240, 255, 200);
                    d3->AddRectFilled(ImVec2(c.x - arm, c.y - thk * 0.5f),
                                      ImVec2(c.x + arm, c.y + thk * 0.5f), plus, thk * 0.5f);
                    d3->AddRectFilled(ImVec2(c.x - thk * 0.5f, c.y - arm),
                                      ImVec2(c.x + thk * 0.5f, c.y + arm), plus, thk * 0.5f);
                }

                const char* title3 = "3D Fluid";
                ImVec2 ts3 = ImGui::CalcTextSize(title3);
                d3->AddText(ImVec2(cellPos3.x + (thumbSize - ts3.x) * 0.5f,
                                   cellPos3.y + thumbSize - 2),
                            hov3 ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(220, 226, 235, 220),
                            title3);

                if (hov3) ParamRow::Tooltip("3D Fluid\n\nNative SPH 3D fluid (volumetric)");
                if (clicked3) addFluid3D();

                ImGui::PopID();
                gridPos++;
            }

            for (int vi = 0; vi < (int)visibleIdx.size(); vi++) {
                int i = visibleIdx[vi];
                const auto& shader = shaders[i];
                ImGui::PushID(2000 + i);

                // Layout: new row every `cols` items (sharing the
                // counter with the prepended Particles tile so the
                // grid wraps cleanly).
                if (gridPos % cols != 0) ImGui::SameLine(0, cellPad);
                gridPos++;
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

                // Invisible hit button covering the whole cell.
                // Single-click = show in preview panel. Double-click = add to layer.
                ImGui::InvisibleButton("##tile", ImVec2(thumbSize, cellH));
                bool clicked      = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
                bool singleClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(0);
                bool hov = ImGui::IsItemHovered();

                // Single-click: load into the sidebar preview panel.
                if (singleClicked && m_scPreviewPath != shader.fullPath) {
                    m_scPreviewPath = shader.fullPath;
                    m_scPreviewFrame = 0;
                    m_scPreview = std::make_shared<ShaderSource>();
                    if (!m_scPreview->loadFromFile(shader.fullPath))
                        m_scPreview.reset();
                }
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

                // Title — uppercased, ".fs" stripped, then truncated to fit
                // the cell width. The manifest entry (`shader.title`) stays
                // intact; only the displayed string is transformed.
                std::string title = shader.title.empty() ? "(untitled)"
                                                          : shaderDisplayName(shader.title);
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

                // ── Star rating row (1–5). Hit-test is done via IsMouseClicked
                // + manual rect math (NOT a second InvisibleButton inside the
                // cell — that advanced ImGui's cursor and broke column-wrap).
                // Persists to ~/.easel/shader_ratings.json on click.
                {
                    const float starSize = 11.0f;
                    const float starGap  = 3.0f;
                    const float rowW     = 5 * starSize + 4 * starGap;
                    float       rowX     = cellPos.x + (thumbSize - rowW) * 0.5f;
                    float       rowY     = cellPos.y + thumbSize + 22.0f - 4.0f;
                    int         current  = m_shaderRatings.get(shader.file);

                    int hoverStar = 0;
                    ImVec2 mp = ImGui::GetMousePos();
                    bool   inRow = (mp.y >= rowY - 2 && mp.y <= rowY + starSize + 2 &&
                                    mp.x >= rowX     && mp.x <= rowX + rowW);
                    if (inRow) {
                        for (int s = 1; s <= 5; s++) {
                            float sx = rowX + (s - 1) * (starSize + starGap);
                            if (mp.x >= sx && mp.x < sx + starSize) {
                                hoverStar = s;
                                break;
                            }
                        }
                    }

                    int displayLevel = hoverStar > 0 ? hoverStar : current;
                    for (int s = 1; s <= 5; s++) {
                        float sx = rowX + (s - 1) * (starSize + starGap);
                        ImVec2 c(sx + starSize * 0.5f, rowY + starSize * 0.5f);
                        bool   on = s <= displayLevel;
                        ImU32  col = on
                            ? IM_COL32(255, 210, 80, 230)
                            : IM_COL32(255, 255, 255, hoverStar > 0 ? 70 : 35);
                        ImVec2 pts[10];
                        for (int k = 0; k < 10; k++) {
                            float r = (k % 2 == 0) ? starSize * 0.50f : starSize * 0.21f;
                            float a = -1.5707963f + k * 0.6283185f;
                            pts[k] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
                        }
                        if (on) d->AddConvexPolyFilled(pts, 10, col);
                        else    d->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.0f);
                    }

                    // Click-to-rate. Mouse click while hovering a specific star
                    // sets that rating (or clears it if the same star is re-clicked).
                    if (inRow && hoverStar > 0 && ImGui::IsMouseClicked(0)) {
                        int newRating = (hoverStar == current) ? 0 : hoverStar;
                        m_shaderRatings.set(shader.file, newRating);
                    }
                }

                if (hov) {
                    hoveredPath = shader.fullPath;
                    if (!shader.description.empty()) {
                        char tipBuf[512];
                        snprintf(tipBuf, sizeof(tipBuf), "%s\n\n%s\n\nDrag to timeline track",
                                 shader.title.c_str(), shader.description.c_str());
                        ParamRow::Tooltip(tipBuf);
                    } else {
                        char tipBuf[256];
                        snprintf(tipBuf, sizeof(tipBuf), "%s\n\nDrag to timeline track",
                                 shader.title.c_str());
                        ParamRow::Tooltip(tipBuf);
                    }
                }
                // Drag-to-timeline: drag the tile onto a timeline track to
                // create a new clip (or set the source of an existing one).
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("SC_SHADER_PATH",
                                             shader.fullPath.c_str(),
                                             shader.fullPath.size() + 1);
                    ImGui::TextUnformatted(shader.title.empty() ? shader.file.c_str()
                                                               : shader.title.c_str());
                    ImGui::TextDisabled("Drop onto timeline track");
                    ImGui::EndDragDropSource();
                }

                // Right-click context menu on each shader tile.
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                    ImGui::OpenPopup("##scTileCtx");
                    // stash path for the popup (declared static at top of block below)
                }
                // Use a static so the popup outlives the per-frame hover test.
                {
                    static std::string s_scCtxPath;
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1))
                        s_scCtxPath = shader.fullPath;
                    if (ImGui::BeginPopup("##scTileCtx")) {
                        auto sslash = s_scCtxPath.find_last_of("/\\");
                        std::string bn = (sslash == std::string::npos)
                                         ? s_scCtxPath : s_scCtxPath.substr(sslash + 1);
                        ImGui::TextDisabled("%s", bn.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Add to Layer (new)")) {
                            loadShader(s_scCtxPath);
                        }
                        if (ImGui::MenuItem("Send to Timeline")) {
                            // Add to the selected layer's track at the end of existing clips,
                            // or create a new layer if none is selected.
                            auto baseName2 = [](const std::string& p) -> std::string {
                                auto sl = p.find_last_of("/\\");
                                return (sl == std::string::npos) ? p : p.substr(sl + 1);
                            };
                            int  targetLayer = m_selectedLayer;
                            bool layerExists = (targetLayer >= 0 && targetLayer < m_layerStack.count());
                            if (!layerExists) {
                                // No layer selected — create one.
                                loadShader(s_scCtxPath);
                                // loadShader sets m_selectedLayer; now add a clip below.
                                targetLayer = m_selectedLayer;
                                layerExists = (targetLayer >= 0 && targetLayer < m_layerStack.count());
                            }
                            if (layerExists) {
                                auto layer = m_layerStack[targetLayer];
                                if (layer) {
                                    auto* track = m_timeline.findTrack(layer->id);
                                    double startT = 0.0;
                                    if (track) {
                                        for (const auto& c : track->clips)
                                            startT = std::max(startT, c.startTime + c.duration);
                                    }
                                    if (startT >= m_timeline.duration())
                                        startT = std::max(0.0, m_timeline.duration() - 5.0);
                                    double dur = std::min(5.0, m_timeline.duration() - startT);
                                    if (dur < 0.1) dur = 0.1;
                                    m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                                    auto* nc = m_timeline.addClip(layer->id, startT, dur,
                                                                   baseName2(s_scCtxPath),
                                                                   s_scCtxPath);
                                    if (nc) nc->kind = ClipKind::Shader;
                                }
                            }
                        }
                        ImGui::EndPopup();
                    }
                }

                // Double-click: in Show mode swap top layer source live;
                // in Canvas mode add a new layer as before.
                if (clicked) {
                    if (UIManager::sMode == UIManager::WorkspaceMode::Show) {
                        // Find the top visible layer and hot-swap its source
                        auto* target = (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count())
                            ? m_layerStack[m_selectedLayer].get() : nullptr;
                        if (!target && m_layerStack.count() > 0)
                            target = m_layerStack[m_layerStack.count()-1].get();
                        if (target) {
                            auto src = std::make_shared<ShaderSource>();
                            if (src->loadFromFile(shader.fullPath)) {
                                target->source = src;
                                target->visible = true;
                                target->userHidden = false;
                            }
                        } else {
                            loadShader(shader.fullPath);
                        }
                    } else {
                        loadShader(shader.fullPath);
                    }
                }

                // ── Overflow ⋯ button (top-right of thumb). Manual hit-test
                //    so it doesn't advance the grid cursor. Opens a popup
                //    with Improve / Save Params / Combine / Delete.
                bool menuClick = false;
                ImVec2 menuMin, menuMax;
                {
                    const float btnSz = 22.0f;
                    menuMax = ImVec2(thumbMax.x - 2.0f, thumbMin.y + btnSz + 2.0f);
                    menuMin = ImVec2(menuMax.x - btnSz, menuMax.y - btnSz);
                    ImVec2 mp = ImGui::GetMousePos();
                    bool hoverMenu = (mp.x >= menuMin.x && mp.x <= menuMax.x &&
                                      mp.y >= menuMin.y && mp.y <= menuMax.y);
                    bool show = hov || hoverMenu;
                    if (show) {
                        ImU32 bg = hoverMenu ? IM_COL32(0, 0, 0, 200)
                                             : IM_COL32(0, 0, 0, 130);
                        d->AddRectFilled(menuMin, menuMax, bg, 4.0f);
                        d->AddRect(menuMin, menuMax,
                                   IM_COL32(255, 255, 255, hoverMenu ? 200 : 90),
                                   4.0f, 0, 1.0f);
                        // Three white dots
                        float cy = (menuMin.y + menuMax.y) * 0.5f;
                        float cx = (menuMin.x + menuMax.x) * 0.5f;
                        float dotR = 1.6f;
                        float dotGap = 4.0f;
                        ImU32 dotC = IM_COL32(255, 255, 255, hoverMenu ? 255 : 200);
                        d->AddCircleFilled(ImVec2(cx - dotGap, cy), dotR, dotC, 6);
                        d->AddCircleFilled(ImVec2(cx,           cy), dotR, dotC, 6);
                        d->AddCircleFilled(ImVec2(cx + dotGap, cy), dotR, dotC, 6);
                    }
                    if (hoverMenu && ImGui::IsMouseClicked(0)) {
                        menuClick = true;
                    }
                }

                // Manual hit-test for the menu also has to suppress the
                // surrounding InvisibleButton's click — otherwise we'd
                // both open the menu AND load the shader.
                if (menuClick) {
                    clicked = false;
                    ImGui::OpenPopup("##shaderMenu");
                }

                if (ImGui::BeginPopup("##shaderMenu")) {
                    // ── Zone targeting at ADD time ─────────────────────────
                    // A plain tile click already adds the shader on top in
                    // every zone (registerLayerWithZones). These place it
                    // with an explicit destination instead.
                    if (ImGui::MenuItem("Add to Whole House")) {
                        loadShader(shader.fullPath);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Add on top in every zone\n(same as clicking the tile).");
                    if (ImGui::MenuItem("Whole House — Only This")) {
                        loadShader(shader.fullPath);
                        if (m_layerStack.count() > 0)
                            soloLayerAcrossZones(m_zones,
                                m_layerStack[m_layerStack.count() - 1]->id);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Every zone shows ONLY this shader —\nthe whole house matches.");
                    if (m_zones.size() > 1 && ImGui::BeginMenu("Add to Zone")) {
                        for (int zi = 0; zi < (int)m_zones.size(); zi++) {
                            if (!m_zones[zi]) continue;
                            ImGui::PushID(zi);
                            if (ImGui::MenuItem(m_zones[zi]->name.c_str())) {
                                int before = m_layerStack.count();
                                loadShader(shader.fullPath);
                                if (m_layerStack.count() > before)
                                    placeInZoneOnly(
                                        m_layerStack[m_layerStack.count() - 1]->id, zi);
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    // Gallery group retag — moves the shader between the
                    // VFX / Text / 3D sub-tabs (rewrites manifest categories).
                    if (ImGui::BeginMenu("Group")) {
                        bool isText = false, is3D = false;
                        for (const auto& c : shader.categories) {
                            if (c == "Text") isText = true;
                            if (c == "3D")   is3D   = true;
                        }
                        if (ImGui::MenuItem("VFX",  nullptr, !isText && !is3D))
                            setShaderGroup(shader.file, "VFX");
                        if (ImGui::MenuItem("Text", nullptr, isText))
                            setShaderGroup(shader.file, "Text");
                        if (ImGui::MenuItem("3D",   nullptr, is3D))
                            setShaderGroup(shader.file, "3D");
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();

                    // Save Params — only enabled if this shader is the
                    // active source on the selected layer (so we have
                    // current values to capture).
                    bool canSave = false;
                    std::shared_ptr<ShaderSource> activeShader;
                    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerStack.count()) {
                        auto& sel = m_layerStack[m_selectedLayer];
                        if (sel && sel->source) {
                            auto s = std::dynamic_pointer_cast<ShaderSource>(sel->source);
                            if (s && sel->name == shader.file) {
                                activeShader = s;
                                canSave = true;
                            }
                        }
                    }

                    if (ImGui::MenuItem("Save Params", nullptr, false, canSave)) {
                        if (activeShader) {
                            int n = m_shaderPresets.capture(shader.file, *activeShader);
                            std::cout << "[ShaderPresets] Saved " << n
                                      << " params for " << shader.file << "\n";
                        }
                    }
                    if (m_shaderPresets.has(shader.file)) {
                        if (ImGui::MenuItem("Clear Saved Params")) {
                            m_shaderPresets.clear(shader.file);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Push Further…")) {
                        m_pushPath  = shader.fullPath;
                        m_pushTitle = shader.title;
                        m_pushFile  = shader.file;
                        m_pushInstr[0] = '\0';
                        m_pushCombine = 0;
                        m_shaderImprover.reset();
                        m_pushOpen = true;       // styled panel renders top-level
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete")) {
                        // Move .fs into ~/.easel/trash/ and drop from manifest.
                        namespace fs = std::filesystem;
                        try {
                            const char* home = std::getenv("HOME");
                            fs::path trashDir = fs::path(home ? home : ".")
                                              / ".easel" / "trash";
                            std::error_code ec;
                            fs::create_directories(trashDir, ec);
                            fs::path src(shader.fullPath);
                            fs::path dst = trashDir / src.filename();
                            int suffix = 1;
                            while (fs::exists(dst)) {
                                dst = trashDir / (src.stem().string() + "_" +
                                                  std::to_string(suffix++) +
                                                  src.extension().string());
                            }
                            fs::rename(src, dst, ec);

                            // Drop entry from manifest.
                            fs::path manifestPath =
                                fs::path(m_shaderClaw.shadersDir()) / "manifest.json";
                            if (fs::exists(manifestPath)) {
                                nlohmann::json manifest = nlohmann::json::array();
                                std::ifstream mf(manifestPath);
                                if (mf) mf >> manifest;
                                mf.close();
                                if (manifest.is_array()) {
                                    nlohmann::json kept = nlohmann::json::array();
                                    for (auto& e : manifest) {
                                        if (e.value("file", std::string()) != shader.file)
                                            kept.push_back(e);
                                    }
                                    std::ofstream out(manifestPath);
                                    out << kept.dump(2, ' ', false,
                                            nlohmann::json::error_handler_t::replace);
                                }
                            }
                            m_shaderClaw.refreshManifest();
                            m_shaderPresets.clear(shader.file);
                            std::cout << "[ShaderClaw] Trashed " << shader.file
                                      << " → " << dst.string() << "\n";
                        } catch (const std::exception& e) {
                            std::cerr << "[ShaderClaw] Delete failed: " << e.what() << "\n";
                        }
                    }
                    ImGui::EndPopup();
                }

                if (clicked) addShaderRouted(shader.fullPath);

                ImGui::PopID();
            }

            // (Hover preview removed — was loading a fresh ShaderSource on
            // every tile hover. Static thumbs alone now; less GPU thrash,
            // less visual noise.)
            (void)hoveredPath;
        }
    }
    }  // end ShaderClaw section (was a BeginTabItem block before the inner Sources TabBar was retired)
    // NDI tab (moved from position 4 to 1)
#ifdef HAS_NDI
    // NDI tab folded into "Display" — gated false so the body stays in
    // history while the standalone tab disappears. Plan: relocate the NDI
    // broadcasting + source-list UI inside the Display tab body.
    if (false && sourcesTabsOpen && NDIRuntime::instance().isAvailable() && ImGui::BeginTabItem("NDI")) {
        {
            // --- Broadcasting section ---
            if (flatSection("Broadcasting")) {
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
                            m_ndiOutput.create("Lu");
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

            // --- Receive section — flat headline + small circular-arrow
            //     refresh icon on the far right of the headline row.
            {
                ImGui::Dummy(ImVec2(0, 14));
                ImVec2 hdrPos = ImGui::GetCursorScreenPos();
                float hSize = ImGui::GetFontSize() * 1.4f;
                float aw    = ImGui::GetContentRegionAvail().x;
                ImDrawList* hdrDl = ImGui::GetWindowDrawList();
                hdrDl->AddText(ImGui::GetFont(), hSize, hdrPos,
                               IM_COL32(245, 248, 254, 255), "Receive");

                // Refresh icon — top-right of the headline row, sized
                // to roughly 70% of the headline cap height: big
                // enough to land cleanly as a target without dwarfing
                // the section title.
                float iconSz = hSize * 0.70f;
                ImVec2 iconPos(hdrPos.x + aw - iconSz - 2.0f,
                               hdrPos.y + (hSize - iconSz) * 0.5f);
                ImGui::SetCursorScreenPos(iconPos);
                ImGui::PushID("ndiRefreshIcon");
                bool refreshClicked = ImGui::InvisibleButton("##ndiRefresh",
                                                              ImVec2(iconSz, iconSz));
                bool refreshHov     = ImGui::IsItemHovered();
                ImGui::PopID();
                ImU32 bg = refreshHov ? IM_COL32(255, 255, 255, 32)
                                       : IM_COL32(255, 255, 255, 0);
                hdrDl->AddRectFilled(iconPos,
                                     ImVec2(iconPos.x + iconSz, iconPos.y + iconSz),
                                     bg, 6.0f);
                // Refresh icon — loads assets/icons/refresh.png (rasterised
                // from the user-provided refreshicon.svg) once and renders
                // it as an ImGui::Image overlay inside the headline row.
                static GLuint sRefreshTex = 0;
                static bool   sRefreshTried = false;
                if (!sRefreshTried) {
                    sRefreshTried = true;
                    int rw = 0, rh = 0, rc = 0;
                    unsigned char* d = stbi_load("assets/icons/refresh.png",
                                                 &rw, &rh, &rc, 4);
                    if (d) {
                        glGenTextures(1, &sRefreshTex);
                        glBindTexture(GL_TEXTURE_2D, sRefreshTex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, d);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        stbi_image_free(d);
                    }
                }
                if (sRefreshTex) {
                    ImU32 tint = refreshHov ? IM_COL32(255, 255, 255, 255)
                                             : IM_COL32(255, 255, 255, 220);
                    hdrDl->AddImage((ImTextureID)(intptr_t)sRefreshTex,
                                    iconPos,
                                    ImVec2(iconPos.x + iconSz, iconPos.y + iconSz),
                                    ImVec2(0, 0), ImVec2(1, 1), tint);
                }
                if (refreshHov) ParamRow::Tooltip("Refresh source list");
                if (refreshClicked) m_ndiSources = m_ndiFinder.sources();

                // Reserve headline row vertical space + draw separator
                ImGui::SetCursorScreenPos(hdrPos);
                ImGui::Dummy(ImVec2(aw, hSize + 4.0f));
                float lineY = ImGui::GetCursorScreenPos().y + 2.0f;
                hdrDl->AddLine(ImVec2(hdrPos.x, lineY),
                               ImVec2(hdrPos.x + aw, lineY),
                               IM_COL32(255, 255, 255, 22), 1.0f);
                ImGui::Dummy(ImVec2(0, 6));
            }
            {
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

                    // Add button is left-aligned and first; source name reads
                    // to its right. SmallButton was visually squished (frame
                    // padding y=0); use a regular Button with explicit size so
                    // the pill has comfortable height matching the row.
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    const float addH = ImGui::GetFrameHeight() + 4.0f; // taller than default
                    const float addW = 52.0f;
                    if (ImGui::Button("Add", ImVec2(addW, addH))) {
                        addNDISource(m_ndiSources[i].name, m_ndiSources[i].url);
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine(0, 10);

                    // Single-line, truncated with ellipsis so the row stays
                    // one line and the Add button visually centers with the
                    // title instead of being pinned to the top of a wrap.
                    float remW = ImGui::GetContentRegionAvail().x;
                    std::string display = name;
                    if (ImGui::CalcTextSize(display.c_str()).x > remW) {
                        while (display.size() > 1 &&
                               ImGui::CalcTextSize((display + "…").c_str()).x > remW) {
                            display.pop_back();
                        }
                        display += "…";
                    }
                    // Center the title's optical midline against the
                    // Add pill's midline. Use FontSize (cap-height) +
                    // a 1px optical bias since most type sets render
                    // slightly upper-heavy from the baseline.
                    float fontH = ImGui::GetFontSize();
                    float yOff  = (addH - fontH) * 0.5f + 1.0f;
                    if (yOff > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOff);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                    ImGui::TextUnformatted(display.c_str());
                    if (display != name && ImGui::IsItemHovered())
                        ParamRow::Tooltip(name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 2)); // breathing room between rows

                    ImGui::PopID();
                }

#ifdef HAS_WHEP
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 2));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
                ImGui::Text("Scope (WHEP)");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.97f, 1.0f));
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
    // NDI fallback tab also gated — Display tab will surface the
    // "NDI not installed" hint inline instead.
    if (false && sourcesTabsOpen && ImGui::BeginTabItem("NDI")) {
        ImGui::TextDisabled("NDI SDK not installed");
        ImGui::TextWrapped("Place NDI SDK headers in external/ndi/include/ and rebuild to enable NDI support.");
        ImGui::EndTabItem();
    }
#endif


    // Etherea tab
    if (sourcesTabsOpen && sourcesActiveSub == ST::Mic) {
    {
        PropertyPanel::PanelSectionHeader("Voice", /*firstSection=*/true);
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
                // 2+ failed attempts means it isn't "still dialing in" — it's
                // genuinely unable to reach Etherea and is retrying with
                // backoff (up to 60s between tries). Say so plainly instead
                // of leaving "Connecting…" up indefinitely, which reads as
                // frozen/broken rather than "retrying."
                int failedAttempts = m_ethereaClient.wsFailedAttempts();
                bool cantReach = !ws && !sse && failedAttempts >= 2;
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
                                                       : (ws || sse) ? "Partial"
                                                       : cantReach ? "Can't reach Etherea — retrying…"
                                                       : "Connecting…");
                ImGui::PopStyleColor();
                if (cantReach && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("No response from Etherea's server on this address.\n"
                                      "Make sure the Etherea/radio stack is running,\n"
                                      "then Disconnect and Connect again.");
                }
            }

            // Right-aligned Disconnect — neutral pill, not red.
            float discW = ImGui::CalcTextSize("Disconnect").x + 24.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - discW);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.94f, 0.95f, 0.97f, 1.0f)); // white, visible (2026-07-11)
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
                // Doubled from 200/197 — shows roughly twice the scrollback,
                // which is the only "height" lever here since this box is
                // plain wrapped text, not a fixed-size child window.
                if (transcript.size() > 400) transcript = "..." + transcript.substr(transcript.size() - 397);
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
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.87f, 0.92f, 0.85f));
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

        if (flatSection("Scope Stream")) {
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
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.97f, 1.0f));
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
    }  // end Etherea section

    // Particles tab REMOVED — particle creation is exposed inside the
    // ShaderClaw tab as a "+ Particle System" entry above the shader
    // browser, since particles are conceptually another generator source.

#ifdef HAS_OPENCV
    if (sourcesTabsOpen && sourcesActiveSub == ST::Cam) {
        PropertyPanel::PanelSectionHeader("Camera", /*firstSection=*/true);
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

#ifdef __APPLE__
        // ── Body Tracking (MediaPipe-style, via Apple Vision) ──────────
        // Mirrors ShaderClaw3's "Body Tracking" toggle + Hand/Face/Pose
        // mode buttons + Live Signals readout. Runs its own camera
        // session, so it works whether or not a webcam layer is added.
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextUnformatted("Body Tracking");
        ImGui::SameLine();
        {
            bool running = m_visionTracker.isRunning();
            ImGui::PushStyleColor(ImGuiCol_Button,
                running ? ImVec4(0.43f, 0.91f, 0.34f, 0.55f)
                        : ImVec4(1, 1, 1, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                running ? ImVec4(0.43f, 0.91f, 0.34f, 0.75f)
                        : ImVec4(1, 1, 1, 0.22f));
            if (ImGui::Button(running ? "ON ##bt" : "OFF##bt", ImVec2(64, 0))) {
                if (running) m_visionTracker.stop();
                else         m_visionTracker.start();
            }
            ImGui::PopStyleColor(2);
        }

        if (m_visionTracker.isRunning()) {
            // Mode buttons — Hand / Face / Pose. Each is an independent
            // toggle (ShaderClaw3 allows multiple modes at once).
            ImGui::Dummy(ImVec2(0, 4));
            auto modeBtn = [&](const char* label, bool on,
                               std::function<void(bool)> setter) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    on ? ImVec4(0.30f, 0.55f, 0.95f, 0.60f)
                       : ImVec4(1, 1, 1, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    on ? ImVec4(0.30f, 0.55f, 0.95f, 0.80f)
                       : ImVec4(1, 1, 1, 0.20f));
                if (ImGui::Button(label, ImVec2(72, 0))) setter(!on);
                ImGui::PopStyleColor(2);
            };
            modeBtn("Hand", m_visionTracker.handEnabled(),
                    [&](bool v){ m_visionTracker.setHandEnabled(v); });
            ImGui::SameLine();
            modeBtn("Face", m_visionTracker.faceEnabled(),
                    [&](bool v){ m_visionTracker.setFaceEnabled(v); });
            ImGui::SameLine();
            modeBtn("Pose", m_visionTracker.poseEnabled(),
                    [&](bool v){ m_visionTracker.setPoseEnabled(v); });

            // Live Signals readout — the values being pushed to DataBus.
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.65f, 0.72f, 1));
            ImGui::TextUnformatted("Live Signals");
            ImGui::PopStyleColor();
            auto vs = m_visionTracker.signals();
            auto sigBar = [&](const char* label, float v) {
                ImGui::Text("%-14s", label);
                ImGui::SameLine(140);
                ImGui::ProgressBar(std::max(0.0f, std::min(1.0f, v)),
                                   ImVec2(-1, 14));
            };
            if (m_visionTracker.handEnabled()) {
                ImGui::Text("hands: %.0f", vs.handCount);
                sigBar("pinch",      vs.pinch);
                sigBar("hand1.x",    vs.leftHandX);
                sigBar("hand1.y",    vs.leftHandY);
            }
            if (m_visionTracker.poseEnabled()) {
                sigBar("pose.conf",  vs.poseConfidence);
                sigBar("head.x",     vs.headX);
                sigBar("head.y",     vs.headY);
            }
            if (m_visionTracker.faceEnabled()) {
                sigBar("face",       vs.faceDetected);
                sigBar("smile",      vs.smile);
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.62f, 1));
            ImGui::TextWrapped("Bind these in a shader's text/number input "
                               "(vision.hand.pinch, vision.pose.head.x, ...).");
            ImGui::PopStyleColor();
        }
#endif

    }
#endif

    // Display tab (was "Capture") — hosts both screen capture and the
    // NDI source list so all "incoming display feed" routes live together.
    if (sourcesTabsOpen && sourcesActiveSub == ST::Win) {
    {
        PropertyPanel::PanelSectionHeader("Display", /*firstSection=*/true);
#if defined(_WIN32) || defined(__APPLE__)
        if (flatSection("Screen Capture")) {
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

        if (flatSection("Window Capture")) {
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

#ifdef HAS_NDI
        // ── NDI section — folded in from the disabled NDI tab so a
        //    single Capture/Display tab is the home for "incoming pixels":
        //    Screen + Window capture above, NDI sources below.
        ImGui::Dummy(ImVec2(0, 8));
        if (NDIRuntime::instance().isAvailable()) {
            if (flatSection("NDI Sources")) {
                // Auto-refresh every ~2s so the list stays in sync
                // with the network without a manual button press.
                {
                    static double lastRefresh = 0.0;
                    double now = glfwGetTime();
                    if (now - lastRefresh > 2.0) {
                        m_ndiSources = m_ndiFinder.sources();
                        lastRefresh = now;
                    }
                }

                if (m_ndiSources.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::TextWrapped(
                        "Searching for NDI sources on the local network…");
                    ImGui::PopStyleColor();
                } else {
                    for (int i = 0; i < (int)m_ndiSources.size(); i++) {
                        ImGui::PushID(7000 + i);
                        std::string name = m_ndiSources[i].name;
                        if (name.length() > 40) name = name.substr(0, 37) + "...";

                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();

                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.15f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.30f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.50f));
                        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
                        if (ImGui::Button("Add")) {
                            addNDISource(m_ndiSources[i].name, m_ndiSources[i].url);
                        }
                        ImGui::PopStyleColor(4);
                        ImGui::PopID();
                    }
                }
            }

            // ── NDI Network — choose Wi-Fi vs Ethernet + verify reachability
            if (flatSection("NDI Network")) {
                // Append an IP to extraIps (comma-joined, whitespace-trimmed, deduped).
                auto appendExtraIp = [this](const std::string& ipRaw) {
                    auto trim = [](std::string s) {
                        size_t a = s.find_first_not_of(" \t");
                        if (a == std::string::npos) return std::string();
                        size_t b = s.find_last_not_of(" \t");
                        return s.substr(a, b - a + 1);
                    };
                    std::string ip = trim(ipRaw);
                    if (ip.empty()) return;
                    std::vector<std::string> toks;
                    std::stringstream ss(m_ndiNetwork.extraIps);
                    std::string t;
                    while (std::getline(ss, t, ',')) { t = trim(t); if (!t.empty()) toks.push_back(t); }
                    for (auto& e : toks) if (e == ip) return;  // already present
                    toks.push_back(ip);
                    std::string joined;
                    for (size_t k = 0; k < toks.size(); ++k) { if (k) joined += ","; joined += toks[k]; }
                    m_ndiNetwork.extraIps = joined;
                };

                if (m_netAdapters.empty()) m_netAdapters = EnumerateNetworkAdapters();

                std::string curLabel = "Auto (all interfaces)";
                if (m_ndiNetwork.enabled && !m_ndiNetwork.interfaceIp.empty()) {
                    curLabel = m_ndiNetwork.interfaceIp;
                    for (const auto& a : m_netAdapters)
                        if (a.ipv4 == m_ndiNetwork.interfaceIp) {
                            curLabel = std::string(NetAdapterKindTag(a.kind)) + "  -  " + a.ipv4;
                            break;
                        }
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("Send NDI over");
                ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo("##ndiNic", curLabel.c_str())) {
                    if (ImGui::Selectable("Auto (all interfaces)", !m_ndiNetwork.enabled)) {
                        m_ndiNetwork.enabled = false;
                        m_ndiNetwork.interfaceName.clear();
                        m_ndiNetwork.interfaceIp.clear();
                        applyNdiNetworkSettings(true);
                    }
                    for (const auto& a : m_netAdapters) {
                        std::string lbl = std::string(NetAdapterKindTag(a.kind)) + "  -  " + a.ipv4
                                          + "   (" + a.friendlyLabel + ")";
                        bool sel = m_ndiNetwork.enabled && m_ndiNetwork.interfaceIp == a.ipv4;
                        if (ImGui::Selectable(lbl.c_str(), sel)) {
                            m_ndiNetwork.enabled = true;
                            m_ndiNetwork.interfaceName = a.name;
                            m_ndiNetwork.interfaceIp = a.ipv4;
                            applyNdiNetworkSettings(true);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh##ndiNic")) {
                    m_netAdapters = EnumerateNetworkAdapters();
                    refreshNdiPeerStatus();
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::TextWrapped(m_ndiNetwork.enabled
                    ? "Pinned: NDI sends/receives only on this adapter (machine-wide, effective now)."
                    : "Auto: NDI uses all active network adapters.");
                ImGui::PopStyleColor();

                bool pinnedWifi = false;
                if (m_ndiNetwork.enabled)
                    for (const auto& a : m_netAdapters)
                        if (a.ipv4 == m_ndiNetwork.interfaceIp && a.kind == NetAdapterInfo::Kind::WiFi)
                            pinnedWifi = true;
                if (pinnedWifi) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.30f, 1.0f));
                    ImGui::TextWrapped("Wi-Fi can drop frames under load — Ethernet is recommended for reliable NDI.");
                    ImGui::PopStyleColor();
                }

                ImGui::Dummy(ImVec2(0, 6));

                // ── Device Reachability ───────────────────────────────
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("Device Reachability");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton("Re-check##ndiPeers")) refreshNdiPeerStatus();
                if (glfwGetTime() - m_ndiPeerStatusLastRefresh > 5.0) refreshNdiPeerStatus();

                if (m_ndiPeerStatus.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::TextWrapped("No NDI peers discovered yet. If a device is on another subnet or Wi-Fi/Ethernet segment, add its IP under Peer IPs.");
                    ImGui::PopStyleColor();
                } else {
                    for (int i = 0; i < (int)m_ndiPeerStatus.size(); i++) {
                        const auto& p = m_ndiPeerStatus[i];
                        ImGui::PushID(8500 + i);
                        std::string nm = p.name.length() > 36 ? p.name.substr(0, 33) + "..." : p.name;
                        ImGui::PushStyleColor(ImGuiCol_Text, p.reachable
                            ? ImVec4(0.22f, 0.82f, 0.52f, 1.0f) : ImVec4(0.85f, 0.45f, 0.45f, 1.0f));
                        ImGui::TextUnformatted(nm.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        const char* tag = p.reachable
                            ? (p.sameSubnet ? "reachable" : "reachable (cross-subnet)")
                            : (p.sameSubnet ? "no NDI port (firewall?)" : "unreachable (different subnet)");
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                        ImGui::TextUnformatted(tag);
                        ImGui::PopStyleColor();
                        if (!p.reachable && !p.sameSubnet && !p.ip.empty()) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Add IP")) {
                                appendExtraIp(p.ip);
                                applyNdiNetworkSettings(true);
                            }
                        }
                        ImGui::PopID();
                    }
                }

                ImGui::Dummy(ImVec2(0, 4));

                // Manual cross-subnet peer IPs.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("Peer IPs");
                ImGui::PopStyleColor();
                {
                    static char extraBuf[512] = {};
                    bool editing = (ImGui::GetActiveID() == ImGui::GetID("##ndiExtraIps"));
                    if (!editing) {
                        std::strncpy(extraBuf, m_ndiNetwork.extraIps.c_str(), sizeof(extraBuf) - 1);
                        extraBuf[sizeof(extraBuf) - 1] = '\0';
                    }
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::InputText("##ndiExtraIps", extraBuf, sizeof(extraBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        m_ndiNetwork.extraIps = extraBuf;
                        applyNdiNetworkSettings(true);
                    }
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::TextWrapped("Comma-separated IPs of NDI devices on other subnets (queried directly over TCP 5960).");
                ImGui::PopStyleColor();

                ImGui::Dummy(ImVec2(0, 4));

                // Discovery Server — guaranteed cross-subnet mutual discovery.
                if (ImGui::Checkbox("Use NDI Discovery Server", &m_ndiNetwork.useDiscoveryServer))
                    applyNdiNetworkSettings(true);
                if (m_ndiNetwork.useDiscoveryServer) {
                    static char srvBuf[256] = {};
                    bool editingSrv = (ImGui::GetActiveID() == ImGui::GetID("##ndiDiscoSrv"));
                    if (!editingSrv) {
                        std::strncpy(srvBuf, m_ndiNetwork.discoveryServer.c_str(), sizeof(srvBuf) - 1);
                        srvBuf[sizeof(srvBuf) - 1] = '\0';
                    }
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::InputText("##ndiDiscoSrv", srvBuf, sizeof(srvBuf));
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        m_ndiNetwork.discoveryServer = srvBuf;
                        applyNdiNetworkSettings(true);
                    }
                    if (!m_ndiNetwork.discoveryServer.empty()) {
                        if (glfwGetTime() - m_ndiServerUpLastRefresh > 5.0) {
                            m_ndiServerUp = NdiNetworkConfig::tcpProbe(m_ndiNetwork.discoveryServer, 5959);
                            m_ndiServerUpLastRefresh = glfwGetTime();
                        }
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, m_ndiServerUp
                            ? ImVec4(0.22f, 0.82f, 0.52f, 1.0f) : ImVec4(0.85f, 0.45f, 0.45f, 1.0f));
                        ImGui::TextUnformatted(m_ndiServerUp ? "registry reachable" : "cannot reach :5959");
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::TextWrapped("A Discovery Server lets every device find each other across subnets and Wi-Fi/Ethernet boundaries (central registration on TCP 5959). Run it on one always-on host and enter its IP here on every device.");
                ImGui::PopStyleColor();
            }

            // ── NDI Broadcast — outbound senders (composition + per-layer)
            if (flatSection("NDI Broadcast")) {
                {
                    bool compositionOn = m_ndiOutputEnabled && m_ndiOutput.isActive();
                    if (compositionOn)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    else
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    if (ImGui::Checkbox("Easel  (composition)", &m_ndiOutputEnabled)) {
                        if (m_ndiOutputEnabled && !m_ndiOutput.isActive()) {
                            m_ndiOutput.create("Lu");
                        } else if (!m_ndiOutputEnabled && m_ndiOutput.isActive()) {
                            m_ndiOutput.destroy();
                        }
                    }
                    ImGui::PopStyleColor();
                }

                for (int i = 0; i < m_layerStack.count(); i++) {
                    ImGui::PushID(8000 + i);
                    auto& layer = m_layerStack[i];
                    bool active = layer->ndiSender.isActive();
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                    else
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    std::string label = "Easel - " + layer->name;
                    if (label.length() > 50) label = label.substr(0, 47) + "...";
                    ImGui::Checkbox(label.c_str(), &layer->ndiEnabled);
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                if (m_layerStack.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                    ImGui::TextWrapped("Add a layer to broadcast it as an NDI source.");
                    ImGui::PopStyleColor();
                }
            }
        } else {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("NDI runtime not loaded");
            ImGui::TextWrapped(
                "Install the NDI Tools / NDI Runtime from ndi.video, then "
                "restart Easel. The runtime is loaded via dlopen at startup.");
        }
#endif // HAS_NDI
    }
    }  // end Capture (Win/Display) section

#ifdef HAS_SPOUT
    // Spout retained behind `false &&` (Windows-only) since the new pinned
    // 4-icon source nav (Shader / Mic / Cam / Win) has no Spout entry. Re-
    // expose by adding a SourceTab::Spout when needed.
    if (false && sourcesTabsOpen && ImGui::BeginTabItem("Spout")) {
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

    // The inner Sources TabBar + its 7 transparent-chrome PushStyleColor
    // calls were retired with the strip itself — see the matching comment
    // near where `sourcesTabsOpen` is initialised. Only End() the Sources
    // window itself if we Begin'd it.
    if (sourcesVisible) ImGui::End();

// (Stream panel removed — stream key + aspect now live in the GO LIVE
//  popup on the timeline transport bar.)

    // Audio panel — BPM, device, levels, gain controls
    if (m_ui.isPanelVisible("Audio")) {
    PropertyPanel::PushPanelStyle();
    ImGui::Begin("        ###Audio");
    PropertyPanel::PopPanelStyle();
    {
        PropertyPanel::PanelSectionHeader("Audio", /*firstSection=*/true);
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

        // Label-left + combo (taking remaining width minus refresh button) + Refresh.
        float refreshW = ImGui::CalcTextSize("Refresh").x
                       + ImGui::GetStyle().FramePadding.x * 2.0f;
        ParamRow::Begin("INPUT");
        {
            // ParamRow primed the next item to the row remainder; shrink it
            // to leave room for the inline Refresh button.
            float curW = ImGui::CalcItemWidth();
            float comboW = curW - refreshW - ImGui::GetStyle().ItemSpacing.x;
            if (comboW < 80.0f) comboW = 80.0f;
            ImGui::SetNextItemWidth(comboW);
        }
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
        ParamRow::Begin("INPUT");
        ImGui::TextDisabled("Audio device selection requires FFmpeg build");
#endif

        ImGui::Dummy(ImVec2(0, 4));

        // --- SIGNAL — one group: the four band meters side by side, each
        // with ITS gain directly beneath it (meter + name + gain read as one
        // unit per band), then the master strip and the smoothness envelope.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Signal");
        ImGui::PopStyleColor();

        {
            float avail = ImGui::GetContentRegionAvail().x;
            float barH = 80.0f;
            float bandW = (avail - 9.0f) / 4.0f; // 3px gaps between bars
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            struct BandInfo { const char* name; float level; ImU32 color; float* gain; };
            BandInfo bands[4] = {
                { "BASS", m_audioAnalyzer.bass(),    IM_COL32(220, 60, 60, 230),  &m_audioAnalyzer.bassGain() },
                { "LOW",  m_audioAnalyzer.lowMid(),  IM_COL32(230, 150, 30, 230), &m_audioAnalyzer.lowMidGain() },
                { "HI",   m_audioAnalyzer.highMid(), IM_COL32(60, 200, 90, 230),  &m_audioAnalyzer.highMidGain() },
                { "TREB", m_audioAnalyzer.treble(),  IM_COL32(30, 200, 220, 230), &m_audioAnalyzer.trebleGain() },
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

            // Per-band gain drags, one under each meter column.
            float gy = origin.y + barH + ImGui::GetFontSize() + 6.0f;
            for (int b = 0; b < 4; b++) {
                float x0 = origin.x + b * (bandW + 3.0f);
                ImGui::SetCursorScreenPos(ImVec2(x0, gy));
                ImGui::SetNextItemWidth(bandW);
                ImGui::PushID(b + 7100);
                ImGui::DragFloat("##bandGain", bands[b].gain,
                                 0.01f, 0.0f, 5.0f, "%.2fx");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s gain", bands[b].name);
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x, gy + ImGui::GetFrameHeight() + 6.0f));
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
                                IM_COL32(220, 224, 230, 220), 2.0f);
            // Beat dot (right side)
            float beat = m_audioAnalyzer.beatDecay();
            ImU32 beatCol = IM_COL32((int)(50 + 205 * beat), (int)(50 + 100 * beat),
                                     (int)(50 + 50 * beat), 255);
            draw->AddCircleFilled(ImVec2(origin.x + avail - 10, origin.y + h * 0.5f),
                                  5.0f + beat * 4.0f, beatCol);
            ImGui::Dummy(ImVec2(avail, h + 2));
        }

        ImGui::Dummy(ImVec2(0, 6));

        // Rest of the SIGNAL group: master strip (input gain + gate), then
        // the smoothness envelope right next to the meters it shapes.
        // Per-band gains moved under their meters above — no "Gain" section.
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
        gainRow("Input", "##masterGain", &m_audioAnalyzer.inputGain(), 0.0f, 10.0f, "%.2fx");
        gainRow("Gate",  "##nGate",      &m_audioAnalyzer.noiseGate(), 0.0f,  0.5f, "%.2f");
        // Smoothness — the asymmetric envelope on every band the shaders
        // read. Attack = how fast a rising peak lands; Release = how fast it
        // falls back. Lower = more glide.
        gainRow("Attack",  "##audAttack",  &m_audioAnalyzer.smoothAttack(),  0.5f, 30.0f, "%.1f /s");
        gainRow("Release", "##audRelease", &m_audioAnalyzer.smoothRelease(), 0.5f, 30.0f, "%.1f /s");

        // ── Response curve ────────────────────────────────────────────────
        // Smooth transfer curve from raw band energy → the value shaders react
        // to. Pick a band (Master applies on top of all), then shape it:
        // Curve = ease-in/out gamma, Floor/Ceil = soft gate + headroom, S-Curve
        // = smoothstep contrast. The graph is the exact function, with a live
        // green dot showing where the current signal lands on it.
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SeparatorText("Response");

        static int curveBand = CurveBass;

        // ── Preset picker ─────────────────────────────────────────────────
        // One-click easing curves (table lives in AudioAnalyzer.h). Picking a
        // preset writes its AudioCurve into the active band (or all bands if the
        // checkbox is set) and, when the preset specifies one, sets the GLOBAL
        // attack/release smoothing too — so "smoother" presets actually soften
        // the temporal envelope, not just the transfer shape. Smoothing is
        // shared by every band; only the curve is per-band.
        {
            // Default = Ambient on all bands (matches the analyzer's boot
            // defaults in AudioAnalyzer.h — user rule 2026-07-11). Looked up
            // by name so reordering the preset table can't break it.
            static int presetSel = []() {
                for (int p = 0; p < kAudioCurvePresetCount; p++)
                    if (strcmp(kAudioCurvePresets[p].name, "Ambient") == 0)
                        return p;
                return -1;
            }();
            static bool applyAllBands = true;    // off = only the selected band
            const char* preview = (presetSel >= 0 && presetSel < kAudioCurvePresetCount)
                                      ? kAudioCurvePresets[presetSel].name
                                      : "Custom…";

            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.63f, 0.70f, 1.0f));
            ImGui::Text("Preset");
            ImGui::PopStyleColor();
            ImGui::SameLine(64);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##audCurvePreset", preview)) {
                for (int p = 0; p < kAudioCurvePresetCount; p++) {
                    const AudioCurvePreset& pr = kAudioCurvePresets[p];
                    if (ImGui::Selectable(pr.name, presetSel == p)) {
                        presetSel = p;
                        // Apply curve to the active band — or all bands if asked.
                        if (applyAllBands) {
                            for (int b = 0; b < CurveCount; b++)
                                m_audioAnalyzer.curve(b) = pr.curve;
                        } else {
                            m_audioAnalyzer.curve(curveBand) = pr.curve;
                        }
                        // Apply global smoothing only if the preset defines it.
                        if (pr.attack > 0.0f) {
                            m_audioAnalyzer.smoothAttack()  = pr.attack;
                            m_audioAnalyzer.smoothRelease() = pr.release;
                        }
                    }
                    if (ImGui::IsItemHovered() && pr.desc)
                        ImGui::SetTooltip("%s", pr.desc);
                }
                ImGui::EndCombo();
            }
            // Tooltip on the closed combo too, so the feel is discoverable.
            if (presetSel >= 0 && ImGui::IsItemHovered() && kAudioCurvePresets[presetSel].desc)
                ImGui::SetTooltip("%s", kAudioCurvePresets[presetSel].desc);

            ImGui::Dummy(ImVec2(0, 2));
            ImGui::Checkbox("Apply to all bands", &applyAllBands);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On: preset writes every band (Master + Bass/Low/High/Treble).\n"
                                  "Off: preset only shapes the band selected below.");
        }

        ImGui::Dummy(ImVec2(0, 6));

        const char* bandNames[CurveCount] = {"Master", "Bass", "Low", "High", "Treble"};
        for (int b = 0; b < CurveCount; b++) {
            if (b) ImGui::SameLine();
            bool on = (curveBand == b);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.29f, 0.55f, 1.0f, 0.90f));
            if (ImGui::SmallButton(bandNames[b])) curveBand = b;
            if (on) ImGui::PopStyleColor();
        }

        AudioCurve& cv = m_audioAnalyzer.curve(curveBand);

        // Transfer-curve graph.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 gp = ImGui::GetCursorScreenPos();
            float gw = ImGui::GetContentRegionAvail().x;
            float gh = 92.0f;
            ImVec2 gmax(gp.x + gw, gp.y + gh);
            dl->AddRectFilled(gp, gmax, IM_COL32(12, 14, 20, 255), 6.0f);
            dl->AddRect(gp, gmax, IM_COL32(255, 255, 255, 30), 6.0f);
            // identity reference (faint diagonal) + mid gridlines
            dl->AddLine(ImVec2(gp.x, gmax.y), ImVec2(gmax.x, gp.y), IM_COL32(255, 255, 255, 22));
            dl->AddLine(ImVec2(gp.x + gw * 0.5f, gp.y), ImVec2(gp.x + gw * 0.5f, gmax.y), IM_COL32(255, 255, 255, 12));
            dl->AddLine(ImVec2(gp.x, gp.y + gh * 0.5f), ImVec2(gmax.x, gp.y + gh * 0.5f), IM_COL32(255, 255, 255, 12));
            // the curve
            const int STEPS = 56;
            ImVec2 prev;
            for (int i = 0; i <= STEPS; i++) {
                float x = (float)i / STEPS;
                float y = applyAudioCurve(x, cv);
                ImVec2 p(gp.x + x * gw, gp.y + (1.0f - y) * gh);
                if (i > 0) dl->AddLine(prev, p, IM_COL32(120, 180, 255, 235), 2.0f);
                prev = p;
            }
            // live signal dot + drop line
            float lx = std::min(std::max(m_audioAnalyzer.curveInput(curveBand), 0.0f), 1.0f);
            float ly = applyAudioCurve(lx, cv);
            ImVec2 dot(gp.x + lx * gw, gp.y + (1.0f - ly) * gh);
            dl->AddLine(ImVec2(dot.x, gmax.y), dot, IM_COL32(120, 255, 180, 120), 1.0f);
            dl->AddCircleFilled(dot, 4.0f, IM_COL32(120, 255, 180, 255));
            ImGui::Dummy(ImVec2(gw, gh + 4));
        }

        gainRow("Curve",   "##cvExp",   &cv.exponent, 0.20f, 4.0f, "%.2f");
        gainRow("Floor",   "##cvFloor", &cv.floor,    0.00f, 0.9f, "%.2f");
        gainRow("Ceil",    "##cvCeil",  &cv.ceil,     0.10f, 1.0f, "%.2f");
        gainRow("S-Curve", "##cvCon",   &cv.contrast, 0.00f, 1.0f, "%.2f");
        // Keep floor strictly below ceil so the remap never inverts.
        if (cv.floor > cv.ceil - 0.02f) cv.floor = cv.ceil - 0.02f;

        if (ImGui::SmallButton("Reset Curve")) {
            cv = AudioCurve{};
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Gains")) {
            m_audioAnalyzer.inputGain() = 1.0f;
            m_audioAnalyzer.bassGain() = 1.0f;
            m_audioAnalyzer.lowMidGain() = 1.0f;
            m_audioAnalyzer.highMidGain() = 1.0f;
            m_audioAnalyzer.trebleGain() = 1.0f;
            m_audioAnalyzer.noiseGate() = 0.0f;
            m_audioAnalyzer.smoothAttack()  = 8.0f;
            m_audioAnalyzer.smoothRelease() = 3.0f;
            for (int b = 0; b < CurveCount; b++) m_audioAnalyzer.curve(b) = AudioCurve{};
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
                ParamRow::Begin("OUTPUT");
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
                                        isCurrent ? IM_COL32(255, 255, 255, (int)(140 + pulse * 115))
                                                  : IM_COL32(255, 255, 255, 30));
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

        // --- MIDI: device + status, right here in the Audio tab ----------
        ImGui::Dummy(ImVec2(0, 6));
        PropertyPanel::PanelSectionHeader("MIDI", /*firstSection=*/false);
        {
            auto mdevs = m_midiManager.listDevices();
            const char* mlabel = (m_midiManager.isOpen() &&
                                  m_midiManager.deviceIndex() < (int)mdevs.size())
                ? mdevs[m_midiManager.deviceIndex()].c_str() : "None";
            ParamRow::Begin("DEVICE");
            if (ImGui::BeginCombo("##AudioMIDIDevice", mlabel)) {
                if (ImGui::Selectable("None", !m_midiManager.isOpen())) {
                    m_midiManager.closeDevice();
                    m_midiUserDisconnected = true;
                }
                for (int i = 0; i < (int)mdevs.size(); i++) {
                    bool sel = (m_midiManager.isOpen() && m_midiManager.deviceIndex() == i);
                    if (ImGui::Selectable(mdevs[i].c_str(), sel)) {
                        m_midiManager.openDevice(i);
                        m_midiUserDisconnected = false;
                    }
                }
                ImGui::EndCombo();
            }
            if (mdevs.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
                ImGui::TextWrapped("No MIDI controller detected. Plug one in — it auto-connects.");
                ImGui::PopStyleColor();
            } else if (m_midiManager.isOpen()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
                ImGui::Text("Connected");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.85f));
                int seen = (int)m_midiManager.seenCCs().size();
                ImGui::Text("· %d control%s seen", seen, seen == 1 ? "" : "s");
                ImGui::PopStyleColor();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
            ImGui::TextWrapped("To map a knob: open any parameter's bind menu, pick "
                               "MIDI, then move the knob — it learns automatically.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    }  // end Audio visibility guard

    // MIDI panel — device selection + mapping
    if (m_ui.isPanelVisible("MIDI")) {
    PropertyPanel::PushPanelStyle();
    ImGui::Begin("        ###MIDI");
    PropertyPanel::PopPanelStyle();
    {
        PropertyPanel::PanelSectionHeader("MIDI", /*firstSection=*/true);
        auto devices = m_midiManager.listDevices();
        if (devices.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Device");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
            ImGui::TextWrapped("No MIDI devices found. Plug in a controller and reopen this panel.");
            ImGui::PopStyleColor();
        } else {
            // Current device label
            const char* currentLabel = m_midiManager.isOpen()
                ? devices[m_midiManager.deviceIndex()].c_str() : "None";

            ParamRow::Begin("DEVICE");
            if (ImGui::BeginCombo("##MIDIDevice", currentLabel)) {
                // "None" option to disconnect
                if (ImGui::Selectable("None", !m_midiManager.isOpen())) {
                    m_midiManager.closeDevice();
                    m_midiUserDisconnected = true;   // stop auto-reconnecting
                }
                for (int i = 0; i < (int)devices.size(); i++) {
                    bool selected = (m_midiManager.isOpen() && m_midiManager.deviceIndex() == i);
                    if (ImGui::Selectable(devices[i].c_str(), selected)) {
                        m_midiManager.openDevice(i);
                        m_midiUserDisconnected = false;
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
                    ParamRow::Begin("TARGET");
                    ImGui::Combo("##LearnTarget", &learnTarget, targetNames, IM_ARRAYSIZE(targetNames));

                    static int learnLayerIdx = 0;
                    if (learnTarget <= 5) { // Layer targets
                        ParamRow::Begin("LAYER");
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

    // ── Timecode window — Show mode only, toggled via Windows menu ───────────
    if (UIManager::sMode == UIManager::WorkspaceMode::Show && m_showTimecodeWindow) {
        m_prodjlink.poll();
        ImGui::SetNextWindowSize(ImVec2(340, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("Timecode##win", &m_showTimecodeWindow,
                     ImGuiWindowFlags_NoCollapse);
        m_timecodePanel.render(glfwGetTime(), &m_prodjlink);
        ImGui::End();
    }

    // Media panel — categorised browser: Video / Image / Shader / Sources
    if (m_ui.isPanelVisible("Media")) {
    ImGui::Begin("        ###Media");

    // Helper: draws a CollapsingHeader with a transparent cog button on the right.
    // Returns whether the section is open. cogClicked is set when the cog is pressed.
    auto mediaSection = [&](const char* label, const char* cogId, bool& cogClicked) -> bool {
        float cogY  = ImGui::GetCursorPosY();
        bool  open  = ImGui::CollapsingHeader(label,
                          ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ImVec2 next = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - 22.0f, cogY + 2.0f));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.50f,0.53f,0.60f,0.85f));
        cogClicked = ImGui::SmallButton(cogId);
        ImGui::PopStyleColor(3);
        ImGui::SetCursorPos(next);
        return open;
    };

    auto dimText = [](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.38f,0.41f,0.48f,1.0f));
        ImGui::TextWrapped("%s", t);
        ImGui::PopStyleColor();
    };

    bool cogClicked = false;

    // ── Video ────────────────────────────────────────────────────
    if (mediaSection("Video", "⚙##vcog", cogClicked)) {
        ImGui::Indent(8.0f);
        dimText("Drop .mp4 / .mov / .avi files here to add as a video layer.");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Unindent(8.0f);
    }

    // ── Image ────────────────────────────────────────────────────
    if (mediaSection("Image", "⚙##icog", cogClicked)) {
        ImGui::Indent(8.0f);
        dimText("Drop .png / .jpg / .exr files here to add as an image layer.");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Unindent(8.0f);
    }

    // ── Shader ───────────────────────────────────────────────────
    if (mediaSection("Shader", "⚙##shcog", cogClicked)) {
        ImGui::Indent(8.0f);
        if (!m_shaderClaw.isConnected()) {
            dimText("ShaderClaw not connected — link it in the Sources panel.");
        } else {
            const auto& shaders = m_shaderClaw.shaders();
            if (shaders.empty()) {
                dimText("No shaders found. Check the ShaderClaw directory.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f,0.15f,0.18f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.23f,0.28f,1.0f));
                for (const auto& sh : shaders) {
                    std::string lbl = sh.title.empty() ? sh.file : sh.title;
                    ImGui::PushID(sh.file.c_str());
                    if (ImGui::Button(lbl.c_str(), ImVec2(-1, 0)))
                        loadShader(sh.fullPath);
                    ImGui::PopID();
                }
                ImGui::PopStyleColor(2);
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Unindent(8.0f);
    }

    // ── Sources (cameras + NDI) ───────────────────────────────────
    if (mediaSection("Sources", "⚙##srccog", cogClicked)) {
        ImGui::Indent(8.0f);

        // Camera inputs
        ImGui::TextDisabled("CAMERA");
        ImGui::Dummy(ImVec2(0, 2));
#ifdef HAS_OPENCV
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f,0.15f,0.18f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.23f,0.28f,1.0f));
        if (ImGui::Button("Camera 0 (built-in)", ImVec2(-1, 0))) addWebcam(0);
        if (ImGui::Button("Camera 1",             ImVec2(-1, 0))) addWebcam(1);
        ImGui::PopStyleColor(2);
#else
        dimText("Camera capture requires an OpenCV build.");
#endif

        ImGui::Dummy(ImVec2(0, 6));

        // NDI sources
        ImGui::TextDisabled("NDI");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.50f,0.53f,0.60f,0.85f));
        if (ImGui::SmallButton("↺##ndiref"))
            m_ndiSources = m_ndiFinder.sources();
        ImGui::PopStyleColor(3);
        ImGui::Dummy(ImVec2(0, 2));
#ifdef HAS_NDI
        if (m_ndiSources.empty()) {
            dimText("No NDI sources found. Press ↺ to refresh.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f,0.15f,0.18f,1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.23f,0.28f,1.0f));
            for (const auto& s : m_ndiSources) {
                ImGui::PushID(s.name.c_str());
                if (ImGui::Button(s.name.c_str(), ImVec2(-1, 0)))
                    addNDISource(s.name, s.url);
                ImGui::PopID();
            }
            ImGui::PopStyleColor(2);
        }
#else
        dimText("NDI not available in this build.");
#endif
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Unindent(8.0f);
    }

    ImGui::End();
    }  // end Media visibility guard

#if defined(HAS_OPENCV) && !defined(__APPLE__)
    if (m_ui.isPanelVisible("Scene Scanner")) {
        m_scanPanel.render(m_scanner, m_webcam);
    }
#endif

    // Audio Mixer panel merged into Audio; transport controls now live inside
    // the Timeline panel's transport row (renderTransportBar() is a no-op).

    // Timeline panel — floating overlay that slides up from under the bottom
    // nav (Fix 2). Keep rendering while the close animation is still playing
    // (m_timelineAnimT > 0) so it slides DOWN smoothly instead of vanishing.
    if (m_ui.isPanelVisible("Timeline") &&
        (!m_timelineMinimized || m_timelineAnimT > 0.001f)) {
        renderTimelinePanel();
    }
    // Phase 5 — floating transport pill above the docked timeline. Renders
    // play/stop/loop + timecode in a single rounded surface that floats
    // over the canvas, matching reference B's chrome-light vibe.
    renderFloatingTransportPill();
    renderFloatingActionPills();

    // Overlay inspector tab icons (Properties/Mapping/Audio/MIDI) after all
    // panels have rendered so the icon painting lands on top of ImGui's
    // native tab bar text.
    m_ui.drawInspectorTabIcons();
    // (drawSourcesTabIcons removed — the Shader/Mic/Cam/Win quick-switcher
    // now lives pinned at the top of the Properties panel and drives the
    // same Sources tab bar via UIManager::focusSourcesTab. The Sources
    // panel's own tabs still work programmatically; we just no longer
    // paint their decorative icons there.)
    // Activity rail — rendered LAST so its window draws on top of the
    // dockspace + all panels. Earlier render order put it behind. Rail
    // icons toggle which panel (Layers / Sources / Mapping) is active.
    m_ui.renderLeftRail([&](float innerW) {
        // Floating per-layer thumbnails — re-use Layer::textureId() (no extra
        // thumb FBO, pixel-perfect at any scale). Click selects; double-click
        // toggles visibility (iPad-style); drag-drop reorders the stack.
        const float kThumbSz  = 56.0f;
        const float kThumbR   = 10.0f;
        const float kThumbGap = 6.0f;
        const float xPad      = (innerW - kThumbSz) * 0.5f;
        const int   n         = m_layerStack.count();

        // Vertical centering: pad top so the thumbnail stack sits centered
        // in the rail's remaining content region.
        if (n > 0) {
            float stackH = (float)n * kThumbSz + (float)(n - 1) * kThumbGap;
            float availH = ImGui::GetContentRegionAvail().y;
            float topPad = (availH - stackH) * 0.5f;
            if (topPad > 0.0f) ImGui::Dummy(ImVec2(0, topPad));
        }

        // Deferred reorder — apply after the loop so we don't mutate the
        // stack mid-iteration.
        int pendingMoveFrom = -1, pendingMoveTo = -1;

        for (int i = m_layerStack.count() - 1; i >= 0; --i) {
            auto layer = m_layerStack[i];
            if (!layer) continue;
            const bool hidden = !layer->visible;
            ImGui::PushID(20000 + i);
            float curX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(curX + xPad);
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImVec2 sz(kThumbSz, kThumbSz);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background card.
            dl->AddRectFilled(cur, ImVec2(cur.x + sz.x, cur.y + sz.y),
                              IM_COL32(28, 32, 40, 255), kThumbR);

            // Thumbnail image (dim to ~30% alpha when hidden, iPad style).
            GLuint tex = layer->textureId();
            if (tex) {
                ImU32 tint = hidden ? IM_COL32(255, 255, 255, 80)
                                    : IM_COL32(255, 255, 255, 255);
                dl->PushClipRect(cur, ImVec2(cur.x + sz.x, cur.y + sz.y), true);
                dl->AddImageRounded((ImTextureID)(intptr_t)tex,
                                    cur, ImVec2(cur.x + sz.x, cur.y + sz.y),
                                    ImVec2(0, 1), ImVec2(1, 0),  // V-flip GL
                                    tint, kThumbR);
                dl->PopClipRect();
            }

            // Active border — bright when selected; thin gray otherwise.
            bool active = (i == m_selectedLayer);
            ImU32 borderCol = active ? IM_COL32(247, 248, 248, 255)
                                     : IM_COL32(255, 255, 255, 30);
            float borderW = active ? 2.0f : 1.0f;
            dl->AddRect(cur, ImVec2(cur.x + sz.x, cur.y + sz.y),
                        borderCol, kThumbR, 0, borderW);

            // Hidden indicator — diagonal slash so the state is obvious.
            if (hidden) {
                ImVec2 a(cur.x + 8.0f, cur.y + sz.y - 8.0f);
                ImVec2 b(cur.x + sz.x - 8.0f, cur.y + 8.0f);
                dl->AddLine(a, b, IM_COL32(255, 80, 80, 230), 2.5f);
            }

            // Zone-visibility badges — one color-coded dot per zone, stacked
            // down the thumb's left edge on dark backing discs. Filled =
            // layer renders in that zone; dim ring = hidden there. The
            // right-click menu below is the editor for the same state.
            auto railZoneColor = [](int idx) {
                static const ImU32 pal[] = {
                    IM_COL32(255, 179,  71, 235),  // amber
                    IM_COL32(167, 139, 250, 235),  // violet
                    IM_COL32( 74, 222, 128, 235),  // green
                    IM_COL32(251, 113, 133, 235),  // rose
                    IM_COL32(250, 204,  21, 235),  // yellow
                    IM_COL32(148, 163, 184, 235),  // slate
                };
                return pal[idx % (int)(sizeof(pal) / sizeof(pal[0]))];
            };
            if (m_zones.size() > 1) {
                int zc = (int)m_zones.size();
                float gap = 12.0f;
                if ((zc - 1) * gap > sz.y - 12.0f)
                    gap = (sz.y - 12.0f) / (float)(zc - 1);
                float cy = cur.y + sz.y * 0.5f - (zc - 1) * gap * 0.5f;
                float cx = cur.x + 8.0f;
                for (int zi = 0; zi < zc; zi++) {
                    auto& z = *m_zones[zi];
                    bool inZone = z.showAllLayers ||
                                  z.visibleLayerIds.count(layer->id);
                    ImU32 col = railZoneColor(zi);
                    ImU32 dim = IM_COL32((col & 0xFF) / 3,
                                         ((col >> 8) & 0xFF) / 3,
                                         ((col >> 16) & 0xFF) / 3, 150);
                    dl->AddCircleFilled(ImVec2(cx, cy), 5.0f,
                                        IM_COL32(8, 10, 16, 180));
                    if (inZone) dl->AddCircleFilled(ImVec2(cx, cy), 3.2f, col);
                    else        dl->AddCircle(ImVec2(cx, cy), 3.2f, dim, 0, 1.3f);
                    cy += gap;
                }
            }

            // Hit area. Use the per-item activation event + ImGui's own
            // click-count tracker — both anchored to this specific item, so
            // the drag-drop source we attach after it can't interfere with
            // double-click detection. IsItemActivated fires once per click
            // press; MouseClickedCount tells us 1 (single) vs 2+ (double+).
            ImGui::InvisibleButton("##thumb", sz);
            if (ImGui::IsItemActivated()) {
                int n = ImGui::GetIO().MouseClickedCount[0];
                if (n >= 2) {
                    // userHidden is the CANONICAL "user wants this hidden"
                    // flag. Toggling only `visible` looked correct for one
                    // frame, then Timeline.cpp:588 forced visible=true the
                    // next frame (`if (!layer->userHidden) layer->visible
                    // = true;`) — which is why double-click "didn't work."
                    // Same two-line pattern the eye button uses.
                    layer->userHidden = !layer->userHidden;
                    layer->visible    = !layer->userHidden;
                    std::cerr << "[LayerThumb] DOUBLE-CLICK layer " << i
                              << " -> " << (layer->visible ? "SHOW" : "HIDE")
                              << std::endl;
                } else {
                    m_selectedLayer = i;
                    std::cerr << "[LayerThumb] single-click select layer "
                              << i << std::endl;
                }
            }

            // Right-click: per-zone visibility menu for THIS thumbnail.
            // (PushID above scopes the popup id per layer.)
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("RailZoneMenu");
            if (ImGui::BeginPopup("RailZoneMenu")) {
                ImGui::TextDisabled("%s", layer->name.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Visible in:");
                for (int zi = 0; zi < (int)m_zones.size(); zi++) {
                    auto& z = *m_zones[zi];
                    bool inZone = z.showAllLayers ||
                                  z.visibleLayerIds.count(layer->id);
                    if (ImGui::MenuItem(z.name.c_str(), nullptr, inZone)) {
                        if (z.showAllLayers) {
                            // Freeze the implicit all-layers set, then
                            // toggle this one off.
                            z.showAllLayers = false;
                            for (int li = 0; li < m_layerStack.count(); li++)
                                z.visibleLayerIds.insert(m_layerStack[li]->id);
                            z.visibleLayerIds.erase(layer->id);
                        } else if (inZone) {
                            z.visibleLayerIds.erase(layer->id);
                        } else {
                            z.visibleLayerIds.insert(layer->id);
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Show in All Zones"))
                    showLayerInAllZones(m_zones, layer->id);
                if (ImGui::MenuItem("Solo Across All Zones"))
                    soloLayerAcrossZones(m_zones, layer->id);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("One look everywhere — every zone renders\nonly this layer, so the whole house matches.");
                ImGui::EndPopup();
            }

            // Drag source — picking up a thumbnail to reorder.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("LAYER_THUMB_IDX", &i, sizeof(int));
                ImGui::Text("Move layer #%d", i);
                ImGui::EndDragDropSource();
            }
            // Drop target — drop on this thumbnail to move the source layer
            // to this position. Action is queued and applied after the loop.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("LAYER_THUMB_IDX")) {
                    int from = *(const int*)p->Data;
                    if (from != i) {
                        pendingMoveFrom = from;
                        pendingMoveTo   = i;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::Dummy(ImVec2(0, kThumbGap));
            ImGui::PopID();
        }

        // Apply the queued reorder after the loop completes — safe now that
        // we're done iterating.
        if (pendingMoveFrom >= 0 && pendingMoveTo >= 0) {
            m_layerStack.moveLayer(pendingMoveFrom, pendingMoveTo);
            if      (m_selectedLayer == pendingMoveFrom) m_selectedLayer = pendingMoveTo;
            else if (m_selectedLayer == pendingMoveTo)   m_selectedLayer = pendingMoveFrom;
        }
    });
    // Right tool rail removed — the move/rotate/scale/flip/center
    // icons + vertical zoom slider are no longer surfaced. The same
    // transform actions remain available in the Properties panel
    // (Reset / Flip H / Flip V / X / Y / Size / Rot rows).

    // Scenes panel now renders in the Stage-view scope above (where zoneTextures is live).

    // AI "Push Further" panel — top-level so it floats above everything and
    // doesn't depend on which dock tab is active.
    renderPushFurtherPanel();
}

// ── PUSH FURTHER ─ AI shader improve / combine, styled floating panel ──────
// Sends the shader (+ optional second shader) + a text instruction to Claude,
// compile-tests the result, then backs up + overwrites the original in place
// and opens it. The network call lives on a worker thread (ShaderImprover);
// the compile-test + finalize run here because they need the GL context.
void Application::renderPushFurtherPanel() {
    // Finalize a ready candidate every frame (GL thread).
    if (m_shaderImprover.status() == ShaderImprover::Status::ReadyToCompile) {
        namespace fs = std::filesystem;
        std::string tmp  = m_shaderImprover.resultPath();
        std::string orig = m_shaderImprover.origPath();
        auto test = std::make_shared<ShaderSource>();
        if (!tmp.empty() && !orig.empty() && test->loadFromFile(tmp)) {
            std::error_code ec;
            const char* home = std::getenv("HOME");
            fs::path trashDir = fs::path(home ? home : ".") / ".easel" / "trash";
            fs::create_directories(trashDir, ec);
            std::string stem = fs::path(orig).stem().string();
            fs::path bak = trashDir / (stem + ".bak.fs");
            int s = 1;
            while (fs::exists(bak)) bak = trashDir / (stem + ".bak" + std::to_string(s++) + ".fs");
            fs::copy_file(orig, bak, ec);
            fs::copy_file(tmp, orig, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
            if (ec) {
                m_shaderImprover.markError("Couldn't overwrite original shader");
            } else {
                // Reload any layers already running this shader so they
                // update live. Only when NO live layer runs it is a fresh
                // layer loaded — stacking a brand-new full-res shader layer
                // per accepted candidate made every "Push Further" iteration
                // permanently add a rendering layer (plus its FBO chain) on
                // top of the hot-reloaded ones showing the same result.
                int reloadedLayer = -1;
                for (int li = 0; li < m_layerStack.count(); li++) {
                    auto& L = m_layerStack[li];
                    if (L && L->source && L->source->isShader() &&
                        L->source->sourcePath() == orig) {
                        std::ifstream f(orig);
                        std::stringstream ss; ss << f.rdbuf();
                        auto sh = std::dynamic_pointer_cast<ShaderSource>(L->source);
                        if (sh && sh->reload(ss.str()) && reloadedLayer < 0)
                            reloadedLayer = li;
                    }
                }
                m_shaderClaw.refreshManifest();
                if (reloadedLayer >= 0) {
                    m_selectedLayer = reloadedLayer; // result clearly shows
                } else {
                    loadShader(orig);                // result appears on canvas
                }
                m_shaderImprover.markDone(orig);
                std::cerr << "[Improve] done → " << orig << "\n";
            }
        } else {
            std::string glslErr = test ? test->lastError() : std::string();
            std::error_code ec; std::filesystem::remove(tmp, ec);
            // Feed the GLSL compile error back to the model for a self-correcting
            // retry; only surface the real error once retries are exhausted.
            if (!m_shaderImprover.retryWithError(glslErr)) {
                std::string shortErr = glslErr.empty()
                    ? std::string("try a clearer instruction")
                    : glslErr.substr(0, 200);
                m_shaderImprover.markError("Didn't compile after retries — " + shortErr);
            }
        }
    }

    if (!m_pushOpen) return;

    ShaderImprover::Status st = m_shaderImprover.status();
    bool working = (st == ShaderImprover::Status::Working ||
                    st == ShaderImprover::Status::ReadyToCompile);

    // Center it; dim the screen behind with a full-viewport scrim.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    {
        // Background draw list — dims the canvas BEHIND the panel, never over it.
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                          IM_COL32(0, 0, 0, 140));
    }
    const float PANEL_W = 440.0f;
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(PANEL_W, 0), ImGuiCond_Appearing);
    // Focus ONLY on the frame it opens — focusing every frame steals focus
    // back from child popups (the Combine-with dropdown) and closes them.
    static bool s_wasOpen = false;
    if (m_pushOpen && !s_wasOpen) ImGui::SetNextWindowFocus();
    s_wasOpen = m_pushOpen;

    const ImU32 ACCENT  = IM_COL32(150, 120, 255, 255);  // violet
    const ImU32 ACCENT2 = IM_COL32(90, 210, 255, 255);   // cyan
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 16, 22, 252));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(150, 120, 255, 90));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,  IM_COL32(255, 255, 255, 12));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 20));

    bool open = true;
    ImGui::Begin("##pushFurtherPanel", &open,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowSize().x;

    // ── Header: sparkle glyph + title + shader name + close ──
    {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float cx = c.x + 9, cy = c.y + 9;
        // 4-point sparkle in accent
        dl->AddTriangleFilled(ImVec2(cx, cy-9), ImVec2(cx-3, cy), ImVec2(cx+3, cy), ACCENT);
        dl->AddTriangleFilled(ImVec2(cx, cy+9), ImVec2(cx-3, cy), ImVec2(cx+3, cy), ACCENT);
        dl->AddTriangleFilled(ImVec2(cx-9, cy), ImVec2(cx, cy-3), ImVec2(cx, cy+3), ACCENT2);
        dl->AddTriangleFilled(ImVec2(cx+9, cy), ImVec2(cx, cy-3), ImVec2(cx, cy+3), ACCENT2);
        ImGui::SetCursorScreenPos(ImVec2(c.x + 26, c.y));
        ImGui::TextUnformatted("PUSH FURTHER");
        // close ×
        float xs = 16.0f;
        ImVec2 xc(wp.x + ww - 20, c.y + 9);
        bool overX = fabsf(ImGui::GetMousePos().x - xc.x) < 10 &&
                     fabsf(ImGui::GetMousePos().y - xc.y) < 10;
        ImU32 xcol = overX ? IM_COL32(255,255,255,255) : IM_COL32(150,155,168,200);
        dl->AddLine(ImVec2(xc.x-5, xc.y-5), ImVec2(xc.x+5, xc.y+5), xcol, 1.6f);
        dl->AddLine(ImVec2(xc.x-5, xc.y+5), ImVec2(xc.x+5, xc.y-5), xcol, 1.6f);
        if (overX && ImGui::IsMouseClicked(0) && !working) { m_pushOpen = false; }
        (void)xs;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150,155,170,255));
    ImGui::TextWrapped("%s", m_pushTitle.c_str());
    ImGui::PopStyleColor();
    dl->AddLine(ImVec2(wp.x + 18, ImGui::GetCursorScreenPos().y + 4),
                ImVec2(wp.x + ww - 18, ImGui::GetCursorScreenPos().y + 4),
                IM_COL32(255,255,255,24), 1.0f);
    ImGui::Dummy(ImVec2(0, 6));

    // ── Instruction ──
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190,195,210,255));
    ImGui::TextUnformatted("How should it be better?");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##pfInstr", m_pushInstr, sizeof(m_pushInstr),
                              ImVec2(-1, 84));

    // ── Combine-with picker ──
    const auto& sl = m_shaderClaw.shaders();
    std::string cwLabel = "None";
    if (m_pushCombine > 0 && m_pushCombine <= (int)sl.size())
        cwLabel = shaderDisplayName(sl[m_pushCombine-1].title);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190,195,210,255));
    ImGui::TextUnformatted("Combine with");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pfCombine", cwLabel.c_str())) {
        if (ImGui::Selectable("None", m_pushCombine == 0)) m_pushCombine = 0;
        for (int k = 0; k < (int)sl.size(); k++) {
            if (sl[k].file == m_pushFile) continue;
            bool selc = (m_pushCombine == k+1);
            if (ImGui::Selectable(shaderDisplayName(sl[k].title).c_str(), selc))
                m_pushCombine = k+1;
            if (selc) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, 8));

    // ── Generate button (accent gradient, full width, glows on hover) ──
    {
        float bh = 38.0f;
        ImVec2 bp = ImGui::GetCursorScreenPos();
        float bw = ImGui::GetContentRegionAvail().x;
        bool clicked = ImGui::InvisibleButton("##pfGen", ImVec2(bw, bh));
        bool hov = ImGui::IsItemHovered();
        ImU32 a = working ? IM_COL32(70, 60, 110, 255)
                : hov     ? IM_COL32(176, 146, 255, 255) : ACCENT;
        ImU32 b = working ? IM_COL32(50, 70, 95, 255)
                : hov     ? IM_COL32(120, 226, 255, 255) : ACCENT2;
        dl->AddRectFilledMultiColor(bp, ImVec2(bp.x+bw, bp.y+bh), a, b, b, a);
        dl->AddRect(bp, ImVec2(bp.x+bw, bp.y+bh),
                    IM_COL32(255,255,255, hov?120:60), 9.0f, 0, 1.0f);
        const char* lbl = working ? "Working…" : "Generate";
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(bp.x + (bw-ts.x)*0.5f, bp.y + (bh-ts.y)*0.5f),
                    IM_COL32(12, 10, 22, 255), lbl);
        // rounded mask corners (draw bg rounding by overdrawing window bg corners)
        if (clicked && !working) {
            std::string cwPath;
            if (m_pushCombine > 0 && m_pushCombine <= (int)sl.size())
                cwPath = sl[m_pushCombine-1].fullPath;
            // Empty instruction → strong default so combine "just works".
            std::string instr = m_pushInstr;
            while (!instr.empty() && (instr.back()==' '||instr.back()=='\n'||instr.back()=='\t'))
                instr.pop_back();
            if (instr.empty()) {
                instr = cwPath.empty()
                  ? "Push this shader further into a bold, premium, modern result. Add movement, "
                    "depth and a sense of 3D, fluid/organic motion, evolving patterns and shapes, "
                    "rhythm and speed controls, simple physics, and rich color-theory palettes. "
                    "Make it strongly audio-reactive (bass/mid/high) and expose enough sensible "
                    "parameters (speed, intensity, scale, color, reactivity) for live control."
                  : "Combine these two shaders into ONE dope, premium, modern single-pass result "
                    "that fuses both their best ideas. Add movement, depth and a sense of 3D, "
                    "fluid/organic motion, evolving patterns and shapes, rhythm and speed controls, "
                    "simple physics, and rich color-theory palettes. Make it strongly audio-reactive "
                    "(bass/mid/high) and expose enough sensible parameters (speed, intensity, scale, "
                    "color, reactivity) for live control.";
            }
            m_shaderImprover.request(m_pushPath, instr, cwPath, m_shaderClaw.shadersDir());
        }
    }

    // ── LIVE MERGE PREVIEW ── while working, animate the two source shaders
    // fusing: cross-dissolve their thumbnails behind a sweeping scan line +
    // flickering "building-block" flecks, so it reads as a synthesis in progress.
    if (working) {
        auto thumbId = [&](const std::string& full) -> GLuint {
            auto it = m_scThumbnails.find(full);
            if (it != m_scThumbnails.end() && it->second.ready && it->second.texture)
                return it->second.texture->id();
            return 0;
        };
        ImGui::Dummy(ImVec2(0, 8));
        float pw = ImGui::GetContentRegionAvail().x;
        float ph = 120.0f;
        ImVec2 pp = ImGui::GetCursorScreenPos();
        ImVec2 pe(pp.x + pw, pp.y + ph);
        ImGui::Dummy(ImVec2(pw, ph));
        float t = (float)ImGui::GetTime();
        GLuint ta = thumbId(m_pushPath);
        GLuint tb = (m_pushCombine > 0 && m_pushCombine <= (int)sl.size())
                  ? thumbId(sl[m_pushCombine-1].fullPath) : 0;
        dl->PushClipRect(pp, pe, true);
        dl->AddRectFilled(pp, pe, IM_COL32(10, 10, 16, 255), 8.0f);
        // shader A (full), shader B cross-dissolving over it
        float mixB = 0.5f + 0.5f * sinf(t * 0.9f);
        if (ta) dl->AddImageRounded((ImTextureID)(intptr_t)ta, pp, pe,
                    ImVec2(0,1), ImVec2(1,0), IM_COL32(255,255,255,255), 8.0f);
        else dl->AddRectFilledMultiColor(pp, pe, ACCENT, IM_COL32(18,18,28,255),
                    IM_COL32(18,18,28,255), ACCENT2);
        if (tb) dl->AddImageRounded((ImTextureID)(intptr_t)tb, pp, pe,
                    ImVec2(0,1), ImVec2(1,0), IM_COL32(255,255,255,(int)(190*mixB)), 8.0f);
        else dl->AddRectFilled(pp, pe, IM_COL32(90,210,255,(int)(110*mixB)), 8.0f);
        // sweeping scan beam
        float sx = pp.x + fmodf(t * 200.0f, pw);
        dl->AddRectFilled(ImVec2(sx-22, pp.y), ImVec2(sx, pe.y), IM_COL32(150,200,255,46));
        dl->AddRectFilled(ImVec2(sx-1, pp.y), ImVec2(sx+1, pe.y), IM_COL32(190,225,255,210));
        // building-block flecks
        for (int i = 0; i < 12; i++) {
            float fx = fmodf(fabsf(sinf(i*12.9898f))*43758.5f, 1.0f);
            float fy = fmodf(fabsf(sinf(i*78.233f))*4537.5f, 1.0f);
            float bl = 0.5f + 0.5f * sinf(t*5.0f + i*1.7f);
            ImVec2 q(pp.x + fx*pw, pp.y + fy*ph);
            dl->AddRectFilled(q, ImVec2(q.x+7, q.y+7),
                              IM_COL32(180,160,255,(int)(140*bl)), 1.5f);
        }
        // scanlines + frame
        for (float y = pp.y; y < pe.y; y += 3.0f)
            dl->AddLine(ImVec2(pp.x, y), ImVec2(pe.x, y), IM_COL32(0,0,0,38));
        dl->AddRect(pp, pe, IM_COL32(150,120,255,130), 8.0f, 0, 1.0f);
        int dd = (int)(t * 3.0) % 4;
        std::string syn = std::string("SYNTHESIZING") + std::string(dd, '.');
        dl->AddText(ImVec2(pp.x + 10, pp.y + 8), IM_COL32(225,228,255,235), syn.c_str());
        dl->PopClipRect();
    }

    // ── Status row ──
    std::string msg = m_shaderImprover.message();
    if (!msg.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImU32 mc = (st==ShaderImprover::Status::Error) ? IM_COL32(255,120,120,255)
                 : (st==ShaderImprover::Status::Done)  ? IM_COL32(120,230,150,255)
                                                       : IM_COL32(150,190,255,255);
        // animated spinner dots while working
        std::string line = msg;
        if (working) {
            int d = (int)(ImGui::GetTime() * 3.0) % 4;
            line = std::string("Working") + std::string(d, '.');
        }
        ImGui::PushStyleColor(ImGuiCol_Text, mc);
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);
    if (!open && !working) m_pushOpen = false;
}

// Frame-rate hint for the recorder. PTS is wall-clock VFR, so this only feeds the
// GOP cadence + framerate metadata — but a value matching the real render rate keeps
// keyframe spacing sane. Prefer the user's FPS cap if set, else the display refresh.
static int recorderFpsHint(float targetFPS) {
    if (targetFPS > 0.0f) {
        int t = (int)(targetFPS + 0.5f);
        return t < 1 ? 1 : t;
    }
    int fps = 60;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        if (const GLFWvidmode* vm = glfwGetVideoMode(mon)) {
            if (vm->refreshRate > 0) fps = vm->refreshRate;
        }
    }
    if (fps < 24) fps = 24;
    if (fps > 240) fps = 240;
    return fps;
}

// Start an indefinite, live recording of the active zone's output to a timestamped
// .mp4. Records continuously until the user clicks stop — does NOT move the playhead,
// force playback, or bind to the timeline Work Area. Frame timing is wall-clock VFR,
// so the file captures whatever rate the app renders at, at correct playback speed.
void Application::startRecording(const std::string& overridePath) {
#ifdef HAS_FFMPEG
    if (m_recorder.isActive()) return;

    std::string fname = overridePath;
    if (fname.empty()) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buf[160];
        strftime(buf, sizeof(buf), "recordings/%Y%m%d_%H%M%S.mp4", &tm_buf);
        fname = buf;
    }

    auto& zone = activeZone();
    m_recorder.setAudioDevice(m_selectedAudioDevice);
    if (!m_recorder.start(fname, zone.warpFBO.width(), zone.warpFBO.height(), recorderFpsHint(m_targetFPS))) {
        std::cerr << "[REC] Recording failed: recorder.start() returned false\n";
        return;
    }
    std::cerr << "[REC] Recording (indefinite) to " << fname << "\n";
#endif
}

// Minimal show-programming timeline UI. Transport + ruler + per-layer tracks.
// Interactions: click ruler to seek, drag playhead to scrub, drag clip body to
// move, drag clip edges to trim, right-click track for +Clip at cursor.
void Application::renderFloatingTransportPill() {
    // Floating transport pill — reference design:
    //   [⏮] [▶ accent] [⏹] [∞] | [MM:SS / DUR ns] | [🎙] [System Audio ▾] [🔊] [▮▮▮▮▮]
    // Single long pill with grouped sections separated by hairlines. Play
    // is the visual anchor (larger + cyan accent ring + halo when paused).
    if (UIManager::sMode != UIManager::WorkspaceMode::Canvas) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Spacing grid — every gap in this pill must come from one of these.
    // Don't add ad-hoc SameLine(0, N) values; reach for a constant.
    //   kInsetX/Y  : window padding (left/right, top/bottom)
    //   kGap       : between two adjacent inline items (icon→icon, icon→pill)
    //   kDivPad    : each side of a divider — symmetric so the line sits centered
    //   kBtn       : every circular icon button (play uses kPlayBtn but stays centered on the same row baseline)
    const float pillH    = 56.0f;
    const float kInsetX  = 20.0f;
    const float kInsetY  = (pillH - 36.0f) * 0.5f;   // keeps 36px buttons centered
    const float kGap     = 10.0f;
    const float kDivPad  = 12.0f;

    // Full-width bottom bar. Fix 2: the bottom nav slides UP by the current
    // animated timeline height so the timeline pops out from underneath it —
    // the two move as one rigid unit. m_timelineTopY (the single source of
    // truth) is exactly the bottom-nav's top edge, so the pill sits right
    // below it.
    // Fix 1: the pill computes its Y from its OWN always-valid expression and
    // never reads m_timelineTopY (a shared member that is 0 on the first
    // frames / when updateTimelineAnim was skipped). yFlush is the original
    // flush-at-bottom position; subtract only the animated timeline height
    // (m_timelineCurH starts at a valid 0 and only grows as the timeline
    // opens). Result: the pill is correct even if every other timeline
    // member is uninitialised.
    float x = vp->Pos.x;
    float yFlush = vp->Pos.y + vp->Size.y - pillH;
    float y = yFlush - m_timelineCurH;
    // Clamp transport pill to the left edge of the right sidebar.
    float pillW;
    {
        float rpLeft = m_ui.getRightPanelLeft();
        if (rpLeft > vp->Pos.x && rpLeft < vp->Pos.x + vp->Size.x)
            pillW = rpLeft - vp->Pos.x;
        else
            pillW = vp->Size.x;
    }

    // Defensive belt-and-braces: m_timelineCurH should never exceed the
    // viewport, but if a degenerate frame made it huge/negative, snap the
    // pill back to flush-at-bottom so it can never leave the screen.
    if (!(y > vp->Pos.y) || y > yFlush) y = yFlush;
    if (pillW < 1.0f) pillW = vp->Size.x;   // never a zero-width window

    ImGui::SetNextWindowPos (ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pillW, pillH), ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize  |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse|
        ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(kInsetX, kInsetY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // sharp edges — docked bar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    // Bottom bar uses the same family as the top nav (canvas WindowBg
    // = (33, 33, 36)) but a few notches darker so the chrome row reads as
    // its own surface. Top hairline separates it from the canvas above.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(24, 24, 27, 250));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(255, 255, 255, 28));

    if (ImGui::Begin("##TransportPill", nullptr, flags)) {
        // (Drop shadow removed — bar is docked at the bottom edge so a
        // floating shadow would just darken the canvas above for no reason.
        // A 1px top hairline reads cleaner.)
        {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddLine(ImVec2(wp.x, wp.y),
                        ImVec2(wp.x + ws.x, wp.y),
                        IM_COL32(255, 255, 255, 30), 1.0f);
        }

        bool playing = m_timeline.isPlaying();
        const float btnSize  = 36.0f;   // every circular button — same size, no exceptions
        const float playSize = btnSize; // play stays in line; cyan ring is the anchor, not size
        // Single glyph size used for EVERY icon in the row. Don't pass
        // ad-hoc multipliers per icon — that's how the row ended up with
        // visually different-sized icons. One value, one rhythm.
        const float kGlyphSize = btnSize * 0.50f;
        ImDrawList* dl       = ImGui::GetWindowDrawList();
        // Light glyphs on near-black bottom bar. Variable name kept the
        // same so every call site reads as "the primary glyph colour".
        const ImU32 kFgWhite = IM_COL32(235, 240, 250, 245);
        const ImU32 kFgDim   = IM_COL32(140, 148, 165, 220);
        const ImU32 kHair    = IM_COL32(255, 255, 255, 30);

        // ── Helper: glyph-only button (no circle bg, no outline). Click area
        // matches glyph footprint; hover state is communicated by the glyph
        // itself via the per-button drawGlyph callback (callers are tinting
        // kFgWhite/kFgDim already), keeping the row visually quiet.
        auto smallBtn = [&](const char* id,
                            std::function<void(float cx, float cy)> drawGlyph) -> bool {
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImVec2 sz(btnSize, btnSize);
            bool clicked = ImGui::InvisibleButton(id, sz);
            float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
            drawGlyph(cx, cy);
            return clicked;
        };

        // ── Vertical hairline divider (group separator) ────────────────────
        // Symmetric kDivPad on each side so the line is perfectly centered
        // in the gap between groups. Don't change one side without the other.
        auto divider = [&]() {
            ImGui::SameLine(0, kDivPad);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float yMid = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f;
            float halfH = 14.0f;
            dl->AddLine(ImVec2(p.x, yMid - halfH),
                        ImVec2(p.x, yMid + halfH),
                        IM_COL32(255, 255, 255, 28), 1.0f);
            ImGui::Dummy(ImVec2(1, btnSize));
            ImGui::SameLine(0, kDivPad);
        };

        // Layout: LEFT cluster (timeline toggle + timecode), CENTER cluster
        // (Stop / Play / Loop with Play at exact window midpoint), RIGHT
        // cluster (System Audio + Mic + Meter + Fullscreen + REC + STREAM).
        // The three clusters are positioned with absolute SetCursorPosX
        // calls so Play sits dead center regardless of label widths on
        // either side.
        (void)playSize;
        bool looping = m_timeline.looping();

        // ─── LEFT — Mic + Sound dropdown + audio meter, flush left ────────
        // Mic comes first (far left), then Sound dropdown with the level
        // meter immediately to its right.
#ifdef __APPLE__
        if (smallBtn("##fp_mic", [&](float cx, float cy) {
            ImU32 micCol = m_voiceContinuous ? kFgWhite : kFgDim;
            if (m_voiceContinuous) lucide::mic   (dl, cx, cy, kGlyphSize, micCol);
            else                   lucide::micOff(dl, cx, cy, kGlyphSize, micCol);
        })) {
            m_voiceContinuous = !m_voiceContinuous;
            if (m_voiceContinuous) {
                if (!m_voiceListening) startVoiceRecording();
            } else {
                if (m_voiceListening) stopVoiceRecording();
                m_voiceRestartPending = false;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(m_voiceListening
                ? "Mic on — click to mute"
                : (m_voiceContinuous
                    ? "Mic off (no permission yet)"
                    : "Mic muted — click to start"));
        }
        ImGui::SameLine(0, kGap);
#endif

        // Sound dropdown — leading volume-2 icon + (selected device name
        // when picked, otherwise omitted) + chevron. Replaces the bare
        // "Sound" text label so the affordance reads as an audio control
        // at a glance.
        {
            const char* audioLabel = nullptr;
#ifdef HAS_FFMPEG
            if (m_selectedAudioDevice >= 0 &&
                m_selectedAudioDevice < (int)m_audioDevices.size()) {
                audioLabel = m_audioDevices[m_selectedAudioDevice].name.c_str();
            }
#endif
            const float iconW   = 18.0f;
            const float iconGap = (audioLabel && audioLabel[0]) ? 6.0f : 0.0f;
            const float chevW   = 10.0f;
            const float chevGap = 6.0f;
            const float maxLW   = 130.0f;
            ImVec2 ats = audioLabel ? ImGui::CalcTextSize(audioLabel) : ImVec2(0, 0);
            float labelW = std::min(ats.x, maxLW);
            ImVec2 sz(iconW + iconGap + labelW + chevGap + chevW, btnSize);
            ImVec2 cur = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##fp_sound", sz);
            bool hov     = ImGui::IsItemHovered();
            float yMid = cur.y + sz.y * 0.5f;
            ImU32 textCol = hov ? kFgWhite : kFgDim;
            // Icon
            lucide::volume2(dl, cur.x + iconW * 0.5f, yMid, iconW, textCol);
            // Optional device label
            if (audioLabel && audioLabel[0]) {
                char shown[64];
                if (ats.x > maxLW) {
                    int n = std::min((int)strlen(audioLabel), 14);
                    snprintf(shown, sizeof(shown), "%.*s…", n, audioLabel);
                } else {
                    snprintf(shown, sizeof(shown), "%s", audioLabel);
                }
                ImVec2 sts = ImGui::CalcTextSize(shown);
                dl->AddText(ImVec2(cur.x + iconW + iconGap,
                                   yMid - sts.y * 0.5f),
                            textCol, shown);
            }
            // Chevron
            float cxv = cur.x + iconW + iconGap + labelW + chevGap + chevW * 0.5f;
            lucide::chevronDown(dl, cxv, yMid, chevW, textCol);
            // Remember the button's top edge so the popup can open UPWARD
            // (this pill is flush to the viewport bottom; a downward popup
            // would be clipped behind the pill / off-screen).
            m_fpSoundBtnTopX = cur.x;
            m_fpSoundBtnTopY = cur.y;
            if (clicked) {
#ifdef HAS_FFMPEG
                m_audioDevices = VideoRecorder::enumerateAudioDevices();
#endif
                ImGui::OpenPopup("##fp_sound_popup");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sound source");
        }
#ifdef HAS_FFMPEG
        {
            // Anchor the popup just above the mic button with a bottom-left
            // pivot so it grows UPWARD, and cap its height so a long input
            // list scrolls internally instead of overflowing under the pill.
            ImGuiViewport* svp = ImGui::GetMainViewport();
            float maxH = std::min(360.0f, (m_fpSoundBtnTopY - svp->Pos.y) - 12.0f);
            if (maxH < 120.0f) maxH = 120.0f;
            ImGui::SetNextWindowPos(ImVec2(m_fpSoundBtnTopX, m_fpSoundBtnTopY - 6.0f),
                                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0.0f),
                                                ImVec2(FLT_MAX, maxH));
            if (ImGui::BeginPopup("##fp_sound_popup")) {
                ImGui::TextDisabled("Sound capture");
                ImGui::Separator();
                if (ImGui::Selectable("System default", m_selectedAudioDevice == -1)) {
                    m_selectedAudioDevice = -1;
                    m_recorder.setAudioDevice(-1);
                }
                for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                    const auto& dv = m_audioDevices[i];
                    if (!dv.isCapture) continue;
                    if (ImGui::Selectable(dv.name.c_str(), m_selectedAudioDevice == i)) {
                        m_selectedAudioDevice = i;
                        m_recorder.setAudioDevice(i);
                    }
                }
                ImGui::EndPopup();
            }
        }
#endif

        // Audio meter — sits immediately to the right of the Sound dropdown,
        // next to the source that drives it.
        ImGui::SameLine(0, kGap);
        {
            const float meterW = 56.0f;
            const float meterH = 5.0f;
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(meterW, btnSize));
            float yMidM = cur.y + btnSize * 0.5f;
            ImVec2 a(cur.x, yMidM - meterH * 0.5f);
            ImVec2 b(cur.x + meterW, yMidM + meterH * 0.5f);
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 30), meterH * 0.5f);
            float lvl = std::min(1.0f, m_audioRMS * 4.0f);
            if (lvl > 0.01f) {
                ImU32 fillCol = IM_COL32(235, 240, 250, 235);
                dl->AddRectFilled(a, ImVec2(cur.x + meterW * lvl, b.y),
                                  fillCol, meterH * 0.5f);
            }
        }

        // (Duration popup is opened from the right-cluster timecode below;
        //  body kept here so it lives in this function's scope.)
        {
            double dur = m_timeline.duration();
            int dm = (int)dur / 60, ds = (int)dur % 60;
            (void)dm; (void)ds;
            if (ImGui::BeginPopup("##fp_dur_popup")) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(8, 8));

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
                ImGui::TextUnformatted("Timeline duration");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.65f, 1.0f));
                ImGui::TextUnformatted("Quick presets");
                ImGui::PopStyleColor();

                static int s_mm = 0, s_ss = 0;
                int dm = (int)dur / 60, ds = (int)dur % 60;
                if (!ImGui::IsAnyItemActive()) { s_mm = dm; s_ss = ds; }

                auto presetBtn = [&](const char* lbl, int totalSec) {
                    bool match = (totalSec == s_mm * 60 + s_ss);
                    if (match) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.97f, 1.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 1));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.93f, 1.0f));
                    }
                    if (ImGui::Button(lbl, ImVec2(54, 28))) {
                        s_mm = totalSec / 60;
                        s_ss = totalSec % 60;
                        m_timeline.setDuration((double)totalSec);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor(3);
                };
                presetBtn("15s",   15);  ImGui::SameLine();
                presetBtn("30s",   30);  ImGui::SameLine();
                presetBtn("1 min", 60);  ImGui::SameLine();
                presetBtn("2 min", 120);
                presetBtn("5 min", 300); ImGui::SameLine();
                presetBtn("10 min",600); ImGui::SameLine();
                presetBtn("30 min",1800);ImGui::SameLine();
                presetBtn("1 hr",  3600);

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.65f, 1.0f));
                ImGui::TextUnformatted("Custom");
                ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(64);
                ImGui::InputInt("##mm", &s_mm, 1, 5);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.68f, 0.74f, 1.0f));
                ImGui::TextUnformatted("min");
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 14);
                ImGui::SetNextItemWidth(64);
                ImGui::InputInt("##ss", &s_ss, 1, 5);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.68f, 0.74f, 1.0f));
                ImGui::TextUnformatted("sec");
                ImGui::PopStyleColor();

                ImGui::Spacing();
                if (s_mm < 0) s_mm = 0;
                if (s_ss < 0) s_ss = 0;
                if (s_ss > 59) { s_mm += s_ss / 60; s_ss = s_ss % 60; }
                int totalPreview = s_mm * 60 + s_ss;
                if (totalPreview < 1) totalPreview = 1;

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.97f, 1.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 1));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
                char setLabel[32];
                snprintf(setLabel, sizeof(setLabel), "Set duration  (%d:%02d)",
                         totalPreview / 60, totalPreview % 60);
                bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter)
                                  || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
                if (ImGui::Button(setLabel, ImVec2(-1, 32)) || enterPressed) {
                    m_timeline.setDuration((double)totalPreview);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);
                ImGui::EndPopup();
            }
        }

        // Work Area editor — opened by the ##fp_wa pill in the right cluster.
        // Body lives here (same window as the OpenPopup call) so it works
        // even when the timeline overlay is closed (Fix 4).
        {
            double wa0 = m_timeline.workAreaStart();
            double wa1 = m_timeline.workAreaEnd();
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
                    double tldur = m_timeline.duration();
                    if (start < 0) start = 0;
                    if (end > tldur) end = tldur;
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

        // ─── CENTER — Loop, Play, Stop (Play at exact window midpoint) ────
        {
            float winW = ImGui::GetWindowSize().x;
            float clusterW = btnSize * 3.0f + kGap * 2.0f;
            float clusterStartX = (winW - clusterW) * 0.5f;
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(clusterStartX);

            if (smallBtn("##fp_loop", [&](float cx, float cy) {
                ImU32 c = looping ? kFgWhite : kFgDim;
                lucide::infinity(dl, cx, cy, kGlyphSize, c);
            })) m_timeline.setLooping(!looping);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(looping ? "Loop on — click to disable" : "Loop");

            ImGui::SameLine(0, kGap);
            // Play (anchor) — cyan ring instead of hairline.
            {
                ImVec2 cur = ImGui::GetCursorScreenPos();
                ImVec2 sz(btnSize, btnSize);
                bool clicked = ImGui::InvisibleButton("##fp_play", sz);
                bool hov     = ImGui::IsItemHovered();
                float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
                // No background circle, no ring — Play is just the glyph.
                // Slight scale boost on the glyph keeps it as the visual
                // anchor in the row.
                (void)hov;
                float playGlyph = kGlyphSize * 1.18f;
                if (playing) lucide::pause(dl, cx, cy, playGlyph, kFgWhite);
                else         lucide::play (dl, cx, cy, playGlyph, kFgWhite);
                if (clicked) m_timeline.togglePlay();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(playing ? "Pause" : "Play");
            }

            ImGui::SameLine(0, kGap);
            if (smallBtn("##fp_stop", [&](float cx, float cy) {
                lucide::squareFilled(dl, cx, cy, kGlyphSize, kFgWhite);
            })) m_timeline.stop();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop");
        }

        // ─── RIGHT — Timeline-toggle + Timecode + REC + STREAM ───────────
        // (Sound dropdown + audio meter live on the far left of this row;
        // mic lives in the top nav next to the hamburger.)
        {
            // Timecode block width (precomputed so right-cluster anchoring
            // can reserve exactly the space it needs).
            double ph = m_timeline.playhead();
            double dur = m_timeline.duration();
            int pm = (int)ph / 60, ps = (int)ph % 60;
            char tc[16], du[16];
            snprintf(tc, sizeof(tc), "%02d:%02d", pm, ps);
            snprintf(du, sizeof(du), "DUR  %ds", (int)(dur + 0.5));
            ImFont* font = ImGui::GetFont();
            const float baseSize = ImGui::GetFontSize();
            const float bigSize  = baseSize * 1.18f;
            const float smSize   = baseSize * 0.78f;
            const float kInner   = 10.0f;
            ImVec2 tcSize = font->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, tc);
            ImVec2 duSize = font->CalcTextSizeA(smSize,  FLT_MAX, 0.0f, du);
            float tcBlockW = tcSize.x + kInner + duSize.x;

            // Fix 4: Work Area control now lives here (single source of
            // truth — removed from the timeline). Compact pill matching the
            // row rhythm; opens the existing ##WAEditPopup editor.
            double wa0b = m_timeline.workAreaStart();
            double wa1b = m_timeline.workAreaEnd();
            char waBtn[48];
            snprintf(waBtn, sizeof(waBtn), "%d:%02d-%d:%02d",
                     (int)wa0b / 60, (int)wa0b % 60,
                     (int)wa1b / 60, (int)wa1b % 60);
            float waPad   = 10.0f;
            float waBlockW = ImGui::CalcTextSize(waBtn).x + waPad * 2.0f;

            // Total: tcBlock + gap + waBlock + gap + tlBtn + gap + rec + gap + stream
            float rightW = tcBlockW + kGap + waBlockW
                         + kGap + btnSize + kGap + btnSize + kGap + btnSize;

            float winW = ImGui::GetWindowSize().x;
            float startX = winW - rightW - kInsetX;
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(startX);

            // Timecode block (MM:SS  DUR Xs) — single-row layout, click to edit.
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float yMid = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f;
                dl->AddText(font, bigSize,
                            ImVec2(p.x, yMid - tcSize.y * 0.5f),
                            kFgWhite, tc);
                dl->AddText(font, smSize,
                            ImVec2(p.x + tcSize.x + kInner, yMid - duSize.y * 0.5f),
                            kFgDim, du);
                ImGui::SetCursorScreenPos(p);
                if (ImGui::InvisibleButton("##fp_dur", ImVec2(tcBlockW, btnSize))) {
                    ImGui::OpenPopup("##fp_dur_popup");
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Playhead / duration\nClick to edit timeline length");
            }

            // Work Area pill — same kGap rhythm as every other gap in this
            // row. Click opens the Start/End editor (##WAEditPopup, body in
            // renderTimelinePanel). Kept visually quiet to match the bar.
            ImGui::SameLine(0, kGap);
            {
                ImVec2 wcur = ImGui::GetCursorScreenPos();
                ImVec2 wsz(waBlockW, btnSize);
                bool wclk = ImGui::InvisibleButton("##fp_wa", wsz);
                bool whov = ImGui::IsItemHovered();
                ImU32 wbg = whov ? IM_COL32(255, 255, 255, 22)
                                 : IM_COL32(255, 255, 255, 10);
                dl->AddRectFilled(wcur,
                                  ImVec2(wcur.x + wsz.x, wcur.y + wsz.y),
                                  wbg, 6.0f);
                ImVec2 wts = ImGui::CalcTextSize(waBtn);
                dl->AddText(ImVec2(wcur.x + (wsz.x - wts.x) * 0.5f,
                                   wcur.y + (wsz.y - wts.y) * 0.5f),
                            whov ? kFgWhite : kFgDim, waBtn);
                if (wclk) ImGui::OpenPopup("##WAEditPopup");
                if (whov) ImGui::SetTooltip(
                    "Work Area — the range REC will capture.\n"
                    "Click to edit start/end (or I / O at playhead).");
            }

            // Timeline toggle (film+play icon) — opens / collapses the timeline.
            ImGui::SameLine(0, kGap);
            if (smallBtn("##fp_tl_toggle", [&](float cx, float cy) {
                ImU32 c = m_timelineMinimized ? kFgDim : kFgWhite;
                lucide::filmPlay(dl, cx, cy, kGlyphSize, c);
            })) m_timelineMinimized = !m_timelineMinimized;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Timeline");

            // Fit — zoom canvas so it fits within the visible workspace.
            ImGui::SameLine(0, kGap);
            if (smallBtn("##fp_fit", [&](float cx, float cy) {
                dl->AddRect(ImVec2(cx - 6, cy - 5), ImVec2(cx + 6, cy + 5),
                            kFgWhite, 1.5f, 0, 1.5f);
                dl->AddLine(ImVec2(cx - 3, cy - 2), ImVec2(cx + 3, cy + 2), kFgWhite, 1.2f);
            })) {
                m_viewportPanel.setZoom(0.75f);
                m_viewportPanel.resetPan();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit Canvas");

            // Fill — zoom canvas to fill the visible workspace edge-to-edge.
            ImGui::SameLine(0, kGap);
            if (smallBtn("##fp_fill", [&](float cx, float cy) {
                dl->AddRectFilled(ImVec2(cx - 6, cy - 5), ImVec2(cx + 6, cy + 5),
                                  IM_COL32(255, 255, 255, 40), 1.5f);
                dl->AddRect(ImVec2(cx - 6, cy - 5), ImVec2(cx + 6, cy + 5),
                            kFgWhite, 1.5f, 0, 1.5f);
            })) {
                m_viewportPanel.setZoom(1.0f);
                m_viewportPanel.resetPan();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill Canvas");

            // (Audio meter moved to the LEFT next to the Sound dropdown.)

            // REC
#ifdef HAS_FFMPEG
            ImGui::SameLine(0, kGap);
            {
                bool recActive = m_recorder.isActive();
                if (smallBtn("##fp_rec", [&](float cx, float cy) {
                    ImU32 col = recActive ? IM_COL32(255, 90, 90, 245)
                                          : IM_COL32(255, 80, 80, 200);
                    lucide::circleDot(dl, cx, cy, kGlyphSize, col);
                })) {
                    if (m_recorder.isActive()) {
                        m_recorder.stop();
                        m_timelineExporting = false;
                    } else {
                        m_recorder.setAudioDevice(m_selectedAudioDevice);
                        startRecording();
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(recActive ? "Recording — click to stop" : "Record");
            }

            // STREAM
            ImGui::SameLine(0, kGap);
            {
                bool liveActive = m_rtmpOutput.isActive();
                if (smallBtn("##fp_stream", [&](float cx, float cy) {
                    ImU32 c = liveActive ? IM_COL32(74, 230, 144, 245) : kFgWhite;
                    lucide::radio(dl, cx, cy, kGlyphSize, c);
                })) {
                    if (m_rtmpOutput.isActive()) m_rtmpOutput.stop();
                    else                         ImGui::OpenPopup("##fp_stream_popup");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(liveActive ? "Streaming — click to stop" : "Stream to RTMP");
                if (ImGui::BeginPopup("##fp_stream_popup")) {
                    ImGui::TextDisabled("Stream to RTMP");
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
                    ImGui::Text("YouTube Stream Key");
                    ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(260);
                    ImGui::InputText("##fp_streamkey", m_streamKeyBuf, sizeof(m_streamKeyBuf),
                                     ImGuiInputTextFlags_Password);
                    ImGui::Separator();
                    bool hasKey = m_streamKeyBuf[0] != '\0';
                    ImGui::BeginDisabled(!hasKey);
                    if (ImGui::Button("Start streaming", ImVec2(-1, 0))) {
                        auto& z = activeZone();
                        m_rtmpOutput.start(m_streamKeyBuf, z.warpFBO.width(), z.warpFBO.height(),
                                           16, 9, 30);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
            }
#endif
        }
        (void)divider;

        // (legacy pillBtn / mic / REC / STREAM / AUDIO blocks below this point
        //  have been removed — the new top-down layout above is the entire pill.)
#if 0
        auto pillBtn = [&](const char* id, int kind /*0 play/pause, 1 stop, 2 loop*/,
                           bool toggled = false) -> bool {
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImVec2 sz(btnSize, btnSize);
            bool clicked = ImGui::InvisibleButton(id, sz);
            bool hov     = ImGui::IsItemHovered();
            ImDrawList* d = ImGui::GetWindowDrawList();
            float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
            // Permanent dark circle bg + 1px hairline so each icon reads as a
            // standalone floating button. Toggled / hover layer brighter fills
            // on top.
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f,
                               IM_COL32(20, 22, 28, 220), 28);
            ImU32 bgCol = toggled ? IM_COL32(255, 255, 255, 36)
                                  : (hov ? IM_COL32(255, 255, 255, 22)
                                         : IM_COL32(255, 255, 255, 0));
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f, bgCol, 28);
            d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f,
                         IM_COL32(255, 255, 255, 30), 28, 1.0f);
            // Play button — accent ring outside the fill, soft glow halo.
            if (kind == 0) {
                d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f - 1.0f,
                             UITokens::kAccent, 28, 1.6f);
                if (!playing) {
                    d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f + 3.0f,
                                 UITokens::kAccentGlow, 28, 2.0f);
                }
            }
            ImU32 glyphCol = IM_COL32(240, 244, 252, 240);
            float r  = sz.x * 0.30f;
            if (kind == 0) {
                if (playing) {
                    d->AddRectFilled(ImVec2(cx - r * 0.7f, cy - r),
                                     ImVec2(cx - r * 0.15f, cy + r),
                                     glyphCol, 1.5f);
                    d->AddRectFilled(ImVec2(cx + r * 0.15f, cy - r),
                                     ImVec2(cx + r * 0.7f, cy + r),
                                     glyphCol, 1.5f);
                } else {
                    d->AddTriangleFilled(
                        ImVec2(cx - r * 0.5f, cy - r),
                        ImVec2(cx - r * 0.5f, cy + r),
                        ImVec2(cx + r * 0.85f, cy), glyphCol);
                }
            } else if (kind == 1) {
                d->AddRectFilled(ImVec2(cx - r * 0.75f, cy - r * 0.75f),
                                 ImVec2(cx + r * 0.75f, cy + r * 0.75f),
                                 glyphCol, 2.0f);
            } else {
                // Loop: lemniscate (∞).
                d->AddCircle(ImVec2(cx - r * 0.5f, cy), r * 0.6f, glyphCol, 16, 1.6f);
                d->AddCircle(ImVec2(cx + r * 0.5f, cy), r * 0.6f, glyphCol, 16, 1.6f);
            }
            return clicked;
        };

        if (pillBtn("##fp_play", 0))   m_timeline.togglePlay();
        ImGui::SameLine(0, 8);
        if (pillBtn("##fp_stop", 1))   m_timeline.stop();
        ImGui::SameLine(0, 8);
        if (pillBtn("##fp_loop", 2, m_timeline.looping()))
            m_timeline.setLooping(!m_timeline.looping());

        // Timecode — inline, monospace feel.
        ImGui::SameLine(0, 14);
        double ph = m_timeline.playhead();
        double dur = m_timeline.duration();
        int pm = (int)ph / 60, ps = (int)ph % 60;
        int dm = (int)dur / 60, ds = (int)dur % 60;
        char tc[32];
        snprintf(tc, sizeof(tc), "%02d:%02d / %02d:%02d", pm, ps, dm, ds);
        ImVec2 tcPos = ImGui::GetCursorScreenPos();
        float pillCenterY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f;
        ImVec2 tcSize = ImGui::CalcTextSize(tc);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(tcPos.x, pillCenterY - tcSize.y * 0.5f),
            IM_COL32(232, 238, 250, 240), tc);
        ImGui::Dummy(ImVec2(tcSize.x + 8, btnSize));

        // Mic / voice — restored here because the docked transport row
        // (which used to host this) is hidden. Without it, voice
        // recording is unreachable from the canvas chrome.
#ifdef __APPLE__
        ImGui::SameLine(0, 12);
        {
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImVec2 sz(btnSize, btnSize);
            float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
            ImDrawList* d = ImGui::GetWindowDrawList();
            bool hov = ImGui::IsMouseHoveringRect(cur,
                ImVec2(cur.x + sz.x, cur.y + sz.y));
            // Permanent dark circle base + state-dependent overlay.
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f,
                               IM_COL32(20, 22, 28, 220), 28);
            ImU32 bg = m_voiceListening
                       ? IM_COL32(255, 70, 70, 90)
                       : (hov ? IM_COL32(255, 255, 255, 22)
                              : IM_COL32(255, 255, 255, 0));
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f, bg, 28);
            d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f,
                         IM_COL32(255, 255, 255, 30), 28, 1.0f);
            // Mic glyph — Lucide/Phosphor style. Vertical capsule for
            // the body, an open semicircle below for the cradle arms,
            // short stem + horizontal base. Drawn from primitives so it
            // crisp at button size; no bezier guesswork.
            //
            // Glyph color stays white when listening so it reads against
            // the red listening bg — red-on-red made the mic look like a
            // featureless red blob next to the REC dot.
            ImU32 col = IM_COL32(232, 238, 250, 240);
            float bodyW   = 5.0f;          // half-width of mic body
            float bodyTop = cy - 7.0f;     // top of mic capsule
            float bodyBot = cy + 2.0f;     // bottom of mic capsule
            float cradleR = 8.5f;          // radius of cradle arc
            float baseY   = cy + 11.0f;    // y of horizontal base line
            // Mic body — pill shape.
            d->AddRectFilled(ImVec2(cx - bodyW, bodyTop),
                             ImVec2(cx + bodyW, bodyBot),
                             col, bodyW);
            // Cradle — bottom-half arc, open at top.
            d->PathArcTo(ImVec2(cx, cy), cradleR,
                         0.10f * 3.14159f, 0.90f * 3.14159f, 16);
            d->PathStroke(col, 0, 1.6f);
            // Stem and base.
            d->AddLine(ImVec2(cx, cy + cradleR),
                       ImVec2(cx, baseY), col, 1.6f);
            d->AddLine(ImVec2(cx - 4.0f, baseY),
                       ImVec2(cx + 4.0f, baseY), col, 1.6f);
            // Pulse ring while listening.
            if (m_voiceListening) {
                float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f);
                d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f - 1.0f,
                             IM_COL32(255, 70, 70, (int)(60 + pulse * 140)),
                             28, 1.6f);
            }
            if (ImGui::InvisibleButton("##fp_mic", sz)) {
                ImGui::OpenPopup("##VoicePopup");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(m_voiceListening
                    ? "Listening — click for voice settings"
                    : "Voice settings (transcript / mic / device / decay)");
            }

            // Voice settings popup — replaces the old Sources › Voice tab.
            // Hosts the live transcript, mic on/off, audio device picker
            // and decay slider in a compact pop-over anchored above the
            // mic button.
            ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
            if (ImGui::BeginPopup("##VoicePopup")) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));

                // Live transcript display
                std::string words = m_dataBus.get("cue.latest");
                if (words.empty()) words = m_dataBus.get("etherea.latest");
                ImVec2 fp = ImGui::GetCursorScreenPos();
                float fw = ImGui::GetContentRegionAvail().x;
                float fh = 56.0f;
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                tdl->AddRectFilled(fp, ImVec2(fp.x + fw, fp.y + fh),
                                   IM_COL32(20, 22, 28, 220), 8.0f);
                tdl->AddRect(fp, ImVec2(fp.x + fw, fp.y + fh),
                             IM_COL32(255, 255, 255, 28), 8.0f, 0, 1.0f);
                const char* placeholder = "START TALKING..";
                const char* shown = words.empty() ? placeholder : words.c_str();
                ImU32 textCol = words.empty() ? IM_COL32(110, 118, 130, 220)
                                              : IM_COL32(232, 238, 250, 240);
                ImVec2 tts = ImGui::CalcTextSize(shown);
                tdl->AddText(ImVec2(fp.x + 14.0f, fp.y + (fh - tts.y) * 0.5f),
                             textCol, shown);
                ImGui::Dummy(ImVec2(0, fh + 10.0f));

#ifdef __APPLE__
                // Mic level meter
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
                    ImGui::Text("MIC LEVEL");
                    ImGui::PopStyleColor();
                    ImVec2 mp = ImGui::GetCursorScreenPos();
                    float mw = ImGui::GetContentRegionAvail().x;
                    float mh = 6.0f;
                    tdl->AddRectFilled(mp, ImVec2(mp.x + mw, mp.y + mh),
                                       IM_COL32(255, 255, 255, 22), mh * 0.5f);
                    float lvl = std::min(1.0f, m_audioRMS * 4.0f);
                    tdl->AddRectFilled(mp, ImVec2(mp.x + mw * lvl, mp.y + mh),
                                       IM_COL32(255, 90, 110, 240), mh * 0.5f);
                    ImGui::Dummy(ImVec2(0, mh + 10.0f));
                }

                // Mic toggle (continuous listening)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
                    ImGui::Text("MIC");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    float w = ImGui::GetContentRegionAvail().x;
                    float switchW = 44.0f, switchH = 22.0f;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - switchW));
                    ImVec2 sp = ImGui::GetCursorScreenPos();
                    bool clicked = ImGui::InvisibleButton("##miconoff_pop", ImVec2(switchW, switchH));
                    ImU32 trackCol = m_voiceContinuous
                        ? IM_COL32(255, 90, 110, 220)
                        : IM_COL32(255, 255, 255, 28);
                    tdl->AddRectFilled(sp, ImVec2(sp.x + switchW, sp.y + switchH),
                                       trackCol, switchH * 0.5f);
                    float knobR = switchH * 0.5f - 3.0f;
                    float knobX = m_voiceContinuous ? sp.x + switchW - knobR - 3.0f : sp.x + knobR + 3.0f;
                    tdl->AddCircleFilled(ImVec2(knobX, sp.y + switchH * 0.5f), knobR,
                                         IM_COL32(240, 244, 250, 255));
                    if (clicked) {
                        m_voiceContinuous = !m_voiceContinuous;
                        if (m_voiceContinuous) {
                            if (!m_voiceListening) startVoiceRecording();
                        } else {
                            if (m_voiceListening) stopVoiceRecording();
                            m_voiceRestartPending = false;
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 14));
                }
#endif

#ifdef HAS_FFMPEG
                // Audio device picker
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
                    ImGui::Text("AUDIO DEVICE");
                    ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const char* preview = "Default";
                    if (m_selectedAudioDevice >= 0 &&
                        m_selectedAudioDevice < (int)m_audioDevices.size()) {
                        preview = m_audioDevices[m_selectedAudioDevice].name.c_str();
                    }
                    if (ImGui::BeginCombo("##voice_audio_pop", preview)) {
                        if (ImGui::Selectable("Default", m_selectedAudioDevice == -1)) {
                            m_selectedAudioDevice = -1;
                            m_recorder.setAudioDevice(-1);
                        }
                        for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                            if (!m_audioDevices[i].isCapture) continue;
                            if (ImGui::Selectable(m_audioDevices[i].name.c_str(),
                                                  m_selectedAudioDevice == i)) {
                                m_selectedAudioDevice = i;
                                m_recorder.setAudioDevice(i);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::Dummy(ImVec2(0, 10));
                }
#endif

                // Decay slider
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
                    ImGui::Text("DECAY");
                    ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##voice_decay_pop", &m_voiceDecayDuration,
                                       0.5f, 10.0f, "%.1fs");
                }

                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }
#endif

        // ── REC / STREAM / SYSTEM-AUDIO icons ─────────────────────────────
        // These replace the bottom-right floating "REC" and "Go Live" text
        // pills. Same visual rhythm as play/stop/loop above so the whole row
        // reads as one compact transport.
#ifdef HAS_FFMPEG
        bool recActive  = m_recorder.isActive();
        bool liveActive = m_rtmpOutput.isActive();

        auto iconBtn = [&](const char* id, bool active, ImU32 activeBg,
                           std::function<void(ImDrawList*, float cx, float cy, float r, ImU32 col)> drawGlyph)
                       -> bool {
            ImGui::SameLine(0, 8);
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImVec2 sz(btnSize, btnSize);
            bool clicked = ImGui::InvisibleButton(id, sz);
            bool hov     = ImGui::IsItemHovered();
            ImDrawList* d = ImGui::GetWindowDrawList();
            float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
            // Permanent dark circle bg + 1px hairline border so REC / STREAM /
            // AUDIO read as floating buttons. State overlay (active/hover)
            // sits on top.
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f,
                               IM_COL32(20, 22, 28, 220), 28);
            ImU32 bg = active ? activeBg
                              : (hov ? IM_COL32(255, 255, 255, 22)
                                     : IM_COL32(255, 255, 255, 0));
            d->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f, bg, 28);
            d->AddCircle(ImVec2(cx, cy), sz.x * 0.5f,
                         IM_COL32(255, 255, 255, 30), 28, 1.0f);
            ImU32 col = active ? IM_COL32(255, 90, 90, 240)
                               : IM_COL32(232, 238, 250, 240);
            drawGlyph(d, cx, cy, sz.x * 0.30f, col);
            return clicked;
        };

        // REC — classic "record" mark: a thin red ring with a small
        // filled red dot in the centre. While recording, the dot grows
        // and pulses so the active state reads at a glance, and so it
        // never collides visually with the listening-mic icon (which
        // shows a white mic glyph on a red bg).
        if (iconBtn("##fp_rec", recActive, IM_COL32(255, 70, 70, 90),
                    [&](ImDrawList* d, float cx, float cy, float r, ImU32 /*c*/) {
                        ImU32 ringCol = recActive ? IM_COL32(255, 90, 90, 245)
                                                  : IM_COL32(255, 80, 80, 200);
                        d->AddCircle(ImVec2(cx, cy), r * 0.95f, ringCol, 28, 1.6f);
                        float dotR = recActive
                            ? r * (0.45f + 0.08f * sinf((float)ImGui::GetTime() * 4.0f))
                            : r * 0.35f;
                        d->AddCircleFilled(ImVec2(cx, cy), dotR, ringCol, 20);
                    })) {
            if (m_recorder.isActive()) {
                m_recorder.stop();
                m_timelineExporting = false;
            } else {
                m_recorder.setAudioDevice(m_selectedAudioDevice);
                startRecording();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(recActive ? "Recording — click to stop" : "Record (work area)");

        // STREAM — three broadcast arcs from a base dot.
        if (iconBtn("##fp_stream", liveActive, IM_COL32(74, 230, 144, 60),
                    [&](ImDrawList* d, float cx, float cy, float r, ImU32 col) {
                        ImU32 useCol = liveActive ? IM_COL32(74, 230, 144, 245) : col;
                        // Base dot at bottom-left.
                        float bx = cx - r * 0.4f, by = cy + r * 0.6f;
                        d->AddCircleFilled(ImVec2(bx, by), 2.0f, useCol, 12);
                        // Three quarter-arcs radiating up-right.
                        for (int i = 1; i <= 3; i++) {
                            float ar = r * 0.45f * (float)i;
                            d->PathArcTo(ImVec2(bx, by), ar,
                                         -1.55f, 0.0f, 14);
                            d->PathStroke(useCol, 0, 1.6f);
                        }
                    })) {
            if (m_rtmpOutput.isActive()) m_rtmpOutput.stop();
            else                         ImGui::OpenPopup("##fp_stream_popup");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(liveActive ? "Streaming — click to stop" : "Stream to RTMP");
        if (ImGui::BeginPopup("##fp_stream_popup")) {
            ImGui::TextDisabled("Stream to RTMP");
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("YouTube Stream Key");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(260);
            ImGui::InputText("##fp_streamkey", m_streamKeyBuf, sizeof(m_streamKeyBuf),
                             ImGuiInputTextFlags_Password);
            static const char* aspectNames[] = { "16:9", "4:3", "16:10", "Source" };
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.59f, 0.62f, 0.68f, 0.90f));
            ImGui::Text("Aspect");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(260);
            ImGui::Combo("##fp_aspect", &m_streamAspect, aspectNames, 4);
            ImGui::Separator();
            bool hasKey = m_streamKeyBuf[0] != '\0';
            ImGui::BeginDisabled(!hasKey);
            if (ImGui::Button("Start streaming", ImVec2(-1, 0))) {
                static const int aspectNums[] = { 16, 4, 16, 0 };
                static const int aspectDens[] = { 9,  3, 10, 0 };
                auto& z = activeZone();
                m_rtmpOutput.start(m_streamKeyBuf, z.warpFBO.width(), z.warpFBO.height(),
                                   aspectNums[m_streamAspect], aspectDens[m_streamAspect], 30);
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

        // SYSTEM AUDIO — speaker glyph + arc waves; click opens device picker.
        if (iconBtn("##fp_audio", false, IM_COL32(255, 255, 255, 0),
                    [&](ImDrawList* d, float cx, float cy, float r, ImU32 col) {
                        // Speaker body: trapezoid (rect + triangle).
                        float bx = cx - r * 0.3f;
                        d->AddRectFilled(ImVec2(bx - r * 0.5f, cy - r * 0.45f),
                                         ImVec2(bx,           cy + r * 0.45f), col);
                        d->AddTriangleFilled(
                            ImVec2(bx,           cy - r * 0.45f),
                            ImVec2(bx,           cy + r * 0.45f),
                            ImVec2(bx + r * 0.7f, cy + r * 0.85f), col);
                        d->AddTriangleFilled(
                            ImVec2(bx,           cy - r * 0.45f),
                            ImVec2(bx + r * 0.7f, cy - r * 0.85f),
                            ImVec2(bx,           cy + r * 0.45f), col);
                        // Waves — two outward arcs.
                        float wx = cx + r * 0.4f;
                        d->PathArcTo(ImVec2(wx, cy), r * 0.45f, -1.0f, 1.0f, 12);
                        d->PathStroke(col, 0, 1.4f);
                        d->PathArcTo(ImVec2(wx, cy), r * 0.75f, -1.0f, 1.0f, 14);
                        d->PathStroke(col, 0, 1.4f);
                    })) {
            // Refresh device list before opening so it's current.
            m_audioDevices = VideoRecorder::enumerateAudioDevices();
            ImGui::OpenPopup("##fp_audio_popup");
        }
        {
            const char* preview = "System default";
            if (m_selectedAudioDevice >= 0 &&
                m_selectedAudioDevice < (int)m_audioDevices.size()) {
                preview = m_audioDevices[m_selectedAudioDevice].name.c_str();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("System audio: %s\n(click to choose capture device)", preview);
        }
        if (ImGui::BeginPopup("##fp_audio_popup")) {
            ImGui::TextDisabled("System audio capture");
            ImGui::Separator();
            if (ImGui::Selectable("System default",
                                  m_selectedAudioDevice == -1)) {
                m_selectedAudioDevice = -1;
                m_recorder.setAudioDevice(-1);
            }
            for (int i = 0; i < (int)m_audioDevices.size(); i++) {
                const auto& dv = m_audioDevices[i];
                if (!dv.isCapture) continue;
                if (ImGui::Selectable(dv.name.c_str(),
                                      m_selectedAudioDevice == i)) {
                    m_selectedAudioDevice = i;
                    m_recorder.setAudioDevice(i);
                }
            }
            ImGui::EndPopup();
        }
#endif
#endif // #if 0 — legacy transport blocks disabled
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    // Keep the bottom-nav pill ABOVE the Timeline without re-introducing
    // NoBringToFrontOnFocus on the Timeline (that flag pushed the whole
    // Timeline window — opaque background and all — BEHIND the docked
    // Canvas/shader, which is the see-through bug). BringWindowToDisplayFront
    // is a pure z-order reorder (moves the pill to the end of g.Windows so it
    // paints last/on-top); unlike SetWindowFocus it does NOT steal keyboard/
    // mouse focus, so clicking timeline widgets still works and never hides
    // the nav. Re-asserted every frame so a click that raises the Timeline
    // can't get stuck above the pill.
    if (ImGuiWindow* pw = ImGui::FindWindowByName("##TransportPill"))
        ImGui::BringWindowToDisplayFront(pw);

    // The pill's own dropdowns are top-level popup windows, not children of
    // ##TransportPill, so the reorder above can paint the pill OVER them.
    // Re-raise the open sound-source popup last so its (possibly long,
    // scrollable) device list sits on top of the pill instead of behind it.
    if (ImGuiWindow* sp = ImGui::FindWindowByName("##fp_sound_popup"))
        if (sp->Active) ImGui::BringWindowToDisplayFront(sp);
}

void Application::renderFloatingActionPills() {
    // Replaced by the REC / STREAM icon buttons in renderFloatingTransportPill.
    // Kept as an empty stub so the existing call sites stay compile-clean and
    // the feature is one diff away from being restored if needed.
    return;
    // Two separate rounded pills bottom-right — REC and LIVE — matching
    // the reference's split record/broadcast affordances. Each is its own
    // ImGui::Begin window so they animate, hover, and reposition cleanly
    // without sharing layout with the docked timeline.
    if (UIManager::sMode != UIManager::WorkspaceMode::Canvas) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();

    float timelineH = 0.0f;
    if (ImGuiWindow* w = ImGui::FindWindowByName("Timeline"))
        timelineH = w->Size.y;

    const float pillH = 38.0f;
    const float yMargin = 14.0f;
    const float rightMargin = 70.0f;  // clear of the right tool rail
    const float gap = 8.0f;
    float baseY = vp->WorkPos.y + vp->WorkSize.y - timelineH - pillH - yMargin;

    auto pill = [&](const char* id, const char* label, ImU32 dotCol,
                    float pillW, float xRight, bool active,
                    std::function<void()> onClick) {
        float x = xRight - pillW;
        ImGui::SetNextWindowPos (ImVec2(x, baseY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(pillW, pillH), ImGuiCond_Always);
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse|
            ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_NoScrollbar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   pillH * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 22, 28, 235));
        ImGui::PushStyleColor(ImGuiCol_Border,
            active ? dotCol : IM_COL32(255, 255, 255, 22));
        if (ImGui::Begin(id, nullptr, flags)) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            // Drop shadow.
            ImGui::GetBackgroundDrawList()->AddRectFilled(
                ImVec2(wp.x + 3, wp.y + 5),
                ImVec2(wp.x + ws.x + 3, wp.y + ws.y + 5),
                IM_COL32(0, 0, 0, 70), pillH * 0.5f);
            // Click hit area covers the entire pill.
            ImGui::SetCursorPos(ImVec2(0, 0));
            if (ImGui::InvisibleButton("##hit", ws) && onClick) onClick();
            // Status dot + label, centred.
            ImDrawList* d = ImGui::GetWindowDrawList();
            float cy = wp.y + ws.y * 0.5f;
            float dotX = wp.x + 14.0f;
            d->AddCircleFilled(ImVec2(dotX, cy), 4.5f, dotCol, 16);
            ImVec2 ts = ImGui::CalcTextSize(label);
            d->AddText(ImVec2(dotX + 12.0f, cy - ts.y * 0.5f),
                       active ? dotCol : IM_COL32(232, 238, 250, 240),
                       label);
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    };

#ifdef HAS_FFMPEG
    bool recording = m_recorder.isActive();
    bool living = m_rtmpOutput.isActive();
#else
    bool recording = false;
    bool living = false;
#endif
    float xRight = vp->WorkPos.x + vp->WorkSize.x - rightMargin;
    // LIVE pill sits at the right edge; REC sits to its left.
    pill("##LivePill", living ? "LIVE" : "Go Live",
         living ? IM_COL32(74, 230, 144, 255) : UITokens::kAccentSoft,
         92.0f, xRight, living,
         [this]() {
#ifdef HAS_FFMPEG
            if (m_rtmpOutput.isActive()) m_rtmpOutput.stop();
            else ImGui::OpenPopup("##GoLivePopup");
#endif
         });
    xRight -= 92.0f + gap;
    pill("##RecPill", recording ? "Recording" : "REC",
         IM_COL32(255, 70, 70, 255),
         84.0f, xRight, recording,
         [this]() {
#ifdef HAS_FFMPEG
            if (m_recorder.isActive()) {
                m_recorder.stop();
                m_timelineExporting = false;
            } else {
                m_recorder.setAudioDevice(m_selectedAudioDevice);
                startRecording();
            }
#endif
         });
}

// ── Lane UI gate ─────────────────────────────────────────────────────────
// The timeline "lane" affordances (+ Lane button, the Automation/MIDI/
// Audio-Reactive sub-rows and their gizmos) are a confusing half-built stub
// and the legacy render loop was memory-unsafe (held a `TimelineLane&` into
// m_timeline.lanes() across calls that could reallocate that vector). We hide
// the entire user-facing lane UI but KEEP the data model, serialization and
// the working Automation→opacity runtime (Timeline::sampleAnimatedParams,
// applyToLayers) fully intact: existing .easel projects with lanes still load,
// round-trip and still drive opacity at runtime. Flip this to re-enable the UI
// once the lane editor is finished and the render loop is made realloc-safe.
static constexpr bool kLanesUiEnabled = false;

void Application::renderTimelinePanel() {
    // Fix 2: the timeline is a FLOATING overlay (not docked) so it can slide
    // smoothly. Its rect is fully derived from the single source of truth:
    //   top    = m_timelineTopY  (also the bottom-nav's top edge)
    //   bottom = viewport bottom − bottom-nav height
    //   width  = full viewport width (responsive — no left/right clipping)
    // The bottom nav (renderFloatingTransportPill) sits directly below, so
    // the two read as one rigid unit popping out from under the nav.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float kPillH = 56.0f;   // must match renderFloatingTransportPill
    // SINGLE SOURCE OF TRUTH = m_timelineCurH (animated current height in px,
    // 0 = closed, grows to full height when open). The timeline window is the
    // full viewport width, m_timelineCurH tall, pinned to the BOTTOM of the
    // viewport. It is deliberately NOT positioned from m_timelineTopY (that
    // member is the bottom-nav pill's top edge and would place the timeline
    // 56px too high — behind the pill, which is exactly the invisible-timeline
    // regression). The pill sits directly on top at
    // (vp_bottom - pillH - m_timelineCurH); pill bottom edge == timeline top
    // edge, so the two ride together as m_timelineCurH animates.
    float tlX = vp->Pos.x;
    // Stop the timeline at the left edge of the right sidebar so it never
    // overlaps the floating props/mapping panel.
    float tlW;
    {
        float rpLeft = m_ui.getRightPanelLeft();
        if (rpLeft > vp->Pos.x && rpLeft < vp->Pos.x + vp->Size.x)
            tlW = rpLeft - vp->Pos.x;
        else
            tlW = vp->Size.x;
    }
    float tlH = m_timelineCurH;
    if (tlH < 1.0f) tlH = 1.0f;
    float tlY = vp->Pos.y + vp->Size.y - tlH;   // pinned to viewport bottom

    // Truly fully closed: minimised AND the open animation has fully drained.
    // Only then do we suppress the panel background (so a 1px residual rect
    // can't tint pixels). While OPEN or ANIMATING (either direction) the
    // timeline IS submitted, HAS its background, and is the correct height.
    const bool tlEffectivelyClosed =
        (m_timelineMinimized && m_timelineAnimT <= 0.001f);

    ImGui::SetNextWindowPos (ImVec2(tlX, tlY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(tlW, tlH), ImGuiCond_Always);
    // Fix 3: allow the window to be smaller than the global 32px min so the
    // closing slide can shrink all the way to 0 without a min-size panel
    // covering the pill. (max = +FLT_MAX → unconstrained on the high side.)
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));

    // Tight edge margins — left 12px matches the Canvas/Stage nav indent,
    // vertical 16px keeps the transport row centred inside the minimised
    // height (FrameHeight + 32).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // Z-ORDER (the real fix for the see-through timeline):
    // Everything in editor mode is an ImGui window. The shader/canvas is the
    // docked "Canvas" window, which lives inside the "DockSpace" host window.
    // That host carries ImGuiWindowFlags_NoBringToFrontOnFocus, so ImGui
    // created it via g.Windows.push_front() (imgui.cpp CreateNewWindow) —
    // i.e. at the BACK of the z-stack... but still a real window that paints
    // the shader image. If the Timeline ALSO has NoBringToFrontOnFocus it is
    // likewise push_front()'d; whichever of {DockSpace, Timeline} was created
    // first ends up BEHIND the other. DockSpace is created on frame 1 (top of
    // renderUI) and the Timeline only when first opened, so push_front puts
    // the Timeline at index 0 — BEHIND the Canvas/shader. Its opaque
    // AddRectFilled is then composited under the shader Image() and the
    // canvas shows straight through. Dropping NoBringToFrontOnFocus makes the
    // Timeline a normal push_back() window, so it is created/drawn IN FRONT
    // of the DockSpace+Canvas and the opaque fill is finally visible. The
    // pill is kept above the timeline explicitly via BringWindowToDisplayFront
    // every frame (see renderFloatingTransportPill) — a z-only reorder that
    // does NOT steal focus, so clicking timeline widgets no longer hides the
    // bottom nav (the original reason NoBringToFrontOnFocus was added).
    ImGuiWindowFlags tlFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove      |
        ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoSavedSettings;
    // When the timeline is effectively closed, paint NOTHING for its panel so
    // a residual rect (or the very last animation frame) can never tint the
    // pixels the pill occupies.
    if (tlEffectivelyClosed)
        tlFlags |= ImGuiWindowFlags_NoBackground;
    ImGui::Begin("Timeline", nullptr, tlFlags);
    ImGui::PopStyleVar(2);

    // Clip everything to the visible (animated) window so the content is
    // revealed as it slides instead of spilling over the canvas/nav.
    ImGui::PushClipRect(ImVec2(tlX, tlY), ImVec2(tlX + tlW, tlY + tlH), true);

    // GUARANTEED opaque panel fill. The timeline is a FLOATING window whose
    // body content (ruler/tracks) is painted with near-zero-alpha fills that
    // rely on an opaque surface behind them. ImGui's own WindowBg only covers
    // the (animated, often shorter-than-content) window rect, so during the
    // slide — and whenever clamped content overflows the window — the track
    // area had nothing behind it and the shader/canvas showed straight
    // through. Draw the panel background ourselves over the FULL clipped
    // timeline rect, BEFORE any content, in the app's canonical panel color
    // (UIManager bgPanel = ImVec4(0.020,0.022,0.026,1) → IM_COL32(5,6,7,255)).
    // Alpha tracks the slide (m_timelineAnimT) so it fades with the animation
    // but is fully opaque whenever the timeline is open; only the truly-closed
    // state (tlEffectivelyClosed) skips it so the pill's pixels stay clean.
    if (!tlEffectivelyClosed) {
        float bgA = m_timelineAnimT;
        if (bgA > 1.0f) bgA = 1.0f;
        if (bgA < 0.0f) bgA = 0.0f;
        ImU32 panelBg = IM_COL32(5, 6, 7, (int)(255.0f * bgA));
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(tlX, tlY), ImVec2(tlX + tlW, tlY + tlH), panelBg);
    }

    // 1px hairline along the timeline window's top edge — explicit because
    // WindowBorderSize is 0 globally. This is the only outline the bottom
    // nav should have (matches the main-nav underline at the top).
    {
        ImVec2 wpos  = ImGui::GetWindowPos();
        float  wwid  = ImGui::GetWindowSize().x;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddLine(ImVec2(wpos.x, wpos.y),
                    ImVec2(wpos.x + wwid, wpos.y),
                    IM_COL32(255, 255, 255, 25), 1.0f);
    }

    // Drag-to-resize handle on the top edge of the timeline.
    // A 6px invisible grab strip sits over the hairline; dragging it
    // adjusts m_timelineTargetH so the user can choose their own height.
    if (!tlEffectivelyClosed) {
        static bool  s_tlResizing = false;
        static float s_tlResizeDragStartY  = 0.0f;
        static float s_tlResizeDragStartH  = 0.0f;
        const float kGrabH = 6.0f;
        ImVec2 grabMin(tlX, tlY);
        ImVec2 grabMax(tlX + tlW, tlY + kGrabH);
        ImGui::SetCursorScreenPos(grabMin);
        ImGui::InvisibleButton("##tlResize", ImVec2(tlW, kGrabH));
        bool grabHovered = ImGui::IsItemHovered();
        bool grabActive  = ImGui::IsItemActive();
        if (grabHovered || s_tlResizing)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (grabActive && ImGui::IsMouseClicked(0)) {
            s_tlResizing        = true;
            s_tlResizeDragStartY = ImGui::GetIO().MousePos.y;
            s_tlResizeDragStartH = m_timelineTargetH;
        }
        if (s_tlResizing) {
            float dy = s_tlResizeDragStartY - ImGui::GetIO().MousePos.y; // drag up = taller
            m_timelineTargetH = s_tlResizeDragStartH + dy;
            if (m_timelineTargetH < 80.0f)  m_timelineTargetH = 80.0f;
            if (m_timelineTargetH > vp->Size.y * 0.75f)
                m_timelineTargetH = vp->Size.y * 0.75f;
            if (!ImGui::IsMouseDown(0)) s_tlResizing = false;
        }
        // Subtle highlight on the grab strip when hovered/active.
        if (grabHovered || s_tlResizing) {
            ImGui::GetForegroundDrawList()->AddRectFilled(
                grabMin, ImVec2(grabMax.x, grabMin.y + 2.0f),
                IM_COL32(255, 255, 255, 45));
        }
    }

    // Measured fully-open content height — written near the function bottom,
    // consumed next frame by updateTimelineAnim() via m_timelineTargetH.
    static float s_tlMeasuredContentH = 0.0f;

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

        // (Collapse chevron removed — toggle now lives only in the bottom
        // transport bar's timeline-icon button. One source of truth for the
        // minimize state, and the timeline header reads as content-only.)

        // Fix 4: duration + Work Area controls were DUPLICATED here. They now
        // live ONLY in the bottom transport bar (##fp_dur_popup / ##fp_wa
        // → ##WAEditPopup). Removing the inline ##TCEdit / ##DurEdit / Dur
        // DragFloat / Work button kills the duplicate; the ##WAEditPopup body
        // is kept (further down) since the bottom-nav Work button opens it.
        double dur = m_timeline.duration();
        (void)dur;

        // Zoom slider — replaces the old wheel-zoom. Logarithmic feel so the
        // whole 1x→64x range fits a short slider without low-zoom being cramped.
        // (No SameLine — it's now the first item on this row.)
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
                char tipBuf[256];
                snprintf(tipBuf, sizeof(tipBuf),
                         "Zoom: %.1fx — shows %.1fs at a time\n"
                         "(double-click ruler to reset)",
                         s_tlZoom, m_timeline.duration() / s_tlZoom);
                ParamRow::Tooltip(tipBuf);
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

        // (Work Area label/editor removed from the timeline — it's a single
        // source of truth in the bottom transport bar now. See Fix 4.)

        // (Centered audio cluster removed — mic icon, System Audio combo,
        // and stereo level meter all live in the bottom transport bar now.
        // Keeping a second copy here was the duplicate the user flagged.)
        ImGui::SameLine(0, 0);
        float contentMaxX = ImGui::GetWindowContentRegionMax().x;

        // Right-anchored: REC only. (Work Area moved next to Dur on the
        // left of this transport row so duration + work-area read as one
        // grouped time control.)
        ImGui::SameLine(0, 0);
        float rightAnchorX = contentMaxX - kRecBtnW - 4.0f;
        if (rightAnchorX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightAnchorX);
        {
            // Fix 4: Work Area editor body MOVED to renderFloatingTransportPill
            // (same window as its OpenPopup call) so it works even when the
            // timeline is closed. Nothing to render here anymore.
        }

        // (REC + GO LIVE removed from the timeline transport — both live in
        // the bottom transport bar's right cluster. Single source of truth.)
        (void)recording;
        (void)exporting;
        (void)timeNow;
        (void)kRecBtnW;
        (void)kRed;
        (void)kRedDim;
        (void)pillBtn;
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

    // Fix 2: render the FULL content (ruler/tracks/audio) whenever the window
    // is on screen — even while sliding closed — so the panel visibly slides
    // DOWN behind the clip rect instead of snapping to a transport-only strip.
    // The window only renders at all while m_timelineAnimT > 0 (see caller),
    // so a fully-minimized timeline costs nothing.

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
                    IM_COL32(255, 255, 255, 90));
        char lbl[16];
        if (majorInterval < 1.0) snprintf(lbl, sizeof(lbl), "%d:%05.2f", (int)t / 60, std::fmod(t, 60.0));
        else                      snprintf(lbl, sizeof(lbl), "%d:%02d", (int)t / 60, ((int)t) % 60);
        // Nudge the very first label inward by 4px so 0:00 doesn't kiss the
        // ruler's left edge, and clamp it inside the visible band so it
        // never overlaps the gutter.
        float lx = x + 6.0f;
        if (lx < rulerOrigin.x + 6.0f) lx = rulerOrigin.x + 6.0f;
        dl->AddText(ImGui::GetFont(), 10.0f,
                    ImVec2(lx, rulerOrigin.y + 4),
                    IM_COL32(225, 230, 240, 170), lbl);
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
                char tipBuf[256];
                snprintf(tipBuf, sizeof(tipBuf),
                         "%s  (click to jump, shift-click to delete)",
                         mk.name.c_str());
                ParamRow::Tooltip(tipBuf);
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
        // Subtle neutral band — the work-area indicator should whisper, not
        // shout. The end brackets carry the affordance; the fill is only a
        // light tone so the ruler ticks and labels still read clearly.
        ImU32 waFill = IM_COL32(255, 255, 255, 12);
        ImU32 waEdge = IM_COL32(170, 180, 210, 130);
        dl->AddRectFilled(ImVec2(waX0, rulerOrigin.y),
                          ImVec2(waX1, rulerOrigin.y + rulerH),
                          waFill, 4.0f);
        // 1px hairline brackets at each end.
        dl->AddLine(ImVec2(waX0 + 0.5f, rulerOrigin.y + 2),
                    ImVec2(waX0 + 0.5f, rulerOrigin.y + rulerH - 2),
                    waEdge, 1.0f);
        dl->AddLine(ImVec2(waX1 - 0.5f, rulerOrigin.y + 2),
                    ImVec2(waX1 - 0.5f, rulerOrigin.y + rulerH - 2),
                    waEdge, 1.0f);
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

        // Drop target: accept a shader dragged from the Media panel.
        // Dropping on empty space creates a new clip; dropping on an
        // existing clip assigns its sourcePath.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("SC_SHADER_PATH")) {
                std::string shaderPath(static_cast<const char*>(payload->Data),
                                       payload->DataSize - 1);
                float relX = ImGui::GetIO().MousePos.x - trackOrigin.x;
                double dropTime = xToTime(relX);
                if (dropTime < 0.0) dropTime = 0.0;
                TimelineClip* hit = nullptr;
                for (auto& c : track.clips) {
                    if (dropTime >= c.startTime && dropTime < c.endTime()) {
                        hit = m_timeline.findClip(track.layerId, c.id);
                        break;
                    }
                }
                auto baseName = [](const std::string& p) -> std::string {
                    auto sl = p.find_last_of("/\\");
                    return (sl == std::string::npos) ? p : p.substr(sl + 1);
                };
                m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                if (hit) {
                    hit->sourcePath = shaderPath;
                    hit->name       = baseName(shaderPath);
                    hit->kind       = ClipKind::Shader;
                } else {
                    auto* nc = m_timeline.addClip(track.layerId, dropTime, 5.0,
                                                  baseName(shaderPath), shaderPath);
                    if (nc) nc->kind = ClipKind::Shader;
                }
            }
            ImGui::EndDragDropTarget();
        }

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

            // Trim handles: always visible (faint), brighten on hover.
            // Yellow indicators at each clip end signal drag-to-trim affordance.
            {
                bool onLeft  = hover && dragClipId == 0 && (mx < x0 + trimZone);
                bool onRight = hover && dragClipId == 0 && (mx > x1 - trimZone);
                ImU32 colL = onLeft  ? IM_COL32(255, 220, 80, 220) : IM_COL32(255, 220, 80, 70);
                ImU32 colR = onRight ? IM_COL32(255, 220, 80, 220) : IM_COL32(255, 220, 80, 70);
                dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x0 + 3, b.y), colL, 2.0f);
                dl->AddRectFilled(ImVec2(x1 - 3, a.y), ImVec2(x1, b.y), colR, 2.0f);
                if (hover && dragClipId == 0 && (onLeft || onRight))
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
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

            // Right-click on a clip → source-assignment context menu.
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)
                && mx >= x0 && mx <= x1 && my >= a.y && my <= b.y) {
                s_ctxLayerId = track.layerId;
                s_ctxClipId  = clip.id;
                ImGui::OpenPopup("##ClipSrcMenu");
            }
            // Popup renders within the same PushID scope as OpenPopup.
            if (ImGui::BeginPopup("##ClipSrcMenu")) {
                if (auto* cc = m_timeline.findClip(s_ctxLayerId, s_ctxClipId)) {
                    if (!cc->sourcePath.empty()) {
                        auto sslash = cc->sourcePath.find_last_of("/\\");
                        std::string bn = (sslash == std::string::npos)
                                         ? cc->sourcePath
                                         : cc->sourcePath.substr(sslash + 1);
                        ImGui::TextDisabled("%s", bn.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Clear Source")) {
                            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                            cc->sourcePath.clear();
                        }
                        ImGui::Separator();
                    } else {
                        ImGui::TextDisabled("(no source)");
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem("Set Source from File...")) {
                        std::string p = openFileDialog(
                            "Shaders & Images\0*.fs;*.frag;*.isf;*.png;*.jpg;*.mp4;*.mov\0All\0*.*\0");
                        if (!p.empty()) {
                            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                            cc->sourcePath = p;
                            auto sl2 = p.find_last_of("/\\");
                            cc->name = (sl2 == std::string::npos) ? p : p.substr(sl2 + 1);
                            cc->kind = ClipKind::Shader;
                        }
                    }
                    if (ImGui::MenuItem("Add Shader Clip After")) {
                        // Place the new clip immediately after this one.
                        double newStart = cc->startTime + cc->duration;
                        double newDur   = 5.0;
                        // Clamp to timeline end.
                        if (newStart >= m_timeline.duration())
                            newStart = m_timeline.duration() - 0.1;
                        if (newStart + newDur > m_timeline.duration())
                            newDur = m_timeline.duration() - newStart;
                        if (newDur < 0.1) newDur = 0.1;
                        m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                        auto* nc = m_timeline.addClip(s_ctxLayerId, newStart, newDur, "New Clip");
                        if (nc) { s_ctxClipId = nc->id; }
                    }
                    if (ImGui::MenuItem("Add Shader Clip from File...")) {
                        std::string p = openFileDialog(
                            "Shaders & Images\0*.fs;*.frag;*.isf;*.png;*.jpg;*.mp4;*.mov\0All\0*.*\0");
                        if (!p.empty()) {
                            double newStart = cc->startTime + cc->duration;
                            double newDur   = 5.0;
                            if (newStart >= m_timeline.duration())
                                newStart = m_timeline.duration() - 0.1;
                            if (newStart + newDur > m_timeline.duration())
                                newDur = m_timeline.duration() - newStart;
                            if (newDur < 0.1) newDur = 0.1;
                            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                            auto sl2 = p.find_last_of("/\\");
                            std::string bn = (sl2 == std::string::npos) ? p : p.substr(sl2 + 1);
                            auto* nc = m_timeline.addClip(s_ctxLayerId, newStart, newDur, bn);
                            if (nc) {
                                nc->sourcePath = p;
                                nc->kind = ClipKind::Shader;
                                s_ctxClipId = nc->id;
                            }
                        }
                    }
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                    if (ImGui::MenuItem("Delete Clip")) {
                        m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                        m_timeline.removeClip(s_ctxLayerId, s_ctxClipId);
                        s_ctxClipId = 0;
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::EndPopup();
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
            if (ImGui::IsItemHovered()) ParamRow::Tooltip(
                visible ? "Layer visible — click to hide"
                        : "Layer hidden — click to show");

            // Kind colour — by ClipKind. Used as the thumbnail's identity ring
            // (and as the full swatch fill when the layer has no texture yet).
            ImU32 swatch = (swatchKind == ClipKind::Video)  ? IM_COL32(130, 156, 255, 220)
                         : (swatchKind == ClipKind::Shader) ? IM_COL32(190, 130, 250, 220)
                                                             : IM_COL32(180, 188, 198, 220);
            // Layer thumbnail — reuse Layer::textureId() (the same raw source
            // texture the left-rail thumbnails draw, see renderLeftRail) so it
            // stays consistent and free. Same 10x10 footprint as the old
            // swatch; kind colour becomes a thin identity ring. Falls back to
            // the solid swatch when the layer has no renderable texture.
            ImVec2 swA(rowOrigin.x + 28, rowY + trackH * 0.5f - 5);
            ImVec2 swB(rowOrigin.x + 38, rowY + trackH * 0.5f + 5);
            GLuint rowTex = layerForRow ? layerForRow->textureId() : 0;
            if (rowTex) {
                dl->AddRectFilled(swA, swB, IM_COL32(28, 32, 40, 255), 2.5f);
                dl->PushClipRect(swA, swB, true);
                dl->AddImageRounded((ImTextureID)(intptr_t)rowTex, swA, swB,
                                    ImVec2(0, 1), ImVec2(1, 0),  // V-flip GL
                                    IM_COL32(255, 255, 255, 255), 2.5f);
                dl->PopClipRect();
                dl->AddRect(swA, swB, swatch, 2.5f, 0, 1.0f);
            } else {
                dl->AddRectFilled(swA, swB, swatch, 2.5f);
            }

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
            if (ImGui::IsItemHovered()) ParamRow::Tooltip(
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
        //
        // Hidden behind kLanesUiEnabled: the data model still exists and still
        // serialises/round-trips; only this UI is suppressed. When suppressed
        // we emit NOTHING (no Dummy/separator) so track rows lay out with the
        // exact same rhythm as if lanes never existed visually.
        //
        // Crash fix: this loop iterates m_timeline.lanes() by reference
        // (`auto& ln`) while its body can trigger m_lanes growth (a brand-new
        // lane added the same frame via the opacity-keyframe runtime), which
        // reallocates the vector and leaves `ln` — and the remaining loop
        // iterations — dangling (heap-use-after-free reading ln.layerId /
        // ln.kind / ln.points). We snapshot the matching lane ids up front and
        // resolve each via findLane() per iteration so no reference is ever
        // held across a potential reallocation.
        if (kLanesUiEnabled) {
            uint32_t toRemoveLane = 0;
            std::vector<uint32_t> laneIds;
            for (const auto& l0 : m_timeline.lanes())
                if (l0.layerId == track.layerId) laneIds.push_back(l0.id);
            for (uint32_t lnId : laneIds) {
                TimelineLane* lnp = m_timeline.findLane(lnId);
                if (!lnp) continue;          // removed mid-frame — skip safely
                TimelineLane& ln = *lnp;
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
        // Hidden behind kLanesUiEnabled (see note above). When disabled we emit
        // nothing — no Dummy/button — so the next track follows immediately
        // with the normal trackSpacing rhythm and no layout gap is introduced.
        if (kLanesUiEnabled) {
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

            // Scope all per-pair widgets (the "##addtr" "+" button, and any
            // future per-transition widgets) under a unique ID for this
            // (aId, bId) pair. Without this, every visible transition lane
            // emits "##addtr" at the same ID and ImGui flags a collision.
            // Hash mixes both ids so it stays unique even if layer ids reuse.
            ImGui::PushID((int)((aId * 2654435761u) ^ (bId + 0x9E3779B9u)));

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

            ImGui::PopID(); // per-pair scope opened at the top of `if (hasNext)`
        }
    }

    // ── Empty-area shader drop zone ───────────────────────────────────────
    // Accepting SC_SHADER_PATH drops onto the blank space below all tracks
    // creates a new layer (via loadShader) + a clip placed at the drop time.
    {
        float dropZoneH = 36.0f;
        ImGui::SetCursorScreenPos(ImVec2(rulerOrigin.x, ImGui::GetCursorScreenPos().y));
        ImGui::InvisibleButton("##tlDropZone", ImVec2(trackAreaW, dropZoneH));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("SC_SHADER_PATH")) {
                std::string shaderPath(static_cast<const char*>(payload->Data),
                                       payload->DataSize - 1);
                auto baseName2 = [](const std::string& p) -> std::string {
                    auto sl = p.find_last_of("/\\");
                    return (sl == std::string::npos) ? p : p.substr(sl + 1);
                };
                float relX = ImGui::GetIO().MousePos.x - rulerOrigin.x;
                double dropTime = xToTime(relX);
                if (dropTime < 0.0) dropTime = 0.0;
                // Create a new layer for this shader then add a clip on it.
                loadShader(shaderPath);
                int newLayerIdx = m_selectedLayer;
                if (newLayerIdx >= 0 && newLayerIdx < m_layerStack.count()) {
                    auto layer = m_layerStack[newLayerIdx];
                    if (layer) {
                        double dur = std::min(5.0, m_timeline.duration() - dropTime);
                        if (dur < 0.1) dur = 0.1;
                        m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                        auto* nc = m_timeline.addClip(layer->id, dropTime, dur,
                                                       baseName2(shaderPath), shaderPath);
                        if (nc) nc->kind = ClipKind::Shader;
                    }
                }
            }
            // Visual cue while hovering with a payload.
            if (ImGui::GetDragDropPayload() &&
                ImGui::GetDragDropPayload()->IsDataType("SC_SHADER_PATH")) {
                ImVec2 zMin = ImGui::GetItemRectMin();
                ImVec2 zMax = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    zMin, zMax, IM_COL32(74, 140, 255, 30), 4.0f);
                ImGui::GetWindowDrawList()->AddRect(
                    zMin, zMax, IM_COL32(74, 140, 255, 120), 4.0f, 0, 1.5f);
                ImGui::GetWindowDrawList()->AddText(
                    ImGui::GetFont(), 11.0f,
                    ImVec2(zMin.x + 12, zMin.y + (dropZoneH - 11.0f) * 0.5f),
                    IM_COL32(255, 255, 255, 180), "Drop to create new layer");
            }
            ImGui::EndDragDropTarget();
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
        // Bounded, searchable transition picker. The catalog grew past what a
        // bare BeginCombo can display — items beyond the screen fold were
        // unreachable. Phase B replaces this with a live-preview grid; this is
        // the Phase A floor: search field on top + max-height scrollable child.
        if (ImGui::BeginCombo("##clipXition", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
            static char clipXitionFilter[64] = "";
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##clipXitionFilter", "Search transitions...",
                                     clipXitionFilter, sizeof(clipXitionFilter));
            ImGui::Separator();

            ImGui::BeginChild("##clipXitionList", ImVec2(260, 240), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            auto matches = [&](const std::string& s) -> bool {
                if (clipXitionFilter[0] == '\0') return true;
                std::string a = s, b = clipXitionFilter;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                return a.find(b) != std::string::npos;
            };
            if (matches("No transition")) {
                if (ImGui::Selectable("No transition", selClip->transitionInName.empty())) {
                    selClip->transitionInName.clear();
                }
            }
            ImGui::Separator();
            for (const auto& n : names) {
                if (!matches(n)) continue;
                bool sel = (selClip->transitionInName == n);
                if (ImGui::Selectable(n.c_str(), sel)) selClip->transitionInName = n;
            }
            ImGui::EndChild();
            ImGui::EndCombo();
        }

        // Live-preview catalog grid — opens a popup with each transition
        // running on the user's actual on-stage textures. Phase B: replaces
        // the BeginCombo as the recommended way to pick a transition; the
        // combo above stays as a fast keyboard-driven path.
        ImGui::SameLine(0, 6);
        if (ImGui::SmallButton("Browse...##clipXitionGrid")) {
            ImGui::OpenPopup("##clipXitionGridPopup");
        }
        ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Once);
        if (ImGui::BeginPopup("##clipXitionGridPopup")) {
            ImGui::TextDisabled("Live transition catalog");
            ImGui::SameLine();
            ImGui::TextDisabled(" · click any tile to apply");
            ImGui::Separator();

            // Pull two on-stage textures as A/B references — first two layers
            // with non-zero textureId. Single-layer setups double up.
            GLuint texA = 0, texB = 0;
            int srcW = 1280, srcH = 720;
            for (int li = 0; li < m_layerStack.count(); li++) {
                const auto& lp = m_layerStack[li];
                if (!lp) continue;
                GLuint t = lp->textureId();
                if (!t) continue;
                if (texA == 0) texA = t;
                else if (texB == 0) { texB = t; break; }
            }
            if (texB == 0) texB = texA;

            std::string current = selClip->transitionInName;
            std::string picked  = m_transitionCatalog.drawAndPick(
                texA, texB, srcW, srcH, current);
            if (!picked.empty()) {
                selClip->transitionInName = picked;
                selClip->transitionInShaderPath.clear(); // grid picks built-ins
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
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
        if (ImGui::IsItemHovered()) ParamRow::Tooltip(
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

        // ── Source row — which shader/video this clip plays when entered.
        // This is the Ableton-style workflow: each clip on a track can hold a
        // different source, sequencing them as the playhead crosses boundaries.
        ImGui::Spacing();
        {
            static char srcBuf[512];
            static uint32_t lastSrcClipId = 0;
            if (lastSrcClipId != s_ctxClipId) {
                std::snprintf(srcBuf, sizeof(srcBuf), "%s", selClip->sourcePath.c_str());
                lastSrcClipId = s_ctxClipId;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 158, 172, 230));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("  Source");
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 8);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("##clipSrc", srcBuf, sizeof(srcBuf),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
                selClip->sourcePath = srcBuf;
                selClip->kind = ClipKind::Shader;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                selClip->sourcePath = srcBuf;
                selClip->kind = ClipKind::Shader;
            }
            ImGui::SameLine(0, 4);
            if (ImGui::SmallButton("...##srcFile")) {
                std::string p = openFileDialog(
                    "Shaders & Images\0*.fs;*.frag;*.isf;*.png;*.jpg;*.mp4;*.mov\0All\0*.*\0");
                if (!p.empty()) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                    selClip->sourcePath = p;
                    std::snprintf(srcBuf, sizeof(srcBuf), "%s", p.c_str());
                    auto sl = p.find_last_of("/\\");
                    selClip->name = (sl == std::string::npos) ? p : p.substr(sl + 1);
                    selClip->kind = ClipKind::Shader;
                }
            }
            if (ImGui::IsItemHovered()) ParamRow::Tooltip("Browse for a shader or image file");
            // ShaderClaw quick-pick — only when connected.
            if (m_shaderClaw.isConnected() && !m_shaderClaw.shaders().empty()) {
                ImGui::SameLine(0, 6);
                ImGui::SetNextItemWidth(160);
                if (ImGui::BeginCombo("##clipSrcSC", "From ShaderClaw...")) {
                    for (const auto& sc : m_shaderClaw.shaders()) {
                        const char* lbl = sc.title.empty() ? sc.file.c_str() : sc.title.c_str();
                        if (ImGui::Selectable(lbl)) {
                            m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                            selClip->sourcePath = sc.fullPath;
                            std::snprintf(srcBuf, sizeof(srcBuf), "%s", sc.fullPath.c_str());
                            selClip->name = sc.title.empty() ? sc.file : sc.title;
                            selClip->kind = ClipKind::Shader;
                            lastSrcClipId = 0; // force buffer refresh
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if (!selClip->sourcePath.empty()) {
                ImGui::SameLine(0, 4);
                if (ImGui::SmallButton("x##clearSrc")) {
                    m_undoStack.pushState(m_layerStack, m_selectedLayer, m_timeline);
                    selClip->sourcePath.clear();
                    srcBuf[0] = '\0';
                }
            }
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
                // Searchable, bounded list — see clipXition above for rationale.
                static char trPickerFilter[64] = "";
                ImGui::SetNextItemWidth(220);
                ImGui::InputTextWithHint("##trPickerFilter", "Search...",
                                         trPickerFilter, sizeof(trPickerFilter));
                auto trMatches = [&](const std::string& s) -> bool {
                    if (trPickerFilter[0] == '\0') return true;
                    std::string a = s, b = trPickerFilter;
                    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                    return a.find(b) != std::string::npos;
                };
                ImGui::BeginChild("##trPickerList", ImVec2(220, 220), false);
                for (const auto& n : names) {
                    if (!trMatches(n)) continue;
                    bool sel = (tr->name == n && tr->shaderPath.empty());
                    if (ImGui::Selectable(n.c_str(), sel)) {
                        tr->name = n;
                        tr->shaderPath.clear(); // switching back to built-in
                    }
                }
                ImGui::EndChild();

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
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.87f, 0.92f, 1.0f));
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
        // "Live edge" blink — pulses the playhead opacity so it reads like a
        // recording indicator (the playhead position IS live; see advance()).
        // Smooth eased sine of real ImGui time, ~1.75 Hz, mapped to the
        // [kPhBlinkMinA, 1.0] alpha band. Only animates while the timeline is
        // actually on screen (this whole block only runs when open/animating).
        const float kPhBlinkHz   = 1.75f;  // pulses per second
        const float kPhBlinkMinA = 0.35f;  // dimmest the playhead ever gets
        float phPhase = sinf((float)ImGui::GetTime() * kPhBlinkHz * 6.2831853f);
        // sin → [0,1] then ease (smoothstep) so the pulse breathes rather than
        // ticking linearly through the midpoint.
        float phT = 0.5f + 0.5f * phPhase;
        phT = phT * phT * (3.0f - 2.0f * phT);
        float phAlpha = kPhBlinkMinA + (1.0f - kPhBlinkMinA) * phT;
        ImU32 phLineCol = IM_COL32(255, 200, 60, (int)(230.0f * phAlpha));
        ImU32 phHandleCol = IM_COL32(255, 200, 60, (int)(255.0f * phAlpha));
        // Soft glow behind the line at the peak of the pulse — fades out as the
        // playhead dims so it never competes with the ruler ticks.
        float phGlowA = (phT - 0.5f) * 2.0f;
        if (phGlowA > 0.0f) {
            dl->AddLine(ImVec2(phX, phY0), ImVec2(phX, phY1),
                        IM_COL32(255, 200, 60, (int)(70.0f * phGlowA)), 6.0f);
        }
        dl->AddLine(ImVec2(phX, phY0), ImVec2(phX, phY1),
                    phLineCol, 2.0f);
        // Playhead triangle handle on ruler
        dl->AddTriangleFilled(ImVec2(phX - 6, phY0),
                              ImVec2(phX + 6, phY0),
                              ImVec2(phX, phY0 + 10),
                              phHandleCol);
        // Timecode readout is already shown in the transport row above — no
        // second label here. (Drawing it over the ruler collided with tick labels.)
    }

  } // end of if (!s_tlCollapsed)

    // Measure actual content height for next frame's slide target. CursorPosY
    // is window-local; add a bit of bottom padding to match the window style.
    // Only update the shared target while fully open so the measurement isn't
    // taken from a clipped/partway-slid frame.
    s_tlMeasuredContentH = ImGui::GetCursorPosY() + 14.0f;
    // Auto-height disabled: timeline stays at user-set height even as layers
    // are added/removed. Height is only changed via the drag handle at the top.

    ImGui::PopClipRect();
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
            m_recorder.start(fname, zone.warpFBO.width(), zone.warpFBO.height(), recorderFpsHint(m_targetFPS));
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
        // Use the global 8.0f frame rounding for consistency (was a stray 4.0f).
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
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
            ParamRow::Tooltip("Set stream key in the Stream tab");
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
    // No-op. The brand mark + overflow menu it used to draw now live in
    // the workspace nav row (Application::renderNavBarPrefix), so we have
    // ONE chrome row instead of two. Body kept for git history / ABI.
}

// Brand mark + overflow ("more") menu. Drawn inline at the start of the
// workspace nav row in ViewportPanel::renderNavBar. NO BeginMainMenuBar —
// this writes into whatever window is current (the viewport's docked
// window) so the chrome reads as a single row.
void Application::renderNavBarPrefix() {
    // Hamburger / brand mark draws on the viewport FOREGROUND list so it
    // shares the same z-priority as the rest of the nav-row chrome
    // (band + workspace tabs + right cluster, all routed through fg in
    // ViewportPanel::renderNavBar). Without this the band on fg would
    // paint over a hamburger drawn into the Canvas window draw list.
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImU32 kMark = IM_COL32(232, 238, 250, 235);

    const float kBtn   = 28.0f;   // matches the inner pill height in the row
    const float kGap   = 8.0f;
    const float kGlyph = 16.0f;

    // (Brand mark removed per user request — the chrome row leads with the
    //  settings gear, no separate brand glyph.)
    (void)kMark;

#ifdef __APPLE__
    // Reserve space on the left for the macOS traffic-light buttons (close /
    // min / max). The native title bar has been merged into the content area
    // so the lights now sit in the same row we draw into. In editor (F11
    // borderless) OR native (green-button) fullscreen AppKit hides them, so
    // the inset is skipped so the row doesn't get an awkward empty gap.
    {
        bool nativeFs = EaselMac_IsNativeFullScreen(m_window) != 0;
        if (!m_editorFullscreen && !nativeFs) {
            const float kTrafficLightInset = 70.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kTrafficLightInset);
        }
    }
#endif

    // Overflow ("···") menu — three Lucide dots, opens an ImGui popup with
    // Edit / File / Layer / Zone submenus.
    {
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImVec2 sz(kBtn, kBtn);
        bool clicked = ImGui::InvisibleButton("##nav_more", sz);
        bool hov     = ImGui::IsItemHovered();
        float cx = cur.x + sz.x * 0.5f, cy = cur.y + sz.y * 0.5f;
        if (hov) {
            dl->AddCircleFilled(ImVec2(cx, cy), sz.x * 0.5f,
                                IM_COL32(255, 255, 255, 18), 24);
        }
        ImU32 menuCol = hov ? kMark : IM_COL32(170, 178, 195, 220);
        // Hamburger menu — three short horizontal lines stacked. Reads
        // unambiguously as "open menu" without the cog overtones the gear
        // had (which suggested settings/preferences only).
        {
            float w  = kGlyph * 0.95f;
            float th = 1.6f;
            float gap = kGlyph * 0.32f;
            float x0 = cx - w * 0.5f;
            float x1 = cx + w * 0.5f;
            dl->AddLine(ImVec2(x0, cy - gap), ImVec2(x1, cy - gap), menuCol, th);
            dl->AddLine(ImVec2(x0, cy),       ImVec2(x1, cy),       menuCol, th);
            dl->AddLine(ImVec2(x0, cy + gap), ImVec2(x1, cy + gap), menuCol, th);
        }
        if (clicked) ImGui::OpenPopup("##nav_more_popup");
    }
    if (ImGui::BeginPopup("##nav_more_popup")) {
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
                    int li = m_layerStack.count() - 1;
                    uint32_t rid = m_layerStack[li] ? m_layerStack[li]->id : 0;
                    m_layerStack.removeLayer(li);
                    if (rid) m_timeline.removeTrackForLayer(rid);
                }
                m_selectedLayer = -1;
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
            if (ImGui::MenuItem("Add Fluid Simulation")) {
                addFluid();
            }
            if (ImGui::MenuItem("Add 3D Fluid")) {
                addFluid3D();
            }
            if (ImGui::MenuItem("Add Hologram Model...")) {
                addHologramModel();
            }
            if (ImGui::MenuItem("Add Moving Company (F-117)")) {
                addMovingCompany();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Project...")) {
                std::string path = saveFileDialog("Easel Project\0*.easel\0", "easel");
                if (!path.empty()) saveProject(path);
            }
            if (ImGui::MenuItem("Publish to Mobile")) {
                publishPlayToAgent();
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
        if (ImGui::BeginMenu("Output")) {
            // Global Audio -> Shaders switch (feeds the Audio Feature Bus to
            // every shader's GLSL uniforms; off = neutralized / fed zeros).
            ImGui::MenuItem("Audio -> Shaders", nullptr, &m_audioToShaders);
            ImGui::Separator();
            // Two alternative output modes (the existing per-zone behaviour is
            // "Independent"; "Spanned" slices one wide canvas across screens).
            bool spanned = (m_outputMode == OutputMode::Spanned);
            ImGui::TextDisabled("Output Mode");
            if (ImGui::RadioButton("Independent (unique per screen)", !spanned)) {
                m_outputMode = OutputMode::Independent;
            }
            if (ImGui::RadioButton("Spanned (one visual across screens)", spanned)) {
                m_outputMode = OutputMode::Spanned;
                ensureSpanZone();
            }

            if (m_outputMode == OutputMode::Spanned) {
                ensureSpanZone();
                ImGui::Separator();
                ImGui::TextDisabled("Span Canvas (custom resolution)");
                int wh[2] = { m_spanWidth, m_spanHeight };
                ImGui::SetNextItemWidth(170);
                if (ImGui::InputInt2("W x H", wh)) {
                    m_spanWidth  = wh[0] < 16 ? 16 : wh[0];
                    m_spanHeight = wh[1] < 16 ? 16 : wh[1];
                    ensureSpanZone();
                }
                if (ImGui::SmallButton("3840x1080")) { m_spanWidth = 3840; m_spanHeight = 1080; ensureSpanZone(); }
                ImGui::SameLine();
                if (ImGui::SmallButton("2560x720"))  { m_spanWidth = 2560; m_spanHeight = 720;  ensureSpanZone(); }
                ImGui::SameLine();
                if (ImGui::SmallButton("5760x1080")) { m_spanWidth = 5760; m_spanHeight = 1080; ensureSpanZone(); }

                ImGui::Separator();
                ImGui::TextDisabled("Screen Slices (left -> right)");
                auto monitors = ProjectorOutput::enumerateMonitors();
                int nslices = (int)m_spanSlices.size();
                ImGui::SetNextItemWidth(140);
                if (ImGui::SliderInt("Slices", &nslices, 1, 4)) {
                    if (nslices < 1) nslices = 1;
                    m_spanSlices.assign(nslices, SpanSlice{});
                    layoutSpanSlices();
                }
                for (int s = 0; s < (int)m_spanSlices.size(); s++) {
                    ImGui::PushID(s);
                    auto& slice = m_spanSlices[s];
                    const char* cur = "- none -";
                    if (slice.monitor >= 0 && slice.monitor < (int)monitors.size())
                        cur = monitors[slice.monitor].name.c_str();
                    char label[64];
                    snprintf(label, sizeof(label), "Slice %d  [%.2f-%.2f]", s + 1, slice.u0, slice.u1);
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::BeginCombo(label, cur)) {
                        if (ImGui::Selectable("- none -", slice.monitor < 0)) slice.monitor = -1;
                        for (int mi = 0; mi < (int)monitors.size(); mi++) {
                            bool sel = (slice.monitor == mi);
                            if (ImGui::Selectable(monitors[mi].name.c_str(), sel)) slice.monitor = mi;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Even split L->R")) layoutSpanSlices();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layer")) {
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Zone")) {
            if (ImGui::MenuItem("Add Zone")) addZone();
            if (ImGui::MenuItem("Duplicate Active Zone")) {
                duplicateZone(m_activeZone);
                m_activeZone = (int)m_zones.size() - 1;
            }
            if (m_zones.size() > 1 && ImGui::MenuItem("Remove Active Zone")) {
                removeZone(m_activeZone);
            }
            ImGui::EndMenu();
        }
        if (UIManager::sMode == UIManager::WorkspaceMode::Show) {
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Timecode Window", nullptr, &m_showTimecodeWindow);
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    // (Mic moved to the bottom transport bar next to the Sound dropdown.)
    ImGui::SameLine(0, kGap + 4.0f);
}

// Legacy menu bar implementation — disabled, body kept for diff history.
#if 0
void Application::renderMenuBar_legacy() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
    if (ImGui::BeginMainMenuBar()) {
        if (ImGuiWindow* mw = ImGui::GetCurrentWindow()) mw->FontWindowScale = 0.88f;
#ifdef __APPLE__
        // Reserve space on the left for the macOS traffic-light buttons
        // (close / min / max) — the window title bar has been merged into
        // the content area so they now sit inside our ImGui row. In
        // either editor (F11 borderless) OR native (green-button)
        // fullscreen AppKit hides the traffic-lights, so skip the inset
        // or the menu row gets an ugly empty gap on the left.
        bool nativeFs = EaselMac_IsNativeFullScreen(m_window) != 0;
        if (!m_editorFullscreen && !nativeFs) {
            const float kTrafficLightInset = 78.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kTrafficLightInset);
        } else {
            // Fullscreen (editor OR native): AppKit hides the traffic-
            // lights. Use a 12px inset to match the left-edge padding
            // used by the floating toolbar / layer panel — visual
            // alignment with other left-edge UI elements.
            ImGui::SetCursorPosX(12.0f);
        }
#endif
        // Awesome-design: brand glyph in the top-left corner. A small
        // V-triangle inside a circular ring — single procedural shape so
        // we don't need an asset. Sits flush with traffic-light row,
        // reads as the app's identity at a glance.
        {
            float baseY = ImGui::GetCursorPosY();
            ImVec2 cur = ImGui::GetCursorScreenPos();
            const float r = 9.0f;
            ImVec2 c(cur.x + r + 2.0f, cur.y + ImGui::GetFrameHeight() * 0.5f);
            ImDrawList* d = ImGui::GetWindowDrawList();
            // Faint inner fill + sharper ring outline for visible weight.
            d->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 10), 24);
            d->AddCircle      (c, r, IM_COL32(255, 255, 255, 130), 24, 1.1f);
            // Apex-up V mark, filled — reads as a confident brand mark
            // even at this small size. Slightly inset so the ring breathes.
            float tw = r * 0.60f, th = r * 0.66f;
            d->AddTriangleFilled(
                ImVec2(c.x,        c.y - th * 0.60f),
                ImVec2(c.x - tw,   c.y + th * 0.50f),
                ImVec2(c.x + tw,   c.y + th * 0.50f),
                IM_COL32(232, 238, 250, 245));
            ImGui::Dummy(ImVec2(r * 2.0f + 10.0f, 0));
            ImGui::SameLine();
            ImGui::SetCursorPosY(baseY);
        }
        // Phase 4 — minimal top bar. The four legacy EDIT/FILE/LAYER/ZONE
        // strips collapse into a single "···" button. All items remain
        // available as submenus under this single dropdown so existing
        // muscle memory (Add Image, New Project, Move Up, etc.) still
        // works — the chrome just gets out of the way.
        // U+22EF MIDLINE HORIZONTAL ELLIPSIS — the typographic "more" glyph;
        // single codepoint, cleaner kerning than three U+2022 bullets.
        if (ImGui::BeginMenu("\xe2\x8b\xaf")) {
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
            if (ImGui::MenuItem("Add Fluid Simulation")) {
                addFluid();
            }
            if (ImGui::MenuItem("Add 3D Fluid")) {
                addFluid3D();
            }
            if (ImGui::MenuItem("Add Hologram Model...")) {
                addHologramModel();
            }
            if (ImGui::MenuItem("Add Moving Company (F-117)")) {
                addMovingCompany();
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
                        addNDISource(m_ndiSources[i].name, m_ndiSources[i].url);
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
            if (ImGui::MenuItem("Publish to Mobile")) {
                publishPlayToAgent();
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
            // Sources submenu — moved out of the Sources panel header
            // so the panel itself stays clean. Each source's connection
            // controls (Connect / Disconnect / Refresh) live here.
            if (ImGui::BeginMenu("Sources")) {
                bool scConn = m_shaderClaw.isConnected();
                if (ImGui::MenuItem(scConn ? "ShaderClaw: Refresh" : "ShaderClaw: (not connected)",
                                    nullptr, false, scConn)) {
                    m_shaderClaw.refreshManifest();
                }
                if (ImGui::MenuItem("ShaderClaw: Disconnect", nullptr, false, scConn)) {
                    m_shaderClaw.disconnect();
                    m_scThumbnails.clear();
                    m_scThumbRenderer.reset();
                    m_scPreview.reset();
                    m_scPreviewPath.clear();
                }
                ImGui::Separator();
                bool ethConn = m_ethereaClient.wsConnected() || m_ethereaClient.sseConnected();
                if (ImGui::MenuItem("Etherea: Disconnect", nullptr, false, ethConn)) {
                    m_ethereaClient.disconnect();
                }
                ImGui::EndMenu();
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
        ImGui::EndMenu();  // close the "···" container menu
        }

        // (Voice mic moved to the timeline transport row — see renderTimelinePanel.
        // Lives next to REC there since it's a transport-adjacent action, not a
        // chrome affordance, and menu-bar input handling was eating the hold gesture.)

        // Fullscreen lives next to the composition chip in the viewport;
        // GO LIVE lives next to REC in the timeline transport. The menu bar
        // now carries only the Edit/File/Layer/Zone dropdowns so it fits
        // inside the unified title bar.
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
}
#endif // #if 0 — legacy renderMenuBar disabled

#ifdef HAS_FFMPEG
void Application::renderGoLiveButton() {
    const char* lbl = m_rtmpOutput.isActive() ? "END LIVE" : "GO LIVE";
    if (m_rtmpOutput.isActive()) {
        // Streaming → red, mirroring REC's domain convention.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.06f, 0.06f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.10f, 0.10f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.14f, 0.14f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 0.55f, 0.55f, 1.00f));
    } else {
        // Idle → neutral ghost button matching the rest of the chrome. Bright
        // white text once a stream key is set (signals "ready"), tertiary text
        // otherwise (signals "needs config"). No second accent hue.
        bool hasKey = m_streamKeyBuf[0] != '\0';
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 1.00f, 1.00f, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 1.00f, 1.00f, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            hasKey ? ImVec4(0.969f, 0.973f, 0.973f, 1.00f)   // textPrimary
                   : ImVec4(0.541f, 0.561f, 0.596f, 1.00f)); // textTertiary
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

#ifdef __APPLE__
// Defined in PlatformMac.mm — auto-hide/restore the menu bar + Dock so the
// borderless editor fullscreen looks truly full-screen.
extern "C" void macSetFullscreenPresentation(bool on);
#endif

void Application::toggleEditorFullscreen() {
    if (!m_editorFullscreen) {
        glfwGetWindowPos(m_window, &m_savedWindowX, &m_savedWindowY);
        glfwGetWindowSize(m_window, &m_savedWindowW, &m_savedWindowH);
        // Pick the monitor the window currently sits on.
        int monCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monCount);
        GLFWmonitor* best = glfwGetPrimaryMonitor();
        int wx, wy;
        glfwGetWindowPos(m_window, &wx, &wy);
        for (int mi = 0; mi < monCount; mi++) {
            int mx, my;
            glfwGetMonitorPos(monitors[mi], &mx, &my);
            const GLFWvidmode* m = glfwGetVideoMode(monitors[mi]);
            if (wx >= mx && wx < mx + m->width &&
                wy >= my && wy < my + m->height) {
                best = monitors[mi];
                break;
            }
        }
        int mx = 0, my = 0;
        glfwGetMonitorPos(best, &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(best);
        // BORDERLESS WINDOWED FULLSCREEN — cover the monitor by repositioning +
        // resizing a *decorationless* window (monitor = nullptr). The previous
        // path passed the monitor handle + refreshRate, which is TRUE
        // fullscreen: on macOS that captures the display and does a video-mode
        // set, stalling the app ~0.5-1s ("freezes up a bit") on every toggle
        // plus a black flash. Borderless covers the same pixels with no display
        // capture and no mode switch — just a one-frame window resize.
        glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(m_window, nullptr, mx, my,
                             mode->width, mode->height, 0);
#ifdef __APPLE__
        macSetFullscreenPresentation(true);  // auto-hide menu bar + Dock
#endif
        m_editorFullscreen = true;
    } else {
        glfwSetWindowMonitor(m_window, nullptr,
                             m_savedWindowX, m_savedWindowY,
                             m_savedWindowW, m_savedWindowH, 0);
        glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
#ifdef __APPLE__
        macSetFullscreenPresentation(false); // restore menu bar + Dock
        // Restoring decorations recreates the standard window buttons with
        // their default actions — re-unify the title bar so the traffic lights
        // are re-positioned AND the green button stays bound to fullscreen.
        EaselMac_UnifyTitleBar(m_window);
#endif
        m_editorFullscreen = false;
        m_presentMode = false;  // present mode only exists while fullscreen
    }
}

void Application::registerLayerWithZones(uint32_t layerId) {
    // Scrub ghosts FIRST: zone sets hold raw ids and deleted layers leave
    // theirs behind, so after a restart resets the id counter a NEW layer can
    // recycle a ghost id and silently "join" zones it was never placed in —
    // the 2026-07-07 painting-fx→FLUX-IN feedback loop. A just-created layer
    // belongs to no zone, so erasing its id everywhere is always correct.
    for (auto& z : m_zones) z->visibleLayerIds.erase(layerId);
    // Agent-managed feed layers (non-empty managedKey) are plumbing: they join
    // a zone only via explicit placement (addZoneLayerByKey / the bus-daisy
    // composite path), never this new-layer auto-insert. Without this, every
    // managed FluxRT layer leaked into curated zones — including the
    // composition that feeds Flux itself (the 2026-06-10 feedback loop).
    for (const auto& l : m_layerStack.layers()) {
        if (l && l->id == layerId && !l->managedKey.empty()) return;
    }
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

void Application::addFluid() {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<FluidSource>();
    // Match the active zone's resolution so the sim aspect is correct.
    int w = 1280, h = 720;
    if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() && m_zones[m_activeZone]) {
        w = m_zones[m_activeZone]->width;
        h = m_zones[m_activeZone]->height;
    }
    if (!src->init(w, h)) {
        std::cerr << "Failed to init FluidSource" << std::endl;
        return;
    }
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "Fluid Simulation";
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

void Application::addFluid3D() {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<FluidSource3D>();
    int w = 1280, h = 720;
    if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() && m_zones[m_activeZone]) {
        w = m_zones[m_activeZone]->width;
        h = m_zones[m_activeZone]->height;
    }
    if (!src->init(w, h)) {
        std::cerr << "Failed to init FluidSource3D" << std::endl;
        return;
    }
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "3D Fluid";
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

void Application::addHologramModel(const std::string& pathArg) {
    // Pick the model first so the layer is born with content. An explicit
    // pathArg (OSC/scripted) skips the native picker.
    std::string path = pathArg;
    if (path.empty()) {
        path = openFileDialog(
            "3D Models\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0"
            "glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");
    }
    if (path.empty()) return;   // user cancelled

    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<HologramModelSource>();
    int w = 1280, h = 720;
    if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() && m_zones[m_activeZone]) {
        w = m_zones[m_activeZone]->width;
        h = m_zones[m_activeZone]->height;
    }
    if (!src->init(w, h)) {
        std::cerr << "Failed to init HologramModelSource" << std::endl;
        return;
    }
    src->loadModel(path);

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "Hologram Model";
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

void Application::addMovingCompany() {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto src = std::make_shared<MovingCompanySource>();
    if (!src->init()) {
        std::cerr << "Failed to init MovingCompanySource" << std::endl;
        return;
    }

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = src;
    layer->name = "Moving Company";

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
        std::cerr << "[Easel] SHADER LOAD FAILED: " << path << std::endl;
        return;
    }
    std::cerr << "[Easel] Shader loaded OK: " << path
              << " | initialized=" << source->isInitialized()
              << " | texId=" << source->textureId()
              << " | size=" << source->width() << "x" << source->height()
              << std::endl;

    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->source = source;
    size_t slash = path.find_last_of("/\\");
    layer->name = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    // Restore saved param preset for this shader file, if any. Keyed by the
    // bare filename (matches manifest's `file` field used elsewhere).
    {
        size_t s = path.find_last_of("/\\");
        std::string fileKey = (s != std::string::npos) ? path.substr(s + 1) : path;
        m_shaderPresets.apply(fileKey, *source);
    }

    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);

    // Register with ShaderClaw bridge for hot-reload
    if (m_shaderClaw.isConnected()) {
        m_shaderClaw.watchSource(path, source);
    }

    // Voice-native auto-binding: any TEXT shader (one whose ISF declares a
    // `msg` text input) gets its message bound to the live LAST WORDS so
    // what the user just said appears on-shader by default. cue.latest is
    // the most recent utterance (vs cue.transcript which is the full
    // running transcript). Non-text shaders (visual generators, effects)
    // get NO transcript binding, so voice doesn't bleed into unrelated
    // parameters.
    if (source) {
        const auto& inputs = source->inputs();
        bool hasMsg = false;
        for (const auto& inp : inputs) {
            if (inp.type == "text" && inp.name == "msg") { hasMsg = true; break; }
        }
        if (hasMsg) {
            m_dataBus.bind(layer->id, "msg", "cue.latest");
        }
    }
}

#ifdef HAS_NDI
void Application::addNDISource(const std::string& senderName, const std::string& senderUrl) {
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto source = std::make_shared<NDISource>();
    if (!source->connect(senderName, senderUrl)) {
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

void Application::ensureManagedNDILayer(const std::string& slot,
                                        const std::string& senderName) {
    if (slot.empty() || senderName.empty()) {
        std::cerr << "[OSC] ensure/ndi ignored: empty slot or source\n";
        return;
    }
    auto nameForSender = [](const std::string& s) {
        size_t slash = s.find('/');
        return std::string("NDI: ")
             + (slash != std::string::npos ? s.substr(slash + 1) : s);
    };

    // Idempotent by slot: reuse an existing managed layer for this key.
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != slot) continue;
        // Already pointed at this sender — nothing to do.
        if (l->source && l->source->typeName() == "NDI"
            && l->source->sourcePath() == senderName) {
            return;
        }
        auto source = std::make_shared<NDISource>();
        if (!source->connect(senderName)) {
            std::cerr << "[OSC] ensure/ndi: failed to connect NDI source: "
                      << senderName << std::endl;
            return;  // leave the existing layer untouched on connect failure
        }
        m_undoStack.pushState(m_layerStack, m_selectedLayer);
        l->source = source;          // releasing the old NDISource disconnects it
        l->name = nameForSender(senderName);
        return;
    }

    // No managed layer for this slot yet — create one, tagged with the key.
    auto source = std::make_shared<NDISource>();
    if (!source->connect(senderName)) {
        std::cerr << "[OSC] ensure/ndi: failed to connect NDI source: "
                  << senderName << std::endl;
        return;
    }
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->managedKey = slot;
    layer->source = source;
    layer->name = nameForSender(senderName);
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}
#endif

void Application::clearManagedLayers() {
    // Remove agent-managed layers (non-empty managedKey) from the top down so
    // indices stay valid. Mirrors the manual /easel/layer/remove path (undo
    // snapshot once, drop the timeline track, clamp the selection).
    bool any = false;
    for (int i = m_layerStack.count() - 1; i >= 0; i--) {
        auto& l = m_layerStack[i];
        if (!l || l->managedKey.empty()) continue;
        if (!any) {
            m_undoStack.pushState(m_layerStack, m_selectedLayer);
            any = true;
        }
        uint32_t rid = l->id;
        m_layerStack.removeLayer(i);
        if (rid) m_timeline.removeTrackForLayer(rid);
    }
    if (any && m_selectedLayer >= m_layerStack.count())
        m_selectedLayer = m_layerStack.count() - 1;
}

void Application::ensureManagedShaderLayer(const std::string& slot,
                                           const std::string& shaderPath) {
    if (slot.empty() || shaderPath.empty()) {
        std::cerr << "[OSC] ensure/shader ignored: empty slot or path\n";
        return;
    }
    auto basename = [](const std::string& p) {
        size_t s = p.find_last_of("/\\");
        return s != std::string::npos ? p.substr(s + 1) : p;
    };
    // Voice-native binding: a text shader (ISF 'msg' text input) gets the live
    // last-words bound so transcript text shows on-shader (mirrors loadShader).
    // Caption slots ("<zone>:captions", the mobile captions toggle) bind to the
    // raw live transcript (etherea.latest, "Latest Words") instead of Cue's
    // curated utterance — captions must follow speech, not Cue's pushes.
    auto bindIfText = [this, &slot](const std::shared_ptr<Layer>& layer) {
        if (!layer->source || !layer->source->isShader()) return;
        auto* sh = static_cast<ShaderSource*>(layer->source.get());
        for (const auto& inp : sh->inputs()) {
            if (inp.type == "text" && inp.name == "msg") {
                bool captions = slot.size() >= 9 &&
                    slot.compare(slot.size() - 9, 9, ":captions") == 0;
                m_dataBus.bind(layer->id, "msg",
                               captions ? "etherea.latest" : "cue.latest");
                break;
            }
        }
    };

    // Idempotent by slot: reuse an existing managed layer for this key.
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != slot) continue;
        if (l->source && l->source->isShader() && l->source->sourcePath() == shaderPath) {
            return;  // already the right shader
        }
        auto src = std::make_shared<ShaderSource>();
        if (!src->loadFromFile(shaderPath)) {
            std::cerr << "[OSC] ensure/shader: failed to load shader: " << shaderPath << std::endl;
            return;
        }
        m_shaderPresets.apply(basename(shaderPath), *src);
        m_undoStack.pushState(m_layerStack, m_selectedLayer);
        l->source = src;
        l->name = basename(shaderPath);
        // A managed-slot swap is a fresh start: voice decay may have driven
        // the layer's opacity to ~0 while a text shader was bound, and the
        // stale msg binding keeps doing so for every later shader on this
        // slot. Reset both; bindIfText re-binds for incoming text shaders.
        l->opacity = 1.0f;
        m_dataBus.bind(l->id, "msg", "");
        if (m_shaderClaw.isConnected()) m_shaderClaw.watchSource(shaderPath, src);
        bindIfText(l);
        return;
    }

    // No managed layer for this slot yet — create one on top, tagged with the key.
    auto src = std::make_shared<ShaderSource>();
    if (!src->loadFromFile(shaderPath)) {
        std::cerr << "[OSC] ensure/shader: failed to load shader: " << shaderPath << std::endl;
        return;
    }
    m_shaderPresets.apply(basename(shaderPath), *src);
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->managedKey = slot;
    layer->source = src;
    layer->name = basename(shaderPath);
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
    if (m_shaderClaw.isConnected()) m_shaderClaw.watchSource(shaderPath, src);
    bindIfText(layer);
}

void Application::ensureManagedFluidLayer(const std::string& slot, bool threeD) {
    if (slot.empty()) {
        std::cerr << "[OSC] ensure/fluid ignored: empty slot\n";
        return;
    }
    // Match the active zone's resolution so the sim aspect is correct (as addFluid).
    int w = 1280, h = 720;
    if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() && m_zones[m_activeZone]) {
        w = m_zones[m_activeZone]->width;
        h = m_zones[m_activeZone]->height;
    }
    const char* kindName = threeD ? "3D Fluid" : "Fluid Simulation";
    auto makeFluid = [&]() -> std::shared_ptr<ContentSource> {
        std::shared_ptr<ContentSource> src;
        bool ok = false;
        if (threeD) {
            auto f3 = std::make_shared<FluidSource3D>();
            ok = f3->init(w, h);
            src = f3;
        } else {
            auto f = std::make_shared<FluidSource>();
            ok = f->init(w, h);
            src = f;
        }
        if (!ok) {
            std::cerr << "[OSC] ensure/fluid: " << kindName << " init failed\n";
            return nullptr;
        }
        return src;
    };
    auto isWantedKind = [&](ContentSource* s) {
        return threeD ? (dynamic_cast<FluidSource3D*>(s) != nullptr)
                      : (dynamic_cast<FluidSource*>(s) != nullptr);
    };
    // Idempotent by slot: a managed fluid layer for this key is already what we want.
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != slot) continue;
        if (l->source && isWantedKind(l->source.get())) {
            return;  // already a managed fluid layer of this kind for this slot
        }
        // Slot holds something else — swap in a fresh fluid source.
        auto src = makeFluid();
        if (!src) return;
        m_undoStack.pushState(m_layerStack, m_selectedLayer);
        l->source = src;
        l->name = kindName;
        // A managed-slot swap is a fresh start: voice decay may have driven
        // the layer's opacity to ~0 while a text shader was bound, and the
        // stale msg binding keeps doing so for every later shader on this
        // slot. Reset both; bindIfText re-binds for incoming text shaders.
        l->opacity = 1.0f;
        m_dataBus.bind(l->id, "msg", "");
        return;
    }
    // No managed layer for this slot yet — create one tagged with the key.
    auto src = makeFluid();
    if (!src) return;
    m_undoStack.pushState(m_layerStack, m_selectedLayer);
    auto layer = std::make_shared<Layer>();
    layer->id = m_nextLayerId++;
    layer->managedKey = slot;
    layer->source = src;
    layer->name = kindName;
    m_layerStack.addLayer(layer);
    m_selectedLayer = m_layerStack.count() - 1;
    registerLayerWithZones(layer->id);
}

void Application::removeManagedLayer(const std::string& slot) {
    if (slot.empty()) return;
    for (int i = m_layerStack.count() - 1; i >= 0; i--) {
        auto& l = m_layerStack[i];
        if (!l || l->managedKey != slot) continue;
        m_undoStack.pushState(m_layerStack, m_selectedLayer);
        uint32_t rid = l->id;
        m_layerStack.removeLayer(i);
        if (rid) m_timeline.removeTrackForLayer(rid);
        if (m_selectedLayer >= m_layerStack.count())
            m_selectedLayer = m_layerStack.count() - 1;
        return;
    }
}

void Application::bindManagedLayerImage(const std::string& key, const std::string& name,
                                        const std::string& sourceRef) {
    if (key.empty() || name.empty() || sourceRef.empty()) return;
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != key) continue;
        if (!l->source || !l->source->isShader()) {
            std::cerr << "[OSC] layer/bind-image: " << key << " is not a shader layer\n";
            return;
        }
        auto* shader = static_cast<ShaderSource*>(l->source.get());
        uint32_t asId = (uint32_t)atoi(sourceRef.c_str());
        for (auto& src : m_layerStack.layers()) {
            if (!src || src->id == l->id || !src->source) continue;
            bool match = (asId != 0 && src->id == asId)
                         || src->managedKey == sourceRef
                         || src->name == sourceRef;
            if (!match) continue;
            // Texture id may still be 0 (source warming up) — the per-frame
            // bindings refresh resolves it from sourceLayerId either way.
            shader->bindImageInput(name,
                                   src->source->textureId(),
                                   src->source->width(),
                                   src->source->height(),
                                   src->id,
                                   src->source->isFlippedV());
            std::cerr << "[OSC] layer/bind-image: " << key << "." << name
                      << " <- layer " << src->id << " (" << src->name << ")\n";
            return;
        }
        std::cerr << "[OSC] layer/bind-image: no source layer matches '" << sourceRef << "'\n";
        return;
    }
    std::cerr << "[OSC] layer/bind-image: no managed layer with key " << key << std::endl;
}

void Application::setManagedLayerParam(const std::string& key, const std::string& name, const OSCMessage& msg) {
    if (key.empty() || name.empty()) return;
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != key) continue;
        if (!l->source || !l->source->isShader()) {
            std::cerr << "[OSC] layer/param: " << key << " is not a shader layer\n";
            return;
        }
        auto* shader = static_cast<ShaderSource*>(l->source.get());
        // Float count picks the setter: 4 floats -> setColor (RGBA), 2 ->
        // setPoint2D, 1 -> setFloat (covers float + long/enum, which Easel
        // stores as float); text -> setText; int -> setBool. Mirrors the
        // index-based path.
        if (msg.floats.size() >= 4)
            shader->setColor(name, glm::vec4(msg.floats[0], msg.floats[1],
                                             msg.floats[2], msg.floats[3]));
        else if (msg.floats.size() == 2)
            shader->setPoint2D(name, glm::vec2(msg.floats[0], msg.floats[1]));
        else if (!msg.floats.empty())     shader->setFloat(name, msg.floats[0]);
        else if (msg.strings.size() >= 3) shader->setText(name, msg.strings[2]);
        else if (!msg.ints.empty())       shader->setBool(name, msg.ints[0] != 0);
        return;
    }
    std::cerr << "[OSC] layer/param: no managed layer with key " << key << std::endl;
}

void Application::setManagedLayerBindImage(const std::string& key, const std::string& input,
                                           const std::string& sourceName) {
    if (key.empty() || input.empty()) return;
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != key) continue;
        if (!l->source || !l->source->isShader()) {
            std::cerr << "[OSC] layer/bindImage: " << key << " is not a shader layer\n";
            return;
        }
        auto* shader = static_cast<ShaderSource*>(l->source.get());
        auto& img = shader->imageBindings()[input];
        if (sourceName.empty() || sourceName == "(none)") {
            img.sourceLayerId = 0;
            return;
        }
        // Source resolves by layer name first, then managedKey — agents
        // address their own managed layers by key, humans by display name.
        for (int li = 0; li < m_layerStack.count(); li++) {
            auto& other = m_layerStack[li];
            if (!other || other->id == l->id) continue;
            if (other->name == sourceName || other->managedKey == sourceName) {
                img.sourceLayerId = other->id;
                return;
            }
        }
        std::cerr << "[OSC] layer/bindImage: no source layer named " << sourceName << std::endl;
        return;
    }
    std::cerr << "[OSC] layer/bindImage: no managed layer with key " << key << std::endl;
}

void Application::setManagedLayerAudioBind(const std::string& key, const std::string& param,
                                           const std::string& signalName, const OSCMessage& msg) {
    if (key.empty() || param.empty() || signalName.empty()) return;

    static const std::map<std::string, AudioSignal> kSignals = {
        {"level", AudioSignal::Level},   {"bass", AudioSignal::Bass},
        {"mid", AudioSignal::Mid},       {"high", AudioSignal::High},
        {"beat", AudioSignal::Beat},     {"energy", AudioSignal::Energy},
        {"build", AudioSignal::Build},   {"drop", AudioSignal::Drop},
        {"silence", AudioSignal::Silence}, {"momentum", AudioSignal::Momentum},
    };
    std::string sig = signalName;
    for (auto& c : sig) c = (char)tolower((unsigned char)c);

    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != key || !l->source || !l->source->isShader()) continue;
        auto* shader = static_cast<ShaderSource*>(l->source.get());
        auto& bindings = shader->audioBindings();

        if (sig == "off" || sig == "none") {
            bindings.erase(param);
            return;
        }
        auto it = kSignals.find(sig);
        if (it == kSignals.end()) {
            std::cerr << "[OSC] layer/audiobind: unknown signal '" << signalName << "'\n";
            return;
        }
        for (const auto& in : shader->inputs()) {
            if (in.name != param || in.type != "float") continue;
            float cur = std::get<float>(in.value);
            float amount = !msg.floats.empty()
                ? std::min(1.0f, std::max(0.0f, msg.floats[0])) : 1.0f;
            AudioBinding& ab = bindings[param];
            ab.signal = it->second;
            // Audio pushes the param UP from where the owner set it, by
            // `amount` of the remaining headroom — the slider stays the
            // floor, so remote binds never fight the hand-set value.
            ab.rangeMin = cur;
            ab.rangeMax = cur + amount * (in.maxVal - cur);
            if (msg.floats.size() >= 2)
                ab.smoothing = std::min(1.0f, std::max(0.0f, msg.floats[1]));
            return;
        }
        std::cerr << "[OSC] layer/audiobind: " << key << " has no float param '"
                  << param << "'\n";
        return;
    }
    std::cerr << "[OSC] layer/audiobind: no managed shader layer with key " << key << std::endl;
}

void Application::setManagedLayerAudioPreset(const std::string& key, const std::string& command, const OSCMessage& msg) {
    if (key.empty() || command.empty()) return;
    for (auto& l : m_layerStack.layers()) {
        if (!l || l->managedKey != key || !l->source) continue;

        // Bindable-param list + bindings map, per source type — the same
        // lists the PropertyPanel builds for its audioPresetRow, so the
        // remote recipe modulates exactly what the desktop row would.
        std::vector<AudioPresetEngine::Param> pp;
        std::map<std::string, AudioBinding>* bindings = nullptr;
        const std::string type = l->source->typeName();
        if (l->source->isShader()) {
            auto* shader = static_cast<ShaderSource*>(l->source.get());
            bindings = &shader->audioBindings();
            for (const auto& in : shader->inputs()) {
                if (in.type != "float") continue;
                if (in.name.find("audio") != std::string::npos ||
                    in.name.find("Audio") != std::string::npos) continue;
                pp.push_back({ in.name, std::get<float>(in.value),
                               in.minVal, in.maxVal });
            }
        } else if (type == "Fluid") {
            auto* f = static_cast<FluidSource*>(l->source.get());
            bindings = &f->audioBindings();
            pp = {
                { "curl",               f->m_curlAmount,         0.0f,  60.0f },
                { "splatRadius",        f->m_splatRadius,        0.05f, 1.5f  },
                { "splatIntensity",     f->m_splatIntensity,     0.1f,  4.0f  },
                { "densityDissipation", f->m_densityDissipation, 0.0f,  4.0f  },
                { "autoSpeed",          f->m_autoSpeed,          0.0f,  4.0f  },
                { "autoScale",          f->m_autoScale,          0.0f,  0.5f  },
                { "bloomIntensity",     f->m_bloomIntensity,     0.0f,  2.0f  },
                { "sunraysWeight",      f->m_sunraysWeight,      0.0f,  2.0f  },
            };
        } else if (type == "Fluid3D") {
            auto* f3 = static_cast<FluidSource3D*>(l->source.get());
            bindings = &f3->audioBindings();
            pp = {
                { "brightness",     f3->m_brightness,     0.0f,  6.0f },
                { "rotateSpeed",    f3->m_rotateSpeed,    0.0f,  2.0f },
                { "tilt",           f3->m_tilt,          -1.57f, 1.57f },
                { "zoom",           f3->m_zoom,           0.5f,  4.0f },
                { "gravity",        f3->m_gravity,        0.0f,  4.0f },
                { "vortex",         f3->m_vortex,         0.0f,  3.0f },
                { "turbulence",     f3->m_turbulence,     0.0f,  2.0f },
                { "forceScale",     f3->m_forceScale,     0.1f,  4.0f },
                { "sphereScale",    f3->m_sphereScale,    0.05f, 1.5f },
                { "ambient",        f3->m_ambient,        0.0f,  1.0f },
                { "specular",       f3->m_specular,       0.0f,  3.0f },
                { "rim",            f3->m_rim,            0.0f,  1.0f },
                { "saturation",     f3->m_saturation,     0.0f,  2.0f },
                { "lightIntensity", f3->m_lightIntensity, 0.0f,  3.0f },
            };
        } else {
            std::cerr << "[OSC] layer/audiopreset: " << key
                      << " has no audio-bindable source (" << type << ")\n";
            return;
        }

        // A reloaded project has bindings but no recipe — adopt them first so
        // a remote knob re-scales the existing motion instead of discarding it.
        AudioPresetEngine::adoptExisting(*bindings, pp, l->id);
        AudioPresetEngine::State& st = AudioPresetEngine::stateFor(l->id);

        if (command == "intensity" && !msg.floats.empty()) {
            float v = msg.floats[0];
            st.intensity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            // Same contract as the panel slider: with nothing bound yet the
            // knob auto-picks a set, so it always DOES something.
            if (!AudioPresetEngine::rebuild(*bindings, pp, l->id))
                AudioPresetEngine::shuffle(*bindings, pp, l->id);
        } else if (command == "character" && !msg.floats.empty()) {
            float v = msg.floats[0];
            st.character = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            if (!AudioPresetEngine::rebuild(*bindings, pp, l->id))
                AudioPresetEngine::retintCharacter(*bindings, l->id);
        } else if (command == "shuffle") {
            AudioPresetEngine::shuffle(*bindings, pp, l->id);
        } else if (command == "on") {
            // Mirror of the panel's On button: restore the set off() stashed,
            // or shuffle a fresh one the first time.
            AudioPresetEngine::on(*bindings, pp, l->id);
        } else if (command == "off") {
            AudioPresetEngine::off(*bindings, l->id);
        } else {
            std::cerr << "[OSC] layer/audiopreset: unknown command " << command << std::endl;
        }
        return;
    }
    std::cerr << "[OSC] layer/audiopreset: no managed layer with key " << key << std::endl;
}

OutputZone* Application::ensureZoneByName(const std::string& name) {
    for (auto& z : m_zones) {
        if (z && z->name == name) return z.get();
    }
    auto zone = std::make_unique<OutputZone>();
    zone->name = name;
    zone->init();
    OutputZone* ptr = zone.get();
    m_zones.push_back(std::move(zone));
    return ptr;
}

void Application::ensureZoneNdi(const std::string& zoneName, const std::string& feedName) {
    if (zoneName.empty() || feedName.empty()) {
        std::cerr << "[OSC] zone/ensure ignored: empty zone or feed\n";
        return;
    }
    OutputZone* z = ensureZoneByName(zoneName);
    // A feed name is "MACHINE (sender)"; NDI re-adds the machine prefix, so the
    // local sender must be just the inner part to broadcast the exact feed name.
    std::string sender = feedName;
    size_t op = feedName.rfind('(');
    if (op != std::string::npos) {
        size_t cp = feedName.find(')', op);
        if (cp != std::string::npos && cp > op + 1)
            sender = feedName.substr(op + 1, cp - op - 1);
    }
    z->ndiStreamName = sender;
    z->rawNdiName = true;
    z->outputDest = OutputDest::NDI;
}

void Application::setZoneOutput(const std::string& zoneName, const std::string& dest,
                               int monitor, const std::string& ndiFeedName) {
    if (zoneName.empty()) {
        std::cerr << "[OSC] zone/output ignored: empty zone\n";
        return;
    }
    OutputZone* z = ensureZoneByName(zoneName);
    std::string d = dest;
    for (auto& c : d) c = (char)tolower((unsigned char)c);

    if (d == "fullscreen" || d == "screen" || d == "monitor") {
        // 0-based GLFW monitor index. The per-zone routing loop in render() opens/
        // moves the projector window next frame; ProjectorOutput::create validates
        // the index and falls back to a secondary monitor if it's the editor's, so
        // a bad index is non-fatal.
        z->outputDest = OutputDest::Fullscreen;
        z->outputMonitor = monitor;
    } else if (d == "ndi") {
        // Optional explicit feed name, unwrapped the same way as ensureZoneNdi
        // ("MACHINE (sender)" -> "sender"); else keep/derive from the zone name.
        if (!ndiFeedName.empty()) {
            std::string sender = ndiFeedName;
            size_t op = ndiFeedName.rfind('(');
            if (op != std::string::npos) {
                size_t cp = ndiFeedName.find(')', op);
                if (cp != std::string::npos && cp > op + 1)
                    sender = ndiFeedName.substr(op + 1, cp - op - 1);
            }
            z->ndiStreamName = sender;
            z->rawNdiName = true;
        } else if (z->ndiStreamName.empty()) {
            z->ndiStreamName = z->name;
        }
        z->outputDest = OutputDest::NDI;
    } else { // "none" / "preview" / unknown -> preview-only
        z->outputDest = OutputDest::None;
        z->outputMonitor = -1;
    }
    std::cerr << "[OSC] zone/output " << zoneName << " -> " << d
              << " (monitor " << z->outputMonitor << ")\n";
}

void Application::addZoneLayerByKey(const std::string& zoneName, const std::string& managedKey) {
    if (zoneName.empty() || managedKey.empty()) return;
    OutputZone* z = ensureZoneByName(zoneName);
    for (auto& l : m_layerStack.layers()) {
        if (l && l->managedKey == managedKey) {
            z->showAllLayers = false;
            z->visibleLayerIds.insert(l->id);
            return;
        }
    }
    std::cerr << "[OSC] zone/layer: no managed layer with key " << managedKey << std::endl;
}

void Application::removeZoneByName(const std::string& zoneName) {
    if (zoneName.empty()) return;
    for (size_t i = 0; i < m_zones.size(); i++) {
        if (!m_zones[i] || m_zones[i]->name != zoneName) continue;
        if (m_zones.size() > 1) {
            // Full cleanup path (projector, NDI/Spout senders, mapping
            // profile) — this used to erase directly and strand the zone's
            // MappingProfile and projector window.
            removeZone((int)i);
            if (m_activeZone >= (int)m_zones.size()) m_activeZone = (int)m_zones.size() - 1;
            if (m_activeZone < 0) m_activeZone = 0;
        } else {
            // Keep at least one zone; just stop its output.
#ifdef HAS_NDI
            if (m_zones[i]->ndiOutput.isActive()) m_zones[i]->ndiOutput.destroy();
#endif
            m_zones[i]->outputDest = OutputDest::None;
            m_zones[i]->visibleLayerIds.clear();
            m_zones[i]->showAllLayers = true;
        }
        return;
    }
}

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

// ---------------------------------------------------------------------
// gzip + base64 helpers for the Play wire payload. macOS caps UDP send
// datagrams at net.inet.udp.maxdgram (9216 bytes by default) — the M1
// publish (shader params + transitions + lanes + ndi) easily exceeds
// that, so raw JSON gets silently dropped by the kernel. We compress
// (gzip ~25% of source for repetitive JSON) and base64-encode so the
// result fits a single OSC string arg AND stays well under the cap.
// The agent + tests recognize a "z:" prefix and decompress; raw JSON
// (no prefix) stays parseable for smaller payloads / legacy listeners.
// ---------------------------------------------------------------------
// gzip compression is DISABLED in this Windows build: zlib dev libs aren't
// installed and the fork pinned no zlib dependency. Returning empty makes
// toWirePayload() fall back to sending raw JSON — a path the original code
// already supports (`if (z.empty()) return json;`). This is safe on Windows,
// whose UDP datagram cap (~64 KB) far exceeds macOS's 9216-byte cap that the
// compression was working around. To restore gzip, vendor zlib (e.g.
// FetchContent madler/zlib + link zlibstatic) and reinstate the deflate()
// body that previously lived here.
static std::string gzipCompress(const std::string& /*src*/) {
    return {};
}
static std::string b64Encode(const std::string& src) {
    static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((src.size() + 2) / 3 * 4);
    for (size_t i = 0; i < src.size(); i += 3) {
        uint32_t triple = (uint8_t)src[i] << 16;
        bool h2 = (i + 1 < src.size()), h3 = (i + 2 < src.size());
        if (h2) triple |= (uint8_t)src[i + 1] << 8;
        if (h3) triple |= (uint8_t)src[i + 2];
        out += kAlpha[(triple >> 18) & 0x3F];
        out += kAlpha[(triple >> 12) & 0x3F];
        out += h2 ? kAlpha[(triple >> 6) & 0x3F] : '=';
        out += h3 ? kAlpha[ triple       & 0x3F] : '=';
    }
    return out;
}

std::string Application::buildPlayJson() {
    // Wire shape mirrors EaselMobile/EaselMobile/Models/Play.swift exactly
    // (camelCase, layerIndex / kind / tone / clips with startSec+durationSec,
    // markers with time+name). Mobile decodes this from
    // SystemSnapshot.play via state.snapshot. See agent.py: action
    // "play.publish" — the OSC bridge below mirrors that action so Easel
    // never needs to speak the relay protocol directly.

    static const char* kTones[] = { "lime", "amber", "cyan", "magenta", "sky" };
    auto toneFor = [](int idx) {
        return kTones[(idx % 5 + 5) % 5];
    };

    auto kindForLayer = [](const std::shared_ptr<Layer>& l) -> std::string {
        // Lowercase the name once for cheap substring checks.
        std::string n = l ? l->name : std::string{};
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        if (n.find("text") != std::string::npos)  return "TEXT";
        if (n.find("theme") != std::string::npos) return "THEME";
        if (l && l->source && l->source->isShader()) return "SHADER";
        return "FX";
    };

    // Stable per-process show id. Allocated once on first publish so the
    // M3 dirty-check (byte-compare JSON) doesn't trip on a moving id and
    // re-publish every frame. Mobile uses id only for identity; content
    // changes flow through everything else.
    if (m_showIdStr.empty()) {
        m_showIdStr = std::string("easel.show.")
                    + std::to_string((long)glfwGetTime());
    }
    json play;
    play["id"] = m_showIdStr;
    play["name"] = "Easel Show";
    play["bpm"] = (double)m_bpmSync.bpm();
    play["playhead"] = (double)m_timeline.playhead();
    play["duration"] = (double)m_timeline.duration();
    // M4 — surface the live cue text so mobile (and the test harness)
    // can mirror what voice-native shaders are rendering on the desktop
    // canvas. Additive on the wire — older mobile decoders ignore it.
    play["cueLatest"] = m_dataBus.get("cue.latest");

    // Recording / RTMP status for the /easel/record + /easel/rtmp OSC
    // surface. Stable fields only (no uptime/dropped-frame counters): they
    // change continuously and would defeat the 300ms byte-compare dirty
    // check, turning the publish stream into a constant re-send.
    {
        json record;
#ifdef HAS_FFMPEG
        record["active"] = m_recorder.isActive();
        record["path"]   = m_recorder.isActive() ? m_recorder.filePath() : "";
#else
        record["active"] = false;
        record["path"]   = "";
#endif
        play["record"] = record;
        json rtmp;
#ifdef HAS_FFMPEG
        rtmp["active"] = m_rtmpOutput.isActive();
#else
        rtmp["active"] = false;
#endif
        play["rtmp"] = rtmp;
    }

    // Helper: extract the file basename from a shader path so the wire
    // payload stays portable across machines (no /Users/lu/... prefixes).
    auto basename = [](const std::string& p) -> std::string {
        size_t slash = p.find_last_of("/\\");
        return (slash == std::string::npos) ? p : p.substr(slash + 1);
    };

    json layersJson = json::array();
    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& lp = m_layerStack[i];
        if (!lp) continue;
        json layerJson;
        layerJson["id"] = std::to_string(lp->id);
        layerJson["layerIndex"] = i;
        layerJson["name"] = lp->name;
        layerJson["kind"] = kindForLayer(lp);
        layerJson["visible"] = lp->visible && !lp->userHidden;
        layerJson["tone"] = toneFor(i);

        // M1 — shader path (basename only — mobile resolves via its
        // own ShaderCatalog/bundle; the desktop path is per-machine).
        layerJson["shaderPath"] = "";
        if (lp->source && lp->source->isShader()) {
            auto* shader = static_cast<ShaderSource*>(lp->source.get());
            layerJson["shaderPath"] = basename(shader->sourcePath());
        }

        // M1 — opacity (separate from visible: lets mobile crossfade).
        layerJson["opacity"] = (double)lp->opacity;

        // M1 — blendMode by name so mobile can render it without
        // mapping numeric enums across language boundaries.
        layerJson["blendMode"] = std::string(blendModeName(lp->blendMode));

        // M1 — transform (pos, scale, rotation in degrees).
        json transform;
        transform["position"] = { (double)lp->position.x, (double)lp->position.y };
        transform["scale"]    = { (double)lp->scale.x,    (double)lp->scale.y    };
        transform["rotation"] = (double)lp->rotation;
        layerJson["transform"] = transform;

        // M1 — shader parameter snapshot. Only currently-set values
        // (skipping defaults would lose live state). Floats / bools /
        // points (as 2-array) / text (as string). Colors collapse to a
        // 4-array. Image inputs are omitted — they're texture pointers,
        // not portable values.
        json paramsJson = json::object();
        if (lp->source && lp->source->isShader()) {
            auto* shader = static_cast<ShaderSource*>(lp->source.get());
            for (const auto& inp : shader->inputs()) {
                if (inp.type == "float") {
                    paramsJson[inp.name] = (double)std::get<float>(inp.value);
                } else if (inp.type == "bool") {
                    paramsJson[inp.name] = std::get<bool>(inp.value);
                } else if (inp.type == "text") {
                    paramsJson[inp.name] = std::get<std::string>(inp.value);
                } else if (inp.type == "point2D") {
                    const auto& v = std::get<glm::vec2>(inp.value);
                    paramsJson[inp.name] = { (double)v.x, (double)v.y };
                } else if (inp.type == "color") {
                    const auto& c = std::get<glm::vec4>(inp.value);
                    paramsJson[inp.name] = { (double)c.r, (double)c.g,
                                             (double)c.b, (double)c.a };
                }
                // image inputs intentionally skipped (texture handle).
            }
        }
        layerJson["parameters"] = paramsJson;

        // Clips: pull from the matching Timeline track if present, else
        // synthesize a single full-duration clip so the mobile timeline
        // has something to display while the artist hasn't authored clips.
        json clipsJson = json::array();
        const TimelineTrack* track = nullptr;
        for (const auto& t : m_timeline.tracks()) {
            if (t.layerId == lp->id) { track = &t; break; }
        }
        if (track && !track->clips.empty()) {
            for (const auto& c : track->clips) {
                json cj;
                cj["id"] = std::to_string(c.id);
                cj["name"] = c.name.empty() ? lp->name : c.name;
                cj["startSec"] = c.startTime;
                cj["durationSec"] = c.duration;
                // M1 — clip-level transition / playback metadata.
                cj["sourcePath"]            = basename(c.sourcePath);
                cj["transitionInName"]      = c.transitionInName;
                cj["transitionInShaderPath"] = basename(c.transitionInShaderPath);
                cj["transitionInDuration"]  = c.transitionInDuration;
                cj["playbackMode"]          = std::string(clipPlaybackModeName(c.playbackMode));
                clipsJson.push_back(cj);
            }
        } else {
            json cj;
            cj["id"] = std::string("auto.") + std::to_string(lp->id);
            cj["name"] = lp->name;
            cj["startSec"] = 0.0;
            cj["durationSec"] = m_timeline.duration();
            // Defaults for the synthetic clip so the field set matches
            // authored clips and mobile decoders don't need null-checks.
            cj["sourcePath"]            = "";
            cj["transitionInName"]      = "";
            cj["transitionInShaderPath"] = "";
            cj["transitionInDuration"]  = 0.0;
            cj["playbackMode"]          = std::string("Forward");
            clipsJson.push_back(cj);
        }
        layerJson["clips"] = clipsJson;
        layersJson.push_back(layerJson);
    }
    play["layers"] = layersJson;

    // M1 — between-layer transitions (crossfades).
    json transitionsJson = json::array();
    for (const auto& tr : m_timeline.transitions()) {
        json tj;
        tj["id"]           = std::to_string(tr.id);
        tj["fromLayerId"]  = std::to_string(tr.fromLayerId);
        tj["toLayerId"]    = std::to_string(tr.toLayerId);
        tj["startSec"]     = tr.startTime;
        tj["durationSec"]  = tr.duration;
        tj["name"]         = tr.name;
        tj["shaderPath"]   = basename(tr.shaderPath);
        transitionsJson.push_back(tj);
    }
    play["transitions"] = transitionsJson;

    // M1 — named sections (verse / chorus / drop).
    json sectionsJson = json::array();
    for (const auto& s : m_timeline.sections()) {
        json sj;
        sj["id"]        = std::to_string(s.id);
        sj["name"]      = s.name;
        sj["startSec"]  = s.startTime;
        sj["endSec"]    = s.endTime;
        sectionsJson.push_back(sj);
    }
    play["sections"] = sectionsJson;

    // M1 — automation / MIDI / audio-reactive lanes. Keyframe data
    // travels along so mobile can scrub animated params correctly.
    json lanesJson = json::array();
    for (const auto& ln : m_timeline.lanes()) {
        json lj;
        lj["id"]           = std::to_string(ln.id);
        lj["layerId"]      = std::to_string(ln.layerId);
        lj["kind"]         = std::string(timelineLaneKindName(ln.kind));
        lj["paramName"]    = ln.paramName;
        lj["midiChannel"]  = ln.midiChannel;
        lj["audioSignal"]  = ln.audioSignal;
        lj["audioStrength"] = (double)ln.audioStrength;
        json pointsJson = json::array();
        for (const auto& p : ln.points) {
            json pj;
            pj["time"]      = p.time;
            pj["value"]     = (double)p.value;
            pj["noteOrCC"]  = p.noteOrCC;
            pointsJson.push_back(pj);
        }
        lj["points"] = pointsJson;
        lanesJson.push_back(lj);
    }
    play["lanes"] = lanesJson;

    // Easel's actual output zones — what the user configured in the
    // desktop's Zones picker. Mobile shows these in the Zones section
    // of the ParameterSheet so the iPhone reflects the show's true
    // output destinations (vs the agent's hard-coded ZONES constant).
    json easelZonesJson = json::array();
    for (size_t zi = 0; zi < m_zones.size(); zi++) {
        auto& zp = m_zones[zi];
        if (!zp) continue;
        json zj;
        zj["index"] = (int)zi;
        zj["name"]  = zp->name;
        zj["width"] = zp->width;
        zj["height"] = zp->height;
        zj["showAllLayers"] = zp->showAllLayers;
        // List of layer IDs visible in this zone (when showAllLayers
        // is false). Mobile toggles `zone.visibility` for finer control.
        json visIds = json::array();
        for (uint32_t lid : zp->visibleLayerIds) {
            visIds.push_back(std::to_string(lid));
        }
        zj["visibleLayerIds"] = visIds;
        // Output destination string — mobile shows a badge.
        switch (zp->outputDest) {
            case OutputDest::None:       zj["outputDest"] = "None";       break;
            case OutputDest::Fullscreen: zj["outputDest"] = "Fullscreen"; break;
            case OutputDest::NDI:        zj["outputDest"] = "NDI";        break;
            case OutputDest::Spout:      zj["outputDest"] = "Spout";      break;
        }
        zj["isActive"] = ((int)zi == m_activeZone);
        // Per-zone mic — mobile's PTT rail reads these live (enabled = the
        // zone is configured for its own mic; pushToTalkActive = currently held).
        zj["micEnabled"] = zp->micEnabled;
        zj["pushToTalkActive"] = zp->pushToTalkActive;
        easelZonesJson.push_back(zj);
    }
    play["easelZones"] = easelZonesJson;

    // M5 — NDI discovery state. Mobile reads this to surface live NDI
    // senders (incl. an iPhone running an NDI sender app like NDI HX
    // Camera) and offer them as ingest options. When HAS_NDI is undefined
    // at build time, runtimeAvailable=false and sources stays empty —
    // mobile then knows NDI isn't an option on this Easel build.
    json ndi = json::object();
#ifdef HAS_NDI
    ndi["runtimeAvailable"] = NDIRuntime::instance().isAvailable();
    json ndiSrcs = json::array();
    for (const auto& s : m_ndiSources) {
        json sj;
        sj["name"] = s.name;
        sj["url"]  = s.url;
        // Heuristic: NDI senders coming from iOS NDI apps tend to label
        // the device name in the sender string. Mobile uses this hint to
        // badge "iPhone" entries in the discovery picker.
        std::string lname = s.name;
        for (auto& c : lname) c = (char)tolower((unsigned char)c);
        sj["isIphone"] = (lname.find("iphone") != std::string::npos
                       || lname.find("ios") != std::string::npos);
        ndiSrcs.push_back(sj);
    }
    ndi["discoveredSources"] = ndiSrcs;
#else
    ndi["runtimeAvailable"]  = false;
    ndi["discoveredSources"] = json::array();
#endif
    play["ndi"] = ndi;

    json markersJson = json::array();
    for (const auto& m : m_timeline.markers()) {
        json mj;
        mj["id"] = std::to_string(m.id);
        mj["time"] = m.time;
        mj["name"] = m.name;
        markersJson.push_back(mj);
    }
    play["markers"] = markersJson;

    // Use the `replace` error handler — invalid UTF-8 bytes that find
    // their way in via speech recognizer text, OS-level filenames, or
    // arbitrary user input get substituted with U+FFFD instead of
    // throwing std::invalid_argument. Throwing here aborts the whole
    // process because publishPlayIfChanged runs from the main render
    // loop with no enclosing try/catch (verified by crash logs:
    // nlohmann::serializer::dump_escaped → __cxa_throw → abort).
    return play.dump(-1, ' ', false,
                      nlohmann::json::error_handler_t::replace);
}

// Helper — package a JSON string for the wire. Always emits a "z:" +
// base64(gzip(payload)) string so we never run afoul of macOS's
// net.inet.udp.maxdgram cap. Agent + tests decode by stripping the
// "z:" prefix.
static std::string toWirePayload(const std::string& json) {
    std::string z = gzipCompress(json);
    if (z.empty()) return json;  // gzip failure — fall back to raw
    return std::string("z:") + b64Encode(z);
}

void Application::publishPlayToAgent() {
    // Always sends — used by the UI PUBLISH button, File menu, and the
    // /easel/play/publish OSC trigger. The dirty-check loop calls
    // publishPlayIfChanged() instead.
    try {
        std::string payload = buildPlayJson();
        std::string wire = toWirePayload(payload);
        m_oscManager.sendString("/agent/play/publish", wire);
        m_lastPublishedJson = payload;
        std::cout << "[Easel] Published Play (" << m_layerStack.count()
                  << " layers, " << m_timeline.markers().size()
                  << " markers, " << payload.size() << " bytes JSON / "
                  << wire.size() << " on wire)" << std::endl;
    } catch (const std::exception& exc) {
        std::cerr << "[Easel] publishPlayToAgent exception: "
                  << exc.what() << std::endl;
    } catch (...) {
        std::cerr << "[Easel] publishPlayToAgent unknown exception"
                  << std::endl;
    }
}

void Application::publishPlayIfChanged() {
    // M3 — Live re-publish: built once every ~300ms and only sent over
    // OSC when the produced JSON differs from the last send. Mobile thus
    // mirrors any edit (layer add/remove, opacity slider, clip move, bpm
    // change, marker add, transport advancing) within ~500ms with no
    // manual PUBLISH click.
    //
    // Playhead participates in the diff so mobile's scrub indicator
    // mirrors live during desktop playback. At 300ms cadence + ~3KB JSON
    // gzipped to ~1KB on the wire, that's a sub-4KB/s loopback stream
    // when the timeline's actually running.
    //
    // Defensive belt: the publish path is called from the render loop,
    // so any exception (nlohmann::json::dump on invalid UTF-8, std::bad_alloc
    // on a runaway payload, OSC sendto failure) would terminate the app.
    // Catch + log + skip this tick — we'll try again in 300ms.
    try {
        std::string payload = buildPlayJson();
        if (payload == m_lastPublishedJson) return;
        m_oscManager.sendString("/agent/play/publish",
                                  toWirePayload(payload));
        m_lastPublishedJson = payload;
    } catch (const std::exception& exc) {
        std::cerr << "[Easel] publishPlayIfChanged exception: "
                  << exc.what() << " — skipping tick" << std::endl;
    } catch (...) {
        std::cerr << "[Easel] publishPlayIfChanged unknown exception"
                  << " — skipping tick" << std::endl;
    }
}

#ifdef HAS_NDI
// Apply the current NDI network selection: write ndi-config.v1.json (+ set
// NDI_CONFIG_DIR) and, when requested, re-initialize the NDI runtime so a new
// adapter pin / discovery setting takes effect (the SDK reads its config only at
// initialize() time). reinit=false just persists for the next launch.
void Application::applyNdiNetworkSettings(bool reinit) {
    NDIRuntime::setPendingNetworkSettings(m_ndiNetwork);
    if (reinit && NDIRuntime::instance().isAvailable()) {
        // Destroy sender + finder BEFORE the runtime so we never touch a stale
        // api() pointer, then recreate them against the re-initialized runtime.
        m_ndiOutput.destroy();
        m_ndiFinder.destroy();
        NDIRuntime::instance().reinitWithSettings(m_ndiNetwork);
        if (NDIRuntime::instance().isAvailable()) {
            m_ndiFinder.create();
            m_ndiSources = m_ndiFinder.sources();
            if (m_ndiOutputEnabled) m_ndiOutput.create("Lu");
        }
    } else {
        // No runtime (or reinit not requested): persist config for the next launch.
        NdiNetworkConfig::applyToEnv(m_ndiNetwork);
    }
    m_netAdapters.clear();            // force NIC re-enumeration on next panel draw
    m_ndiPeerStatusLastRefresh = 0.0; // force peer re-classification
}

// Re-enumerate NICs and classify each discovered NDI peer (same-subnet + TCP
// reachability). Blocking probes — call on demand / throttled, never per frame.
void Application::refreshNdiPeerStatus() {
    m_netAdapters = EnumerateNetworkAdapters();

    // Active NIC ip/mask for same-subnet math: the pinned adapter when set, else
    // the first Wi-Fi/Ethernet adapter as a best-effort default.
    std::string activeIp, activeMask;
    if (m_ndiNetwork.enabled && !m_ndiNetwork.interfaceIp.empty()) {
        activeIp = m_ndiNetwork.interfaceIp;
        for (const auto& a : m_netAdapters)
            if (a.ipv4 == m_ndiNetwork.interfaceIp) { activeMask = a.subnetMask; break; }
    }
    if (activeIp.empty()) {
        for (const auto& a : m_netAdapters)
            if (a.kind == NetAdapterInfo::Kind::Ethernet || a.kind == NetAdapterInfo::Kind::WiFi) {
                activeIp = a.ipv4; activeMask = a.subnetMask; break;
            }
    }

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(m_ndiSources.size());
    for (const auto& s : m_ndiSources) pairs.emplace_back(s.name, s.url);
    m_ndiPeerStatus = NdiNetworkConfig::classifyPeers(pairs, activeIp, activeMask);
    m_ndiPeerStatusLastRefresh = glfwGetTime();
}
#endif // HAS_NDI

void Application::saveProject(const std::string& path) {
    json j;
    j["version"] = 2;

#ifdef HAS_NDI
    // NDI network selection — additive; older projects omit it and load as Auto.
    j["ndiNetwork"] = {
        {"enabled", m_ndiNetwork.enabled},
        {"interface", m_ndiNetwork.interfaceName},
        {"interfaceIp", m_ndiNetwork.interfaceIp},
        {"extraIps", m_ndiNetwork.extraIps},
        {"useDiscoveryServer", m_ndiNetwork.useDiscoveryServer},
        {"discoveryServer", m_ndiNetwork.discoveryServer},
    };
#endif

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
        json meshCorners = json::array();
        for (uint8_t c : m.meshWarp.corners()) meshCorners.push_back((int)c);
        mj["meshWarp"]["corners"] = meshCorners;   // per-point straight/curve flag

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
        zj["rawNdiName"] = z.rawNdiName;

        // Per-zone mic config (push-to-talk activity itself is transient
        // runtime state and intentionally not persisted).
        zj["micDeviceId"] = z.micDeviceId;
        zj["micEnabled"] = z.micEnabled;

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
        if (!layer->managedKey.empty()) layerJson["managedKey"] = layer->managedKey;
#ifdef HAS_NDI
        layerJson["ndiEnabled"] = layer->ndiEnabled;
#endif

        if (layer->source) {
            layerJson["sourceType"] = layer->source->typeName();
            layerJson["sourcePath"] = layer->source->sourcePath();
#ifdef HAS_NDI
            if (layer->source->typeName() == "NDI") {
                auto* ndiSrc = dynamic_cast<NDISource*>(layer->source.get());
                if (ndiSrc && !ndiSrc->sourceUrl().empty()) {
                    layerJson["sourceUrl"] = ndiSrc->sourceUrl();
                }
            }
#endif

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
                        if (ab.character != 0.0f) abj["character"] = ab.character;
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
            } else if (layer->source->typeName() == "Fluid") {
                // Persist the fluid config + audio bindings (same JSON shape
                // as shader audioBindings). Fluid layers are recreated from
                // scratch on load, so without this they'd revert to defaults.
                auto* f = static_cast<FluidSource*>(layer->source.get());
                json fc;
                fc["curl"]                = f->m_curlAmount;
                fc["densityDissipation"]  = f->m_densityDissipation;
                fc["velocityDissipation"] = f->m_velocityDissipation;
                fc["pressure"]            = f->m_pressureValue;
                fc["pressureIters"]       = f->m_pressureIters;
                fc["splatRadius"]         = f->m_splatRadius;
                fc["splatIntensity"]      = f->m_splatIntensity;
                fc["palette"]             = f->m_palette;
                {
                    json stops = json::array();
                    for (int i = 0; i < 4; ++i)
                        stops.push_back({ f->m_customStops[i][0],
                                          f->m_customStops[i][1],
                                          f->m_customStops[i][2] });
                    fc["customStops"] = stops;
                }
                fc["autoRate"]            = f->m_autoRate;
                fc["autoMovement"]        = f->m_autoMovement;
                fc["autoPattern"]         = f->m_autoPattern;
                fc["autoSpeed"]           = f->m_autoSpeed;
                fc["autoScale"]           = f->m_autoScale;
                fc["shading"]             = f->m_shading;
                fc["hdrDye"]              = f->m_hdrDye;
                fc["bloom"]               = f->m_bloom;
                fc["bloomIntensity"]      = f->m_bloomIntensity;
                fc["bloomThreshold"]      = f->m_bloomThreshold;
                fc["bloomSoftKnee"]       = f->m_bloomSoftKnee;
                fc["sunrays"]             = f->m_sunrays;
                fc["sunraysWeight"]       = f->m_sunraysWeight;
                // Image inject — toggle, strength, and the layer-id the
                // user picked. Resolved back to a live texture each frame
                // by the Fluid branch in the main loop.
                fc["imageEnabled"]        = f->m_imageEnabled;
                fc["imageIntensity"]      = f->m_imageIntensity;
                fc["imageReform"]         = f->m_imageReform;
                fc["reformRate"]          = f->m_reformRate;
                fc["imageSourceLayerId"]  = f->imageSource().sourceLayerId;
                layerJson["fluidConfig"] = fc;

                const auto& audioBinds = f->audioBindings();
                if (!audioBinds.empty()) {
                    json abJson = json::array();
                    for (const auto& [name, ab] : audioBinds) {
                        if (ab.signal == AudioSignal::None) continue;
                        json abj;
                        abj["param"]     = name;
                        abj["signal"]    = (int)ab.signal;
                        abj["rangeMin"]  = ab.rangeMin;
                        abj["rangeMax"]  = ab.rangeMax;
                        abj["smoothing"] = ab.smoothing;
                        if (ab.character != 0.0f) abj["character"] = ab.character;
                        if (ab.signal == AudioSignal::MidiCC) {
                            abj["midiCC"]      = ab.midiCC;
                            abj["midiChannel"] = ab.midiChannel;
                        }
                        abJson.push_back(abj);
                    }
                    if (!abJson.empty()) layerJson["audioBindings"] = abJson;
                }
            } else if (layer->source->typeName() == "Fluid3D") {
                // Native 3D SPH fluid — recreated from scratch on load, so
                // persist the render look + audio bindings. Separate config
                // key from "fluidConfig" to avoid collision with 2D Fluid.
                auto* f = static_cast<FluidSource3D*>(layer->source.get());
                json fc;
                fc["deepColor"]   = { f->m_deepColor[0], f->m_deepColor[1], f->m_deepColor[2] };
                fc["glowColor"]   = { f->m_glowColor[0], f->m_glowColor[1], f->m_glowColor[2] };
                fc["brightness"]  = f->m_brightness;
                fc["shallowColor"]= { f->m_shallowColor[0], f->m_shallowColor[1], f->m_shallowColor[2] };
                fc["lightDir"]    = { f->m_lightDir[0], f->m_lightDir[1], f->m_lightDir[2] };
                fc["lightIntensity"] = f->m_lightIntensity;
                fc["ambient"]     = f->m_ambient;
                fc["specular"]    = f->m_specular;
                fc["rim"]         = f->m_rim;
                fc["saturation"]  = f->m_saturation;
                fc["bgTop"]    = { f->m_bgTop[0], f->m_bgTop[1], f->m_bgTop[2] };
                fc["bgBottom"] = { f->m_bgBottom[0], f->m_bgBottom[1], f->m_bgBottom[2] };
                fc["bgAlpha"]  = f->m_bgAlpha;
                fc["sphereScale"] = f->m_sphereScale;
                fc["zoom"]        = f->m_zoom;
                fc["audioIntensity"] = f->m_audioIntensity;
                fc["gravity"]    = f->m_gravity;
                fc["vortex"]     = f->m_vortex;
                fc["turbulence"] = f->m_turbulence;
                fc["forceScale"] = f->m_forceScale;
                fc["sphereShape"] = f->m_sphereShape;
                fc["fillAmount"]  = f->m_fillAmount;
                fc["particleCube"] = f->m_particleCube;
                // VJ morph (Low/High look snapshots + drive config).
                fc["vjMode"]         = f->m_vjMode;
                fc["vjGrab"]         = f->m_vjGrab;
                fc["journeySignal"]  = f->m_journeySignal;
                fc["journeyGain"]    = f->m_journeyGain;
                fc["journeyPosManual"] = f->m_journeyPosManual;
                if (f->m_lookSet[0]) {
                    float a[FluidSource3D::kLookFloats]; f->lookToArray(0, a);
                    fc["lookLow"] = std::vector<float>(a, a + FluidSource3D::kLookFloats);
                }
                if (f->m_lookSet[1]) {
                    float a[FluidSource3D::kLookFloats]; f->lookToArray(1, a);
                    fc["lookHigh"] = std::vector<float>(a, a + FluidSource3D::kLookFloats);
                }
                fc["autoRotate"]  = f->m_autoRotate;
                fc["rotateSpeed"] = f->m_rotateSpeed;
                fc["tilt"]        = f->m_tilt;
                fc["simRes"]      = f->m_simRes;
                fc["substeps"]    = f->m_substeps;
                fc["renderScale"] = f->m_renderScale;
                fc["imageEnabled"]       = f->m_imageEnabled;
                fc["imageMix"]           = f->m_imageMix;
                fc["imageSourceLayerId"] = f->imageSource().sourceLayerId;
                layerJson["fluid3dConfig"] = fc;

                const auto& audioBinds = f->audioBindings();
                if (!audioBinds.empty()) {
                    json abJson = json::array();
                    for (const auto& [name, ab] : audioBinds) {
                        if (ab.signal == AudioSignal::None) continue;
                        json abj;
                        abj["param"]     = name;
                        abj["signal"]    = (int)ab.signal;
                        abj["rangeMin"]  = ab.rangeMin;
                        abj["rangeMax"]  = ab.rangeMax;
                        abj["smoothing"] = ab.smoothing;
                        if (ab.character != 0.0f) abj["character"] = ab.character;
                        if (ab.signal == AudioSignal::MidiCC) {
                            abj["midiCC"]      = ab.midiCC;
                            abj["midiChannel"] = ab.midiChannel;
                        }
                        abJson.push_back(abj);
                    }
                    if (!abJson.empty()) layerJson["audioBindings"] = abJson;
                }
            } else if (layer->source->typeName() == "Hologram Model") {
                // sourcePath already holds the model file; persist the look.
                auto* hm = static_cast<HologramModelSource*>(layer->source.get());
                const auto& P = hm->params();
                json hc;
                hc["rotateSpeed"]  = P.rotateSpeed;
                hc["modelScale"]   = P.modelScale;
                hc["wireBright"]   = P.wireBright;
                hc["surfaceFill"]  = P.surfaceFill;
                hc["scanSpeed"]    = P.scanSpeed;
                hc["interference"] = P.interference;
                hc["chromaShift"]  = P.chromaShift;
                hc["beamHaze"]     = P.beamHaze;
                hc["audioReact"]   = P.audioReact;
                layerJson["hologramConfig"] = hc;
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

        // Save per-layer effects chain (all params; reader fills any missing
        // field from the LayerEffect struct defaults, so this is forward/back
        // compatible as new effect types are added).
        if (!layer->effects.empty()) {
            json fxArr = json::array();
            for (const auto& fx : layer->effects) {
                json fj;
                fj["type"]          = (int)fx.type;
                fj["enabled"]       = fx.enabled;
                fj["blurRadius"]    = fx.blurRadius;
                fj["brightness"]    = fx.brightness;
                fj["contrast"]      = fx.contrast;
                fj["saturation"]    = fx.saturation;
                fj["hueShift"]      = fx.hueShift;
                fj["pixelSize"]     = fx.pixelSize;
                fj["feedbackMix"]   = fx.feedbackMix;
                fj["feedbackZoom"]  = fx.feedbackZoom;
                fj["glowThreshold"] = fx.glowThreshold;
                fj["glowRadius"]    = fx.glowRadius;
                fj["glowIntensity"] = fx.glowIntensity;
                fj["sharpenAmount"] = fx.sharpenAmount;
                fj["sharpenRadius"] = fx.sharpenRadius;
                fj["audioSignal"]   = fx.audioSignal;
                fj["audioAmount"]   = fx.audioAmount;
                fxArr.push_back(fj);
            }
            layerJson["effects"] = fxArr;
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

    // Write atomically: temp + rename. This file is autosaved constantly and
    // read concurrently by the SDK's probes and heals; a bare truncate-then-
    // write hands readers torn JSON, and a crash mid-save loses the whole
    // project (2026-07-17). rename() replaces the destination in one step.
    const std::string tmpPath = path + ".tmp";
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        // error_handler_t::replace — never abort the app on a stray
        // non-UTF-8 byte in a layer name / msg / path; substitute U+FFFD.
        file << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        file.flush();
        file.close();
        std::error_code ec;
        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            std::cerr << "Failed to move saved project into place: " << path
                      << " (" << ec.message() << ")" << std::endl;
        } else {
            std::cout << "Project saved: " << path << std::endl;
        }
    } else {
        std::cerr << "Failed to save project: " << tmpPath << std::endl;
    }
}

void Application::loadProject(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open project: " << path << std::endl;
        return;
    }
    addRecentProject(path);

    json j;
    try {
        file >> j;
    } catch (const json::exception& e) {
        std::cerr << "Failed to parse project: " << e.what() << std::endl;
        return;
    }

#ifdef HAS_NDI
    // NDI network selection (additive — absent in older projects → stays Auto).
    if (j.contains("ndiNetwork")) {
        const auto& n = j["ndiNetwork"];
        m_ndiNetwork.enabled            = n.value("enabled", false);
        m_ndiNetwork.interfaceName      = n.value("interface", std::string());
        m_ndiNetwork.interfaceIp        = n.value("interfaceIp", std::string());
        m_ndiNetwork.extraIps           = n.value("extraIps", std::string());
        m_ndiNetwork.useDiscoveryServer = n.value("useDiscoveryServer", false);
        m_ndiNetwork.discoveryServer    = n.value("discoveryServer", std::string());
        applyNdiNetworkSettings(/*reinit=*/true);
    }
#endif

    // Clear current state
    while (m_layerStack.count() > 0) {
        uint32_t rid = m_layerStack[0] ? m_layerStack[0]->id : 0;
        m_layerStack.removeLayer(0);
        if (rid) m_timeline.removeTrackForLayer(rid);
    }
    m_selectedLayer = -1;

    // Reset the editor viewport on every project load so the user always
    // sees a centered, 1:1 canvas — never inherits a stale pan/zoom from
    // a previous project that was navigated off-screen.
    m_viewportPanel.resetZoom();

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
            if (mj["meshWarp"].contains("corners")) {
                auto& corners = m.meshWarp.corners();
                const auto& cj = mj["meshWarp"]["corners"];
                for (int i = 0; i < (int)cj.size() && i < (int)corners.size(); i++) {
                    corners[i] = (uint8_t)cj[i].get<int>();
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
            z->rawNdiName = zj.value("rawNdiName", false);

            z->micDeviceId = zj.value("micDeviceId", std::string(""));
            z->micEnabled = zj.value("micEnabled", false);
            // pushToTalkActive is transient runtime state — never loaded from disk.

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
            layer->managedKey = layerJson.value("managedKey", std::string{});
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
            // Restore per-layer effects chain (each field defaults to the
            // LayerEffect struct value when absent in older saves).
            if (layerJson.contains("effects")) {
                for (const auto& fj : layerJson["effects"]) {
                    LayerEffect fx;
                    fx.type          = (EffectType)fj.value("type", 0);
                    fx.enabled       = fj.value("enabled", fx.enabled);
                    fx.blurRadius    = fj.value("blurRadius", fx.blurRadius);
                    fx.brightness    = fj.value("brightness", fx.brightness);
                    fx.contrast      = fj.value("contrast", fx.contrast);
                    fx.saturation    = fj.value("saturation", fx.saturation);
                    fx.hueShift      = fj.value("hueShift", fx.hueShift);
                    fx.pixelSize     = fj.value("pixelSize", fx.pixelSize);
                    fx.feedbackMix   = fj.value("feedbackMix", fx.feedbackMix);
                    fx.feedbackZoom  = fj.value("feedbackZoom", fx.feedbackZoom);
                    fx.glowThreshold = fj.value("glowThreshold", fx.glowThreshold);
                    fx.glowRadius    = fj.value("glowRadius", fx.glowRadius);
                    fx.glowIntensity = fj.value("glowIntensity", fx.glowIntensity);
                    fx.sharpenAmount = fj.value("sharpenAmount", fx.sharpenAmount);
                    fx.sharpenRadius = fj.value("sharpenRadius", fx.sharpenRadius);
                    fx.audioSignal   = fj.value("audioSignal", fx.audioSignal);
                    fx.audioAmount   = fj.value("audioAmount", fx.audioAmount);
                    layer->effects.push_back(fx);
                }
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
                            // Missing in pre-smoothing-upgrade projects → use
                            // the new gentler default (struct default 0.55).
                            ab.smoothing = abj.value("smoothing", 0.55f);
                            ab.character = abj.value("character", 0.0f);
                            ab.midiCC = abj.value("midiCC", -1);
                            ab.midiChannel = abj.value("midiChannel", -1);
                            src->audioBindings()[abj.value("param", "")] = ab;
                        }
                    }
                    layer->source = src;
                }
            } else if (sourceType == "Fluid") {
                // Rebuild the native fluid sim and restore its config + audio
                // bindings. Config is applied BEFORE init() so init-time
                // choices (m_hdrDye → dye buffer format) honor the saved value.
                auto src = std::make_shared<FluidSource>();
                if (layerJson.contains("fluidConfig")) {
                    const auto& fc = layerJson["fluidConfig"];
                    src->m_curlAmount          = fc.value("curl", src->m_curlAmount);
                    src->m_densityDissipation  = fc.value("densityDissipation", src->m_densityDissipation);
                    src->m_velocityDissipation = fc.value("velocityDissipation", src->m_velocityDissipation);
                    src->m_pressureValue       = fc.value("pressure", src->m_pressureValue);
                    src->m_pressureIters       = fc.value("pressureIters", src->m_pressureIters);
                    src->m_splatRadius         = fc.value("splatRadius", src->m_splatRadius);
                    src->m_splatIntensity      = fc.value("splatIntensity", src->m_splatIntensity);
                    src->m_palette             = fc.value("palette", src->m_palette);
                    if (fc.contains("customStops") && fc["customStops"].is_array()) {
                        const auto& stops = fc["customStops"];
                        for (int i = 0; i < 4 && i < (int)stops.size(); ++i)
                            for (int c = 0; c < 3 && c < (int)stops[i].size(); ++c)
                                src->m_customStops[i][c] = stops[i][c].get<float>();
                    }
                    src->m_autoRate            = fc.value("autoRate", src->m_autoRate);
                    src->m_autoMovement        = fc.value("autoMovement", src->m_autoMovement);
                    src->m_autoPattern         = fc.value("autoPattern", src->m_autoPattern);
                    src->m_autoSpeed           = fc.value("autoSpeed", src->m_autoSpeed);
                    src->m_autoScale           = fc.value("autoScale", src->m_autoScale);
                    src->m_shading             = fc.value("shading", src->m_shading);
                    src->m_hdrDye              = fc.value("hdrDye", src->m_hdrDye);
                    src->m_bloom               = fc.value("bloom", src->m_bloom);
                    src->m_bloomIntensity      = fc.value("bloomIntensity", src->m_bloomIntensity);
                    src->m_bloomThreshold      = fc.value("bloomThreshold", src->m_bloomThreshold);
                    src->m_bloomSoftKnee       = fc.value("bloomSoftKnee", src->m_bloomSoftKnee);
                    src->m_sunrays             = fc.value("sunrays", src->m_sunrays);
                    src->m_sunraysWeight       = fc.value("sunraysWeight", src->m_sunraysWeight);
                    // Image inject — restore enable + strength + bound
                    // layer id (texture is rebound each frame from the
                    // layer id by the Fluid branch in the main loop).
                    src->m_imageEnabled        = fc.value("imageEnabled",   src->m_imageEnabled);
                    src->m_imageIntensity      = fc.value("imageIntensity", src->m_imageIntensity);
                    src->m_imageReform         = fc.value("imageReform",    src->m_imageReform);
                    src->m_reformRate          = fc.value("reformRate",     src->m_reformRate);
                    src->imageSource().sourceLayerId =
                        fc.value("imageSourceLayerId",
                                 (uint32_t)src->imageSource().sourceLayerId);
                }
                int fw = 1280, fh = 720;
                if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() &&
                    m_zones[m_activeZone]) {
                    fw = m_zones[m_activeZone]->width;
                    fh = m_zones[m_activeZone]->height;
                }
                if (src->init(fw, fh)) {
                    if (layerJson.contains("audioBindings")) {
                        for (const auto& abj : layerJson["audioBindings"]) {
                            AudioBinding ab;
                            ab.signal = (AudioSignal)abj.value("signal", 0);
                            ab.rangeMin = abj.value("rangeMin", 0.0f);
                            ab.rangeMax = abj.value("rangeMax", 1.0f);
                            ab.smoothing = abj.value("smoothing", 0.55f);
                            ab.character = abj.value("character", 0.0f);
                            ab.midiCC = abj.value("midiCC", -1);
                            ab.midiChannel = abj.value("midiChannel", -1);
                            src->audioBindings()[abj.value("param", "")] = ab;
                        }
                    }
                    layer->source = src;
                }
            } else if (sourceType == "Fluid3D") {
                // Rebuild the native 3D SPH fluid. simRes is applied BEFORE
                // init() since it sizes the volumes.
                auto src = std::make_shared<FluidSource3D>();
                if (layerJson.contains("fluid3dConfig")) {
                    const auto& fc = layerJson["fluid3dConfig"];
                    if (fc.contains("deepColor") && fc["deepColor"].is_array() &&
                        fc["deepColor"].size() == 3)
                        for (int i = 0; i < 3; i++) src->m_deepColor[i] = fc["deepColor"][i].get<float>();
                    if (fc.contains("glowColor") && fc["glowColor"].is_array() &&
                        fc["glowColor"].size() == 3)
                        for (int i = 0; i < 3; i++) src->m_glowColor[i] = fc["glowColor"][i].get<float>();
                    src->m_brightness  = fc.value("brightness",  src->m_brightness);
                    if (fc.contains("shallowColor") && fc["shallowColor"].is_array() &&
                        fc["shallowColor"].size() == 3)
                        for (int i = 0; i < 3; i++) src->m_shallowColor[i] = fc["shallowColor"][i].get<float>();
                    if (fc.contains("lightDir") && fc["lightDir"].is_array() &&
                        fc["lightDir"].size() == 3)
                        for (int i = 0; i < 3; i++) src->m_lightDir[i] = fc["lightDir"][i].get<float>();
                    src->m_lightIntensity = fc.value("lightIntensity", src->m_lightIntensity);
                    src->m_ambient        = fc.value("ambient",        src->m_ambient);
                    src->m_specular       = fc.value("specular",       src->m_specular);
                    src->m_rim            = fc.value("rim",            src->m_rim);
                    src->m_saturation     = fc.value("saturation",     src->m_saturation);
                    if (fc.contains("bgTop") && fc["bgTop"].is_array() && fc["bgTop"].size()==3)
                        for (int i=0;i<3;i++) src->m_bgTop[i] = fc["bgTop"][i].get<float>();
                    if (fc.contains("bgBottom") && fc["bgBottom"].is_array() && fc["bgBottom"].size()==3)
                        for (int i=0;i<3;i++) src->m_bgBottom[i] = fc["bgBottom"][i].get<float>();
                    src->m_bgAlpha     = fc.value("bgAlpha",     src->m_bgAlpha);
                    src->m_sphereScale = fc.value("sphereScale", src->m_sphereScale);
                    src->m_zoom        = fc.value("zoom",        src->m_zoom);
                    src->m_audioIntensity = fc.value("audioIntensity", src->m_audioIntensity);
                    src->m_gravity     = fc.value("gravity",     src->m_gravity);
                    src->m_vortex      = fc.value("vortex",      src->m_vortex);
                    src->m_turbulence  = fc.value("turbulence",  src->m_turbulence);
                    src->m_forceScale  = fc.value("forceScale",  src->m_forceScale);
                    src->m_sphereShape = fc.value("sphereShape", src->m_sphereShape);
                    src->m_fillAmount  = fc.value("fillAmount",  src->m_fillAmount);
                    src->m_particleCube = fc.value("particleCube", src->m_particleCube);
                    src->m_vjMode           = fc.value("vjMode",         src->m_vjMode);
                    src->m_vjGrab           = fc.value("vjGrab",         src->m_vjGrab);
                    src->m_journeySignal    = fc.value("journeySignal",  src->m_journeySignal);
                    src->m_journeyGain      = fc.value("journeyGain",    src->m_journeyGain);
                    src->m_journeyPosManual = fc.value("journeyPosManual", src->m_journeyPosManual);
                    if (fc.contains("lookLow") && fc["lookLow"].is_array() &&
                        fc["lookLow"].size() == FluidSource3D::kLookFloats) {
                        float a[FluidSource3D::kLookFloats];
                        for (int i = 0; i < FluidSource3D::kLookFloats; i++) a[i] = fc["lookLow"][i].get<float>();
                        src->lookFromArray(0, a);
                    }
                    if (fc.contains("lookHigh") && fc["lookHigh"].is_array() &&
                        fc["lookHigh"].size() == FluidSource3D::kLookFloats) {
                        float a[FluidSource3D::kLookFloats];
                        for (int i = 0; i < FluidSource3D::kLookFloats; i++) a[i] = fc["lookHigh"][i].get<float>();
                        src->lookFromArray(1, a);
                    }
                    // m_vjModePrev stays -1 so the first frame loads the right
                    // look for the restored mode.
                    src->m_autoRotate  = fc.value("autoRotate",  src->m_autoRotate);
                    src->m_rotateSpeed = fc.value("rotateSpeed", src->m_rotateSpeed);
                    src->m_tilt        = fc.value("tilt",        src->m_tilt);
                    src->m_simRes      = fc.value("simRes",      src->m_simRes);
                    src->m_substeps    = fc.value("substeps",    src->m_substeps);
                    src->m_renderScale = fc.value("renderScale", src->m_renderScale);
                    src->m_imageEnabled   = fc.value("imageEnabled", src->m_imageEnabled);
                    src->m_imageMix       = fc.value("imageMix",     src->m_imageMix);
                    src->imageSource().sourceLayerId =
                        fc.value("imageSourceLayerId",
                                 (uint32_t)src->imageSource().sourceLayerId);
                }
                int fw = 1280, fh = 720;
                if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() &&
                    m_zones[m_activeZone]) {
                    fw = m_zones[m_activeZone]->width;
                    fh = m_zones[m_activeZone]->height;
                }
                if (src->init(fw, fh)) {
                    if (layerJson.contains("audioBindings")) {
                        for (const auto& abj : layerJson["audioBindings"]) {
                            AudioBinding ab;
                            ab.signal = (AudioSignal)abj.value("signal", 0);
                            ab.rangeMin = abj.value("rangeMin", 0.0f);
                            ab.rangeMax = abj.value("rangeMax", 1.0f);
                            ab.smoothing = abj.value("smoothing", 0.55f);
                            ab.character = abj.value("character", 0.0f);
                            ab.midiCC = abj.value("midiCC", -1);
                            ab.midiChannel = abj.value("midiChannel", -1);
                            src->audioBindings()[abj.value("param", "")] = ab;
                        }
                    }
                    layer->source = src;
                }
            } else if (sourceType == "Hologram Model") {
                auto src = std::make_shared<HologramModelSource>();
                int hw = 1280, hh = 720;
                if (m_activeZone >= 0 && m_activeZone < (int)m_zones.size() &&
                    m_zones[m_activeZone]) {
                    hw = m_zones[m_activeZone]->width;
                    hh = m_zones[m_activeZone]->height;
                }
                if (src->init(hw, hh)) {
                    if (layerJson.contains("hologramConfig")) {
                        const auto& hc = layerJson["hologramConfig"];
                        auto& P = src->params();
                        P.rotateSpeed  = hc.value("rotateSpeed", P.rotateSpeed);
                        P.modelScale   = hc.value("modelScale", P.modelScale);
                        P.wireBright   = hc.value("wireBright", P.wireBright);
                        P.surfaceFill  = hc.value("surfaceFill", P.surfaceFill);
                        P.scanSpeed    = hc.value("scanSpeed", P.scanSpeed);
                        P.interference = hc.value("interference", P.interference);
                        P.chromaShift  = hc.value("chromaShift", P.chromaShift);
                        P.beamHaze     = hc.value("beamHaze", P.beamHaze);
                        P.audioReact   = hc.value("audioReact", P.audioReact);
                    }
                    if (!sourcePath.empty()) src->loadModel(sourcePath);
                    layer->source = src;
                }
#ifdef HAS_NDI
            } else if (sourceType == "NDI" && !sourcePath.empty()) {
                auto src = std::make_shared<NDISource>();
                std::string sourceUrl = layerJson.value("sourceUrl", "");
                if (src->connect(sourcePath, sourceUrl)) {
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

    // Voice-native auto-binding (must come AFTER layer IDs are assigned so
    // the binding key uses the final layer->id). loadShader() wires this up
    // for shaders added live, but project load builds ShaderSources directly
    // and never calls loadShader — so without this pass a restored text
    // shader has no `msg` -> `cue.latest` binding and the live transcript
    // never reaches it. Easel auto-loads the default project on startup, so
    // this is the common path. Mirrors loadShader()'s detection exactly.
    for (int i = 0; i < m_layerStack.count(); i++) {
        auto& layer = m_layerStack[i];
        if (!layer->source || !layer->source->isShader()) continue;
        auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
        const auto& inputs = shaderSrc->inputs();
        bool hasMsg = false;
        for (const auto& inp : inputs) {
            if (inp.type == "text" && inp.name == "msg") { hasMsg = true; break; }
        }
        if (hasMsg) {
            // Managed caption layers ("<zone>:captions") follow the raw live
            // transcript, not Cue's curated utterance — mirrors
            // ensureManagedShaderLayer's bindIfText.
            const std::string& mk = layer->managedKey;
            bool captions = mk.size() >= 9 &&
                mk.compare(mk.size() - 9, 9, ":captions") == 0;
            m_dataBus.bind(layer->id, "msg",
                           captions ? "etherea.latest" : "cue.latest");
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

// Flip + PNG-encode on a worker thread. The encode is the dominant cost of
// a screenshot (tens to hundreds of ms at show resolutions) and used to run
// on the render thread — externally triggerable mid-show via the
// screenshots/.capture file, so each capture froze a frame. At most one job
// runs at a time; the future's destructor joins at shutdown.
void Application::writeScreenshotAsync(const std::string& path,
                                       std::vector<uint8_t> pixels,
                                       int w, int h) {
    if (m_screenshotJob.valid()) m_screenshotJob.wait();
    m_screenshotJob = std::async(std::launch::async,
        [path, w, h, pixels = std::move(pixels)]() mutable {
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
            std::error_code ec;
            std::filesystem::create_directories(
                std::filesystem::path(path).parent_path(), ec);
            if (stbi_write_png(path.c_str(), w, h, 4, pixels.data(), stride)) {
                std::cout << "Screenshot saved: " << path << std::endl;
            } else {
                std::cerr << "Screenshot failed to write: " << path << std::endl;
            }
        });
}

void Application::captureScreenshot(const std::string& path) {
    auto& zone = activeZone();
    int w = zone.warpFBO.width();
    int h = zone.warpFBO.height();
    if (w <= 0 || h <= 0) {
        std::cerr << "Screenshot failed: no framebuffer" << std::endl;
        return;
    }

    std::vector<uint8_t> pixels((size_t)w * h * 4);
    glBindTexture(GL_TEXTURE_2D, zone.warpFBO.textureId());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    writeScreenshotAsync(path, std::move(pixels), w, h);
}

void Application::captureWindow(const std::string& path) {
    int w = m_windowWidth;
    int h = m_windowHeight;
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> pixels((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    writeScreenshotAsync(path, std::move(pixels), w, h);
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
void Application::loadRecentProjectsList() {
    m_recentProjects.clear();
    std::ifstream f("recent_projects.txt");
    std::string line;
    while (std::getline(f, line))
        if (!line.empty() && std::filesystem::exists(line))
            m_recentProjects.push_back(line);
}

void Application::addRecentProject(const std::string& path) {
    m_recentProjects.erase(
        std::remove(m_recentProjects.begin(), m_recentProjects.end(), path),
        m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), path);
    if (m_recentProjects.size() > 10) m_recentProjects.resize(10);
    std::ofstream f("recent_projects.txt");
    for (auto& p : m_recentProjects) f << p << "\n";
}

void Application::renderSplash() {
    if (!m_showSplash) return;

    double now = glfwGetTime();
    if (m_splashStartTime == 0.0) m_splashStartTime = now;
    double elapsed = now - m_splashStartTime;

    // Auto-dismiss at 1.6s → reveal landing page (instant cut, no fade).
    // Skipped when the default project auto-loaded — the show is already
    // on canvas and the landing page must not cover it.
    if (elapsed > 1.6) {
        m_showSplash  = false;
        m_showLanding = !m_autoLoadedProject;
        return;
    }

    // Content fades out in final 0.35s; background stays opaque so there's
    // never a black flash between the splash and the landing page overlay.
    float alpha = (elapsed > 1.25) ? (float)(1.0 - (elapsed - 1.25) / 0.35) : 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;

    ImGuiIO&    io = ImGui::GetIO();
    ImDrawList* fg = ImGui::GetForegroundDrawList();

    // Background always fully opaque
    fg->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(8, 10, 16, 255));

    float cx = io.DisplaySize.x * 0.5f;
    float cy = io.DisplaySize.y * 0.5f;
    float t  = (float)now;

    // ── "EASEL" wordmark ────────────────────────────────────────────────
    float titleSz = ImGui::GetFontSize() * 2.8f;
    const char* title = "EASEL";
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(titleSz, FLT_MAX, 0.0f, title);
    fg->AddText(ImGui::GetFont(), titleSz,
        ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f - 22.0f),
        IM_COL32(200, 218, 255, (int)(alpha * 245)), title);

    // Subtitle
    const char* sub = "Projection Mapping";
    ImVec2 ss = ImGui::CalcTextSize(sub);
    fg->AddText(ImVec2(cx - ss.x * 0.5f, cy + ts.y * 0.5f - 16.0f),
        IM_COL32(110, 135, 185, (int)(alpha * 200)), sub);

    // Thin accent line under subtitle
    float lw = ss.x * 0.55f;
    float lineY = cy + ts.y * 0.5f - 3.0f;
    fg->AddLine(ImVec2(cx - lw * 0.5f, lineY), ImVec2(cx + lw * 0.5f, lineY),
        IM_COL32(80, 110, 200, (int)(alpha * 120)), 1.2f);

    // Three pulsing loading dots
    float dotY = cy + ts.y * 0.5f + 24.0f;
    for (int i = 0; i < 3; i++) {
        float phase = t * 3.2f - i * 0.55f;
        float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(phase));
        fg->AddCircleFilled(
            ImVec2(cx + (i - 1) * 20.0f, dotY), 4.5f,
            IM_COL32(120, 155, 230, (int)(pulse * alpha * 230)));
    }

    // Skip hint (fades in after 0.6s)
    if (elapsed > 0.6) {
        float hintA = std::min(1.0f, (float)((elapsed - 0.6) / 0.4)) * alpha;
        const char* hint = "Press any key to continue";
        ImVec2 hs = ImGui::CalcTextSize(hint);
        fg->AddText(ImVec2(cx - hs.x * 0.5f, dotY + 26.0f),
            IM_COL32(70, 95, 145, (int)(hintA * 160)), hint);
    }

    // Skip on keypress or click
    if (elapsed > 0.3) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_Space)  ||
            ImGui::IsKeyPressed(ImGuiKey_Enter)  ||
            ImGui::IsMouseClicked(0)) {
            m_showSplash  = false;
            m_showLanding = true;
        }
    }
}

void Application::renderLandingPage() {
    if (!m_showLanding) return;

    // Everything lives on the foreground draw list so it renders OVER all
    // ImGui windows. Using ImGui::Begin() here was wrong — ImGui windows
    // render before the foreground list, so the dark background rect was
    // painting on top of the modal card every frame.
    ImGuiIO&    io  = ImGui::GetIO();
    ImDrawList* fg  = ImGui::GetForegroundDrawList();
    ImFont*     font = ImGui::GetFont();
    float        fsz = ImGui::GetFontSize();

    // Full-screen opaque cover
    fg->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(8, 10, 16, 255));

    float cx = io.DisplaySize.x * 0.5f;
    float cy = io.DisplaySize.y * 0.5f;

    // ── Modal card ───────────────────────────────────────────────────────
    const float mw = 320.0f, mh = 296.0f;
    float ml = cx - mw * 0.5f, mt = cy - mh * 0.5f;
    float mr = ml + mw,        mb = mt + mh;
    fg->AddRectFilled({ml, mt}, {mr, mb}, IM_COL32(18, 20, 28, 255), 10.0f);
    fg->AddRect      ({ml, mt}, {mr, mb}, IM_COL32(60, 70, 100, 180), 10.0f, 0, 1.1f);

    // Title
    float titleSz = fsz * 1.55f;
    ImVec2 ts = font->CalcTextSizeA(titleSz, FLT_MAX, 0.0f, "Easel");
    fg->AddText(font, titleSz, {ml + 24.0f, mt + 22.0f},
                IM_COL32(204, 224, 255, 255), "Easel");
    fg->AddText({ml + 24.0f, mt + 22.0f + ts.y + 2.0f},
                IM_COL32(120, 135, 170, 255), "Projection Mapping");
    float sepY = mt + 22.0f + ts.y + fsz + 16.0f;
    fg->AddLine({ml + 12.0f, sepY}, {mr - 12.0f, sepY}, IM_COL32(255,255,255,28), 1.0f);

    // ── Buttons (foreground-list drawn, mouse hit-tested) ─────────────
    ImVec2 mpos   = io.MousePos;
    bool   clicked = ImGui::IsMouseClicked(0);
    float  bx = ml + 24.0f, bw = mw - 48.0f, bh = 42.0f;
    float  by0 = sepY + 14.0f;

    auto fgButton = [&](int idx, const char* label) -> bool {
        float by = by0 + idx * (bh + 10.0f);
        bool hov = mpos.x >= bx && mpos.x <= bx + bw &&
                   mpos.y >= by && mpos.y <= by + bh;
        fg->AddRectFilled({bx, by}, {bx + bw, by + bh},
            hov ? IM_COL32(50, 58, 90, 255) : IM_COL32(30, 34, 50, 255), 6.0f);
        fg->AddRect({bx, by}, {bx + bw, by + bh},
            IM_COL32(80, 90, 130, hov ? 200 : 80), 6.0f, 0, 1.0f);
        ImVec2 lsz = ImGui::CalcTextSize(label);
        fg->AddText({bx + (bw - lsz.x) * 0.5f, by + (bh - lsz.y) * 0.5f},
                    IM_COL32(215, 225, 255, 255), label);
        return hov && clicked;
    };

    // Static state: recent dropdown open flag + deferred file-dialog trigger
    static bool s_recentOpen      = false;
    static bool s_openFilePending = false;

    if (fgButton(0, "New Project"))  { m_showLanding = false; }
    if (fgButton(1, "Open File...")) { m_showLanding = false; s_openFilePending = true; }
    if (fgButton(2, "Load Recent"))  { s_recentOpen = !s_recentOpen; }

    if (!m_showLanding) s_recentOpen = false;

    // ── Recent dropdown ───────────────────────────────────────────────
    if (s_recentOpen) {
        float dropX = bx;
        float dropY = by0 + 2 * (bh + 10.0f) + bh + 4.0f;
        float rowH  = 32.0f;
        int   show  = (int)std::min(m_recentProjects.size(), (size_t)10);
        float dropH = (show == 0) ? rowH : show * rowH + 8.0f;

        fg->AddRectFilled({dropX, dropY}, {dropX + bw, dropY + dropH},
                          IM_COL32(22, 26, 40, 252), 6.0f);
        fg->AddRect({dropX, dropY}, {dropX + bw, dropY + dropH},
                    IM_COL32(60, 70, 100, 180), 6.0f, 0, 1.0f);

        if (show == 0) {
            const char* none = "No recent projects";
            ImVec2 ns = ImGui::CalcTextSize(none);
            fg->AddText({dropX + (bw - ns.x) * 0.5f, dropY + (rowH - ns.y) * 0.5f},
                        IM_COL32(120, 135, 170, 255), none);
        } else {
            for (int i = 0; i < show; i++) {
                float ry  = dropY + 4.0f + i * rowH;
                const std::string& rp = m_recentProjects[i];
                std::string label = std::filesystem::path(rp).filename().string();
                bool hov = mpos.x >= dropX + 4 && mpos.x <= dropX + bw - 4 &&
                           mpos.y >= ry && mpos.y <= ry + rowH;
                if (hov)
                    fg->AddRectFilled({dropX + 4.0f, ry}, {dropX + bw - 4.0f, ry + rowH},
                                      IM_COL32(45, 52, 82, 255), 4.0f);
                ImVec2 ls = ImGui::CalcTextSize(label.c_str());
                fg->AddText({dropX + 12.0f, ry + (rowH - ls.y) * 0.5f},
                            IM_COL32(205, 215, 240, 255), label.c_str());
                if (hov && clicked) {
                    loadProject(rp);
                    m_showLanding = false;
                    s_recentOpen  = false;
                }
            }
        }
    }

    // ── Deferred open-file dialog (runs after draw so no half-frame flicker)
    if (s_openFilePending && !m_showLanding) {
        s_openFilePending = false;
        std::string path = openFileDialog("Easel Project\0*.easel\0All Files\0*.*\0");
        if (!path.empty()) loadProject(path);
        // landing already dismissed above
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (s_recentOpen) s_recentOpen = false;
        else              m_showLanding = false;
    }
}

void Application::renderEdgeBlendInline(OutputZone& zone) {
    if (flatSection("Edge Blend")) {
        auto* ebm = mappingForZone(zone);
        if (ebm) {
            // One row per side. The previous BeginPair layout truncated
            // "LEFT / RIGHT" and clipped the second control on narrow
            // panel widths; one-row-per-control is wider and consistent
            // with the rest of the Properties panel rhythm.
            ParamRow::Begin("LEFT");
            ImGui::DragFloat("##EBL", &ebm->edgeBlendLeft, 1.0f, 0.0f,
                             (float)zone.width * 0.5f, "%.0fpx");

            ParamRow::Begin("RIGHT");
            ImGui::DragFloat("##EBR", &ebm->edgeBlendRight, 1.0f, 0.0f,
                             (float)zone.width * 0.5f, "%.0fpx");

            ParamRow::Begin("TOP");
            ImGui::DragFloat("##EBT", &ebm->edgeBlendTop, 1.0f, 0.0f,
                             (float)zone.height * 0.5f, "%.0fpx");

            ParamRow::Begin("BOTTOM");
            ImGui::DragFloat("##EBB", &ebm->edgeBlendBottom, 1.0f, 0.0f,
                             (float)zone.height * 0.5f, "%.0fpx");

            ParamRow::Begin("GAMMA");
            ImGui::DragFloat("##EBGamma", &ebm->edgeBlendGamma, 0.05f, 0.5f,
                             4.0f, "%.2f");
        }
    }
}
