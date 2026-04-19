#include "ui/UIManager.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

bool UIManager::init(GLFWwindow* window) {
    m_window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Get DPI scale from the monitor
    float xscale = 1.0f, yscale = 1.0f;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        glfwGetMonitorContentScale(monitor, &xscale, &yscale);
    }
    float dpiScale = xscale;
    // On macOS Retina, fonts are loaded at 2x for crispness, then
    // FontGlobalScale is set to 1/dpiScale so they render at logical size.
    // On Windows, fonts and UI are both scaled by dpiScale.
    float fontScale = dpiScale;   // font texture resolution
#ifdef __APPLE__
    float uiScale = 1.0f;        // widget sizes — already in logical coords on mac
    m_baseFontGlobalScale = 1.0f / dpiScale;
#else
    float uiScale = dpiScale;
    m_baseFontGlobalScale = 1.0f;
#endif

    // Extended glyph ranges for unicode support (accented chars, etc.)
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement (covers umlauts, accents)
        0x0100, 0x017F, // Latin Extended-A
        0x2000, 0x206F, // General Punctuation
        0x2190, 0x21FF, // Arrows
        0x25A0, 0x25FF, // Geometric Shapes
        0,
    };

    // Font loading — prefer Segoe UI Variable (Windows 11, Inter-like) on
    // Windows with Segoe UI static fallback; Apple system fonts on macOS.
    // Variable font gives the "510 weight" feel Linear's design system leans on.
    const char* primaryFontPath = "C:/Windows/Fonts/SegUIVar.ttf";
    const char* primaryFallback = "C:/Windows/Fonts/segoeui.ttf";
    const char* boldFontPath    = "C:/Windows/Fonts/seguisb.ttf";       // SemiBold
    const char* boldFallback    = "C:/Windows/Fonts/segoeuib.ttf";      // Bold
    const char* monoFontPath    = "C:/Windows/Fonts/CascadiaMono.ttf";
    const char* macPrimaryPath  = "/System/Library/Fonts/Helvetica.ttc";
    const char* macPrimaryFB    = "/System/Library/Fonts/Supplemental/Arial.ttf";
    const char* macBoldPath     = "/System/Library/Fonts/Supplemental/Arial Bold.ttf";
    const char* macMonoPath     = "/System/Library/Fonts/Menlo.ttc";

    float fontSize = 15.0f * fontScale;  // denser feel; scaled for DPI
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 3;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH = false;
    ImFont* mainFont = nullptr;
#ifdef _WIN32
    mainFont = io.Fonts->AddFontFromFileTTF(primaryFontPath, fontSize, &fontCfg, glyphRanges);
    if (!mainFont) mainFont = io.Fonts->AddFontFromFileTTF(primaryFallback, fontSize, &fontCfg, glyphRanges);
#elif defined(__APPLE__)
    mainFont = io.Fonts->AddFontFromFileTTF(macPrimaryPath, fontSize, &fontCfg, glyphRanges);
    if (!mainFont) mainFont = io.Fonts->AddFontFromFileTTF(macPrimaryFB, fontSize, &fontCfg, glyphRanges);
#endif
    if (!mainFont) {
        ImFontConfig defCfg;
        defCfg.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&defCfg);
    }

    // Small font — for captions, metadata, tertiary labels
    float smallSize = 13.0f * fontScale;
    ImFontConfig smallCfg;
    smallCfg.OversampleH = 3;
    smallCfg.PixelSnapH = false;
    m_smallFont = nullptr;
#ifdef _WIN32
    m_smallFont = io.Fonts->AddFontFromFileTTF(primaryFontPath, smallSize, &smallCfg, glyphRanges);
    if (!m_smallFont) m_smallFont = io.Fonts->AddFontFromFileTTF(primaryFallback, smallSize, &smallCfg, glyphRanges);
