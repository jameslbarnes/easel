#include "ui/UIManager.h"
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

    // Extended glyph ranges for unicode support (accented chars, etc.)
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement (covers umlauts, accents)
        0x0100, 0x017F, // Latin Extended-A
        0x2000, 0x206F, // General Punctuation
        0x2190, 0x21FF, // Arrows
        0x25A0, 0x25FF, // Geometric Shapes
        0,
    };

    // Load Segoe UI from Windows for a clean, modern look
    float fontSize = 14.0f * dpiScale;
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 1;
    ImFont* mainFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", fontSize, &fontCfg, glyphRanges);
    if (!mainFont) {
        ImFontConfig defCfg;
        defCfg.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&defCfg);
    }

    // Slightly smaller font for secondary text
    float smallSize = 11.0f * dpiScale;
    ImFontConfig smallCfg;
    smallCfg.OversampleH = 2;
    m_smallFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", smallSize, &smallCfg, glyphRanges);
    if (!m_smallFont) {
        ImFontConfig defCfg;
        defCfg.SizePixels = smallSize;
        m_smallFont = io.Fonts->AddFontDefault(&defCfg);
    }

    // Bold font for headers
    ImFontConfig boldCfg;
    boldCfg.OversampleH = 2;
    m_boldFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", fontSize, &boldCfg, glyphRanges);
    if (!m_boldFont) m_boldFont = mainFont ? mainFont : io.Fonts->Fonts[0];

    applyTheme(dpiScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    return true;
}

