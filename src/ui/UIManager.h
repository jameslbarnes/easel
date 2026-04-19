#pragma once

struct GLFWwindow;
struct ImFont;

// Three top-level modes that map to the phases of a live show:
//   Stage  — physical/spatial setup (projector layout, warp, masks, scanner)
//   Canvas — content authoring (layers, properties, sources)
//   Show   — live ops (preview + I/O monitoring)
enum class Workspace { Stage, Canvas, Show };

class UIManager {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setupDockspace(float bottomBarHeight = 0);

    ImFont* smallFont() const { return m_smallFont; }
    ImFont* boldFont() const { return m_boldFont; }
    ImFont* monoFont() const { return m_monoFont; }

    void handleZoom();  // call each frame to handle Cmd+/- zoom
    float uiScale() const { return m_uiZoom; }

    Workspace workspace() const { return m_workspace; }
    void setWorkspace(Workspace w);

private:
    void applyTheme(float dpiScale);

    GLFWwindow* m_window = nullptr;
    bool m_firstFrame = true;
    float m_lastDockW = 0;
    float m_lastDockH = 0;
    ImFont* m_smallFont = nullptr;
    ImFont* m_boldFont = nullptr;
    ImFont* m_monoFont = nullptr;
    float m_uiZoom = 1.0f;
    float m_baseFontGlobalScale = 1.0f;
    Workspace m_workspace = Workspace::Canvas;
};