#elif defined(__APPLE__)
    m_smallFont = io.Fonts->AddFontFromFileTTF(macPrimaryPath, smallSize, &smallCfg, glyphRanges);
    if (!m_smallFont) m_smallFont = io.Fonts->AddFontFromFileTTF(macPrimaryFB, smallSize, &smallCfg, glyphRanges);
#endif
    if (!m_smallFont) {
        ImFontConfig defCfg;
        defCfg.SizePixels = smallSize;
        m_smallFont = io.Fonts->AddFontDefault(&defCfg);
    }

    // Semibold for headers / emphasis (Linear's ~590 weight)
    ImFontConfig boldCfg;
    boldCfg.OversampleH = 3;
    boldCfg.PixelSnapH = false;
    m_boldFont = nullptr;
#ifdef _WIN32
    m_boldFont = io.Fonts->AddFontFromFileTTF(boldFontPath, fontSize, &boldCfg, glyphRanges);
    if (!m_boldFont) m_boldFont = io.Fonts->AddFontFromFileTTF(boldFallback, fontSize, &boldCfg, glyphRanges);
#elif defined(__APPLE__)
    m_boldFont = io.Fonts->AddFontFromFileTTF(macBoldPath, fontSize, &boldCfg, glyphRanges);
#endif
    if (!m_boldFont) m_boldFont = mainFont ? mainFont : io.Fonts->Fonts[0];

    // Mono font for uppercase section labels / technical metadata
    float monoSize = 11.0f * fontScale;
    ImFontConfig monoCfg;
    monoCfg.OversampleH = 3;
    monoCfg.PixelSnapH = false;
    m_monoFont = nullptr;
#ifdef _WIN32
    m_monoFont = io.Fonts->AddFontFromFileTTF(monoFontPath, monoSize, &monoCfg, glyphRanges);
#elif defined(__APPLE__)
    m_monoFont = io.Fonts->AddFontFromFileTTF(macMonoPath, monoSize, &monoCfg, glyphRanges);
#endif
    if (!m_monoFont) m_monoFont = m_smallFont;

    applyTheme(uiScale);

    // Set font global scale (on Retina: 0.5 to counteract 2x font texture)
    io.FontGlobalScale = m_baseFontGlobalScale * m_uiZoom;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __APPLE__
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 430");
#endif

    return true;
}