void UIManager::applyTheme(float dpiScale) {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry - compact but readable
    s.WindowPadding     = ImVec2(8, 8);
    s.FramePadding      = ImVec2(6, 3);
    s.CellPadding       = ImVec2(4, 2);
    s.ItemSpacing       = ImVec2(6, 3);
    s.ItemInnerSpacing  = ImVec2(4, 3);
    s.IndentSpacing     = 14.0f;
    s.ScrollbarSize     = 8.0f;
    s.GrabMinSize       = 8.0f;

    // Rounding - smooth and modern
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;

    // Borders
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    // Alignment
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

    // Anti-aliasing
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;

    // --- Color Palette ---
    // Deep space background with blue undertone
    ImVec4 bgDeep       = ImVec4(0.055f, 0.063f, 0.082f, 1.0f);  // #0E1015
    ImVec4 bgPanel      = ImVec4(0.075f, 0.086f, 0.114f, 1.0f);  // #131623
    ImVec4 bgWidget     = ImVec4(0.110f, 0.125f, 0.165f, 1.0f);  // #1C202A
    ImVec4 bgWidgetHov  = ImVec4(0.145f, 0.165f, 0.220f, 1.0f);  // #252A38
    ImVec4 bgWidgetAct  = ImVec4(0.180f, 0.200f, 0.270f, 1.0f);  // #2E3345
    ImVec4 border       = ImVec4(0.160f, 0.180f, 0.240f, 0.50f); // subtle blue-gray
    ImVec4 borderLight  = ImVec4(0.200f, 0.220f, 0.300f, 0.35f);

    // Accent: electric cyan
    ImVec4 accent       = ImVec4(0.000f, 0.780f, 1.000f, 1.0f);  // #00C7FF
    ImVec4 accentDim    = ImVec4(0.000f, 0.550f, 0.710f, 1.0f);  // #008CB5
    ImVec4 accentSoft   = ImVec4(0.000f, 0.780f, 1.000f, 0.18f);
    ImVec4 accentMed    = ImVec4(0.000f, 0.780f, 1.000f, 0.40f);

    // Text hierarchy
    ImVec4 textPrimary  = ImVec4(0.900f, 0.920f, 0.960f, 1.0f);  // #E6EBF5
    ImVec4 textSecondary= ImVec4(0.550f, 0.600f, 0.680f, 1.0f);  // #8C99AD
    ImVec4 textDisabled = ImVec4(0.350f, 0.380f, 0.440f, 1.0f);  // #596170

    // Semantic
    ImVec4 success      = ImVec4(0.220f, 0.820f, 0.520f, 1.0f);  // #38D184
    ImVec4 warning      = ImVec4(1.000f, 0.720f, 0.240f, 1.0f);  // #FFB83D

    auto* c = s.Colors;

    // Window
    c[ImGuiCol_WindowBg]            = bgPanel;
    c[ImGuiCol_ChildBg]             = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]             = ImVec4(bgDeep.x, bgDeep.y, bgDeep.z, 0.96f);
    c[ImGuiCol_Border]              = border;
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);

    // Text
    c[ImGuiCol_Text]                = textPrimary;
    c[ImGuiCol_TextDisabled]        = textDisabled;

    // Title bar
    c[ImGuiCol_TitleBg]             = bgDeep;
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.065f, 0.075f, 0.100f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]    = ImVec4(bgDeep.x, bgDeep.y, bgDeep.z, 0.75f);

    // Menu bar
    c[ImGuiCol_MenuBarBg]           = bgDeep;

    // Frames (inputs, sliders, etc.)
    c[ImGuiCol_FrameBg]             = bgWidget;
    c[ImGuiCol_FrameBgHovered]      = bgWidgetHov;
    c[ImGuiCol_FrameBgActive]       = bgWidgetAct;

    // Tabs
    c[ImGuiCol_Tab]                 = ImVec4(bgWidget.x, bgWidget.y, bgWidget.z, 0.86f);
    c[ImGuiCol_TabHovered]          = accentMed;
    c[ImGuiCol_TabActive]           = ImVec4(accent.x * 0.25f, accent.y * 0.25f, accent.z * 0.25f, 1.0f);
    c[ImGuiCol_TabUnfocused]        = ImVec4(bgDeep.x, bgDeep.y, bgDeep.z, 0.97f);
    c[ImGuiCol_TabUnfocusedActive]  = ImVec4(bgWidget.x, bgWidget.y, bgWidget.z, 1.0f);

    // Headers (collapsing headers, selectables)
    c[ImGuiCol_Header]              = accentSoft;
    c[ImGuiCol_HeaderHovered]       = accentMed;
    c[ImGuiCol_HeaderActive]        = ImVec4(accent.x, accent.y, accent.z, 0.55f);

    // Buttons
    c[ImGuiCol_Button]              = bgWidget;
    c[ImGuiCol_ButtonHovered]       = bgWidgetHov;
    c[ImGuiCol_ButtonActive]        = accentMed;

    // Checkmarks, slider grabs
    c[ImGuiCol_CheckMark]           = accent;
    c[ImGuiCol_SliderGrab]          = accentDim;
    c[ImGuiCol_SliderGrabActive]    = accent;

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]         = ImVec4(bgDeep.x, bgDeep.y, bgDeep.z, 0.40f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.200f, 0.220f, 0.280f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.280f, 0.310f, 0.400f, 0.80f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;

    // Separator
    c[ImGuiCol_Separator]           = borderLight;
    c[ImGuiCol_SeparatorHovered]    = accentDim;
    c[ImGuiCol_SeparatorActive]     = accent;

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = ImVec4(accent.x, accent.y, accent.z, 0.10f);
    c[ImGuiCol_ResizeGripHovered]   = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_ResizeGripActive]    = ImVec4(accent.x, accent.y, accent.z, 0.70f);

    // Docking
    c[ImGuiCol_DockingPreview]      = ImVec4(accent.x, accent.y, accent.z, 0.50f);
    c[ImGuiCol_DockingEmptyBg]      = bgDeep;

    // Nav
    c[ImGuiCol_NavHighlight]        = accent;

    // Table
    c[ImGuiCol_TableHeaderBg]       = bgWidget;
    c[ImGuiCol_TableBorderStrong]   = border;
    c[ImGuiCol_TableBorderLight]    = borderLight;
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1, 1, 1, 0.02f);

    // Drag/drop
    c[ImGuiCol_DragDropTarget]      = warning;

    // Modal dim
    c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);

    // Scale for DPI
    s.ScaleAllSizes(dpiScale);
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
}

void UIManager::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::setupDockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
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

    // Build default layout on first frame (or when no layout saved)
    if (m_firstFrame) {
        m_firstFrame = false;

        // Only build if the dockspace has no saved layout
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr ||
            ImGui::DockBuilderGetNode(dockspaceId)->IsLeafNode()) {

            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

            // Split: left 70% = preview, right 30% = panels
            ImGuiID leftId, rightId;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.7f, &leftId, &rightId);

            // Split right side: top 50% = layers+properties, bottom 50% = warp+projector
            ImGuiID rightTopId, rightBottomId;
            ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.5f, &rightTopId, &rightBottomId);

            // Dock windows into regions
            ImGui::DockBuilderDockWindow("Projector Preview", leftId);
            ImGui::DockBuilderDockWindow("Layers", rightTopId);
            ImGui::DockBuilderDockWindow("Properties", rightTopId);
            ImGui::DockBuilderDockWindow("Warp", rightBottomId);
            ImGui::DockBuilderDockWindow("Projector", rightBottomId);
            ImGui::DockBuilderDockWindow("Capture", rightBottomId);
            ImGui::DockBuilderDockWindow("ShaderClaw", rightBottomId);

            ImGui::DockBuilderFinish(dockspaceId);
        }
    }

    ImGui::End();
}
