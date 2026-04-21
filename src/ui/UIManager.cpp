#include "ui/UIManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
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

    // Geometry — Apple/Vercel-density: tight enough to feel intentional,
    // loose enough to breathe. 10x6 frame padding fits "Masks"/"Mapping" tabs
    // without truncation at any reasonable dock width.
    s.WindowPadding     = ImVec2(16, 14);
    s.FramePadding      = ImVec2(10, 6);
    s.CellPadding       = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 12.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding    = ImVec2(12, 4);

    // Rounding — unified on 6/4 scale (Vercel-style consistent geometry)
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 4.0f;
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
    ImVec2 dockPos = viewport->WorkPos;
    dockPos.y += m_workspaceBarHeight;           // shift below the primary nav bar
    ImGui::SetNextWindowPos(dockPos);
    ImVec2 dockSize = viewport->WorkSize;
    dockSize.y -= (bottomBarHeight + m_workspaceBarHeight);
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

        // Layout — Timeline docked to the bottom (full width), main area above it:
        //   timelineId    = bottom strip — Timeline panel
        //   mainId        = everything else (split below)
        //   canvasId      = left of main — Canvas, always visible
        //   toolsId       = top of right — viewport tools (Stage, Mapping, Masks, Scanner)
        //   rightTopId    = middle of right — scanner etc.
        //   rightBottomId = bottom of right — inspectors + I/O (Properties, Audio, MIDI, NDI, ...)
        ImGuiID mainId, timelineDockId;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.24f, &timelineDockId, &mainId);
        ImGuiID canvasId, rightId;
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, splitRatio, &canvasId, &rightId);

        // Split right column top-to-bottom: tools → sources → inspectors.
        // 0.34 gives "Masks"/"Mapping" tabs room to render without truncation.
        ImGuiID toolsId, rightRemainder;
        ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.34f, &toolsId, &rightRemainder);
        ImGuiID rightTopId, rightBottomId;
        ImGui::DockBuilderSplitNode(rightRemainder, ImGuiDir_Up, 0.45f, &rightTopId, &rightBottomId);

        // Each workspace docks ONLY its whitelisted panels (see isPanelVisible).
        // Hidden panels aren't rendered AND aren't docked — no stray tabs.
        // Dock order matters: the first DockBuilderDockWindow call for a
        // region wins initial focus.
        auto dockIfVisible = [this](const char* name, ImGuiID node) {
            if (isPanelVisible(name)) ImGui::DockBuilderDockWindow(name, node);
        };

        // Left: Canvas + Stage + Scenes as peer tabs. Canvas is the default focused tab.
        dockIfVisible("Canvas",        canvasId);
        dockIfVisible("Stage",         canvasId);
        dockIfVisible("Scenes",        canvasId);

        // Top-right: primary tabs in the requested order — Layers focused.
        dockIfVisible("Layers",        toolsId);
        dockIfVisible("Mapping",       toolsId);
        dockIfVisible("Masks",         toolsId);
        dockIfVisible("Sources",       toolsId);

        // Middle-right: Scanner only (Scene merged into Scenes; Mixer merged into Audio).
        dockIfVisible("Scene Scanner", rightTopId);

        // Properties + I/O bottom-right. NDI/Spout/Capture/ShaderClaw/Etherea
        // are no longer separate docks — they live as tabs inside Sources.
        dockIfVisible("Properties",    rightBottomId);
        dockIfVisible("Audio",         rightBottomId);
        dockIfVisible("MIDI",          rightBottomId);
        dockIfVisible("Stream",        rightBottomId);

        // Bottom strip: the Timeline lives here, spanning the full width.
        dockIfVisible("Timeline",      timelineDockId);

        ImGui::DockBuilderFinish(dockspaceId);

        // Two deferred focus passes: Canvas in the big left slot, Layers as
        // the active tab in the top-right. SetWindowFocus is called every
        // frame while m_pendingFocusFramesLeft > 0 so both settle correctly.
        m_pendingFocus = "Layers";
        m_pendingFocusFramesLeft = 3;
    } else {
        // Track size for change detection even when not rebuilding
        m_lastDockW = dockSize.x;
        m_lastDockH = dockSize.y;
    }

    ImGui::End();

    // Apply deferred focus. SetWindowFocus only works if the named window
    // has been Begin()-ed at least once in a previous frame, so we may need
    // to retry across a couple frames after a dock rebuild.
    if (m_pendingFocus) {
        ImGui::SetWindowFocus(m_pendingFocus);
        m_pendingFocusFramesLeft--;
        if (m_pendingFocusFramesLeft <= 0) m_pendingFocus = nullptr;
    }
}

void UIManager::setWorkspace(Workspace w) {
    if (w == m_workspace) return;
    m_workspace = w;
    // Force dock layout rebuild on next setupDockspace call
    m_firstFrame = true;
}

bool UIManager::isPanelVisible(const char* title) const {
    // Single unified layout — every tracked panel renders + docks. The
    // workspace concept is retired for now (Canvas/Show switcher removed
    // from the menu bar). Kept as a no-op so future curated views can
    // plug back in if we bring modes back.
    (void)title;
    return true;
}

void UIManager::renderWorkspaceBar() {
    // No-op. Workspace switcher now lives inside the main menu bar
    // (see Application::renderMenuBar). Kept as a method so the API
    // doesn't break callers and future redesigns can slot back in.
    m_workspaceBarHeight = 0.0f;
}