void UIManager::applyTheme(float dpiScale) {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry — more breathing room so rows don't feel squeezed
    s.WindowPadding     = ImVec2(14, 14);
    s.FramePadding      = ImVec2(12, 8);
    s.CellPadding       = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(12, 8);
    s.ItemInnerSpacing  = ImVec2(8, 5);
    s.IndentSpacing     = 18.0f;
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 12.0f;

    // Rounding — matches the 6/8/12 scale from the Linear mockup
    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 6.0f;

    // Borders — hairline semi-transparent whites, no solid dark borders
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    // Alignment
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);    // left-align titles (Linear pattern)
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

    // Disabled alpha for "on dark" contexts
    s.DisabledAlpha     = 0.45f;

    // Anti-aliasing
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;

    // --- Color Palette — Linear design system tokens ---
    // Background: near-black canvas with micro luminance steps, no blue tint
    ImVec4 bgVoid       = ImVec4(0.004f, 0.004f, 0.008f, 1.00f); // #010102 deepest
    ImVec4 bgDeep       = ImVec4(0.031f, 0.035f, 0.039f, 1.00f); // #08090a marketing black
    ImVec4 bgPanel      = ImVec4(0.059f, 0.063f, 0.067f, 0.92f); // #0f1011 — slight translucency for faux-glass
    ImVec4 bgWidget     = ImVec4(0.098f, 0.102f, 0.106f, 1.00f); // #191a1b elevated surface
    ImVec4 bgWidgetHov  = ImVec4(0.125f, 0.129f, 0.137f, 1.00f); // #202124 hover
    ImVec4 bgWidgetAct  = ImVec4(0.157f, 0.157f, 0.173f, 1.00f); // #28282c active/lightest dark

    // Borders — semi-transparent white, Linear's signature "moonlight" edges
    ImVec4 borderSubtle = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);
    ImVec4 border       = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    ImVec4 borderStrong = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);

    // Monochrome "accent" — no chromatic color anywhere. Interactions use
    // white-alpha tiers on the dark canvas, giving a dark/gray/light-gray palette.
    ImVec4 brand        = ImVec4(1.0f, 1.0f, 1.0f, 1.00f); // solid white for key interactive fills
    ImVec4 accent       = ImVec4(1.0f, 1.0f, 1.0f, 0.90f); // slider grabs, checkmarks
    ImVec4 accentHover  = ImVec4(1.0f, 1.0f, 1.0f, 1.00f);
    ImVec4 accentSoft   = ImVec4(1.0f, 1.0f, 1.0f, 0.07f); // selection / header bg (light gray tint)
    ImVec4 accentMed    = ImVec4(1.0f, 1.0f, 1.0f, 0.12f); // hover-header (slightly stronger)

    // Text tiers — never pure white, Linear's cool-warm near-white
    ImVec4 textPrimary  = ImVec4(0.969f, 0.973f, 0.973f, 1.00f); // #f7f8f8
    ImVec4 textSecondary= ImVec4(0.816f, 0.839f, 0.878f, 1.00f); // #d0d6e0
    ImVec4 textTertiary = ImVec4(0.541f, 0.561f, 0.596f, 1.00f); // #8a8f98
    ImVec4 textDisabled = ImVec4(0.384f, 0.400f, 0.427f, 1.00f); // #62666d

    // Semantic — muted, used sparingly
    ImVec4 success      = ImVec4(0.063f, 0.725f, 0.506f, 1.00f); // #10b981
    ImVec4 warning      = ImVec4(1.000f, 0.720f, 0.240f, 1.00f);
    ImVec4 error        = ImVec4(0.950f, 0.320f, 0.380f, 1.00f);
    (void)bgVoid; (void)borderStrong; (void)textTertiary; (void)error;
    (void)bgWidget; (void)bgWidgetHov; (void)bgWidgetAct; (void)textSecondary; (void)success;

    auto* c = s.Colors;

    // Window — translucent panel background for a faux-glass feel over the dark viewport
    c[ImGuiCol_WindowBg]            = bgPanel;
    c[ImGuiCol_ChildBg]             = ImVec4(1.0f, 1.0f, 1.0f, 0.02f); // ultra-subtle surface elevation
    c[ImGuiCol_PopupBg]             = ImVec4(0.098f, 0.102f, 0.106f, 0.96f); // #191a1b surface
    c[ImGuiCol_Border]              = border;
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);

    // Text
    c[ImGuiCol_Text]                = textPrimary;
    c[ImGuiCol_TextDisabled]        = textDisabled;

    // Title bar — nearly flush with the panel, no shouting
    c[ImGuiCol_TitleBg]             = ImVec4(0.059f, 0.063f, 0.067f, 0.92f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.098f, 0.102f, 0.106f, 0.95f);
    c[ImGuiCol_TitleBgCollapsed]    = ImVec4(0.031f, 0.035f, 0.039f, 0.75f);

    // Menu bar — flush with deep bg
    c[ImGuiCol_MenuBarBg]           = bgDeep;

    // Frames (inputs, sliders) — Linear's near-zero-opacity white over dark
    c[ImGuiCol_FrameBg]             = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(1.0f, 1.0f, 1.0f, 0.09f);

    // Tabs
    c[ImGuiCol_Tab]                 = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    c[ImGuiCol_TabHovered]          = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_TabActive]           = ImVec4(brand.x, brand.y, brand.z, 0.18f);
    c[ImGuiCol_TabUnfocused]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabUnfocusedActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);

    // Headers (CollapsingHeader, Selectable) — white at low opacity when selected
    c[ImGuiCol_Header]              = accentSoft;
    c[ImGuiCol_HeaderHovered]       = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
    c[ImGuiCol_HeaderActive]        = accentMed;

    // Buttons — ghost/subtle Linear buttons: transparent with hairline border effect (via ItemBg)
    c[ImGuiCol_Button]              = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    c[ImGuiCol_ButtonActive]        = accentMed;

    // Checkmarks / slider grabs — brand/accent at full intensity for key interactions
    c[ImGuiCol_CheckMark]           = accent;
    c[ImGuiCol_SliderGrab]          = accent;
    c[ImGuiCol_SliderGrabActive]    = accentHover;

    // Scrollbar — invisible track, hairline grab
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.22f);

    // Separator — subtle by default, brand on interaction
    c[ImGuiCol_Separator]           = borderSubtle;
    c[ImGuiCol_SeparatorHovered]    = accent;
    c[ImGuiCol_SeparatorActive]     = accentHover;

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);
    c[ImGuiCol_ResizeGripHovered]   = accentSoft;
    c[ImGuiCol_ResizeGripActive]    = accentMed;

    // Docking
    c[ImGuiCol_DockingPreview]      = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_DockingEmptyBg]      = bgDeep;

    // Nav focus — white highlight
    c[ImGuiCol_NavHighlight]        = accent;

    // Table
    c[ImGuiCol_TableHeaderBg]       = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    c[ImGuiCol_TableBorderStrong]   = border;
    c[ImGuiCol_TableBorderLight]    = borderSubtle;
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1.0f, 1.0f, 1.0f, 0.015f);

    // Drag/drop
    c[ImGuiCol_DragDropTarget]      = warning;

    // Modal dim — deep, near-opaque (Linear uses 0.85)
    c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);

    // Scale for DPI
    s.ScaleAllSizes(dpiScale);
}

void UIManager::handleZoom() {
    ImGuiIO& io = ImGui::GetIO();

    // Cmd+= / Cmd+- on macOS, Ctrl+= / Ctrl+- on Windows/Linux
#ifdef __APPLE__
    bool mod = io.KeySuper;
#else
    bool mod = io.KeyCtrl;
#endif

    if (mod && ImGui::IsKeyPressed(ImGuiKey_Equal)) {   // + / =
        m_uiZoom = std::min(m_uiZoom + 0.1f, 3.0f);
        io.FontGlobalScale = m_baseFontGlobalScale * m_uiZoom;
    }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_Minus)) {   // -
        m_uiZoom = std::max(m_uiZoom - 0.1f, 0.4f);
        io.FontGlobalScale = m_baseFontGlobalScale * m_uiZoom;
    }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_0)) {       // reset
        m_uiZoom = 1.0f;
        io.FontGlobalScale = m_baseFontGlobalScale * m_uiZoom;
    }
}

void UIManager::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    handleZoom();
}

void UIManager::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::setupDockspace(float bottomBarHeight) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImVec2 dockSize = viewport->WorkSize;
    dockSize.y -= bottomBarHeight;
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("EaselDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    // Rebuild layout on first frame or when window size changes significantly
    bool sizeChanged = false;
    if (!m_firstFrame) {
        float dw = fabsf(dockSize.x - m_lastDockW);
        float dh = fabsf(dockSize.y - m_lastDockH);
        // Trigger rebuild if size changed by more than 100px in either dimension
        // (catches maximize, restore, resolution change)
        if (dw > 100.0f || dh > 100.0f) {
            sizeChanged = true;
        }
    }

    if (m_firstFrame || sizeChanged) {
        m_firstFrame = false;
        m_lastDockW = dockSize.x;
        m_lastDockH = dockSize.y;

        // Always rebuild layout to ensure clean state
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, dockSize);

        // Responsive split based on window width
        float splitRatio = (dockSize.x > 1600) ? 0.65f : (dockSize.x > 1200) ? 0.60f : 0.55f;

        // Split: left = main viewport tabs, right = side panels (controls)
        ImGuiID leftId, rightId;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, splitRatio, &leftId, &rightId);

        // Split right side: top = Sources, bottom = Properties/inspectors
        ImGuiID rightTopId, rightBottomId;
        ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.25f, &rightTopId, &rightBottomId);

        // Dock windows into regions per workspace.
        //   Stage  — 3D layout/warp/masks take center; Canvas preview + Layers on side.
        //   Canvas — Canvas center; sources + properties on sides. (Default authoring view.)
        //   Show   — Canvas center; live-ops panels (Audio, MIDI, NDI/Stream/Spout) on sides.
        // First DockBuilderDockWindow call for a region wins focus, so the focused
        // tab per workspace is deliberate.
        switch (m_workspace) {
        case Workspace::Stage:
            // Center: spatial setup tools (Stage focused)
            ImGui::DockBuilderDockWindow("Stage", leftId);
            ImGui::DockBuilderDockWindow("Mapping", leftId);
            ImGui::DockBuilderDockWindow("Masks", leftId);
            ImGui::DockBuilderDockWindow("Scene Scanner", leftId);
            ImGui::DockBuilderDockWindow("Canvas", leftId);
            // Side: minimal — layer picking + properties
            ImGui::DockBuilderDockWindow("Layers", rightTopId);
            ImGui::DockBuilderDockWindow("Capture", rightTopId);
            ImGui::DockBuilderDockWindow("Properties", rightBottomId);
            ImGui::DockBuilderDockWindow("Audio", rightBottomId);
            break;

        case Workspace::Canvas:
            // Center: authoring preview (Canvas focused)
            ImGui::DockBuilderDockWindow("Canvas", leftId);
            ImGui::DockBuilderDockWindow("Mapping", leftId);
            ImGui::DockBuilderDockWindow("Masks", leftId);
            ImGui::DockBuilderDockWindow("Stage", leftId);
            // Right top: sources (Layers focused)
            ImGui::DockBuilderDockWindow("Layers", rightTopId);
            ImGui::DockBuilderDockWindow("ShaderClaw", rightTopId);
            ImGui::DockBuilderDockWindow("Etherea", rightTopId);
            ImGui::DockBuilderDockWindow("Capture", rightTopId);
            ImGui::DockBuilderDockWindow("Audio Mixer", rightTopId);
            // Right bottom: inspectors (Properties focused)
            ImGui::DockBuilderDockWindow("Properties", rightBottomId);
            ImGui::DockBuilderDockWindow("Audio", rightBottomId);
            ImGui::DockBuilderDockWindow("MIDI", rightBottomId);
            break;

        case Workspace::Show:
            // Center: live preview (Canvas focused)
            ImGui::DockBuilderDockWindow("Canvas", leftId);
            ImGui::DockBuilderDockWindow("Stage", leftId);
            // Right top: live I/O monitoring (Audio focused)
            ImGui::DockBuilderDockWindow("Audio", rightTopId);
            ImGui::DockBuilderDockWindow("Audio Mixer", rightTopId);
            ImGui::DockBuilderDockWindow("MIDI", rightTopId);
            // Right bottom: broadcast / feeds (NDI focused)
            ImGui::DockBuilderDockWindow("NDI", rightBottomId);
            ImGui::DockBuilderDockWindow("Spout", rightBottomId);
            ImGui::DockBuilderDockWindow("Stream", rightBottomId);
            ImGui::DockBuilderDockWindow("Properties", rightBottomId);
            break;
        }

        ImGui::DockBuilderFinish(dockspaceId);
    } else {
        // Track size for change detection even when not rebuilding
        m_lastDockW = dockSize.x;
        m_lastDockH = dockSize.y;
    }

    ImGui::End();
}

void UIManager::setWorkspace(Workspace w) {
    if (w == m_workspace) return;
    m_workspace = w;
    // Force dock layout rebuild on next setupDockspace call
    m_firstFrame = true;
}
