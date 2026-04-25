#pragma once
#include "render/Mesh3D.h"
#include "render/ShaderProgram.h"
#include "render/Framebuffer.h"
#include "render/Mesh.h"
#include "warp/ObjMeshWarp.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// A virtual projector in the 3D stage
struct VirtualProjector {
    std::string name = "Projector";
    glm::vec3 position = {0.0f, 2.0f, 3.0f};
    glm::vec3 target = {0.0f, 0.0f, 0.0f};   // look-at point
    float fovDeg = 60.0f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 50.0f;
    int zoneIndex = 0;     // which OutputZone this feeds
    bool visible = true;

    glm::mat4 viewMatrix() const;
    glm::mat4 projMatrix() const;
    glm::mat4 vpMatrix() const; // proj * view
};

// A projection surface on the 3D model
struct ProjectionSurface {
    std::string name = "Screen";
    int materialIndex = 0;  // which material group to project onto
    int projectorIndex = 0; // which projector feeds this surface
    bool active = true;
    int clusterIndex = -1;  // -1 = ungrouped, >= 0 = belongs to a ScreenCluster
};

// A named group of screens positioned as a unit in 3D space
struct ScreenCluster {
    std::string name = "Cluster";
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f}; // euler angles (degrees)
    float scale = 1.0f;

    // Grid arrangement: how screens are laid out within this cluster
    int gridCols = 1;  // e.g., 2 for a 2x1 video wall
    int gridRows = 1;
    float gapX = 0.0f; // gap between screens in local units
    float gapY = 0.0f;
    float screenW = 1.92f; // individual screen width (meters)
    float screenH = 1.08f; // individual screen height (meters)

    bool collapsed = false; // UI state
    bool visible = true;

    // Compute the model matrix for this cluster
    glm::mat4 getTransform() const;
};

// A display in the 3D stage: a textured plane showing zone content.
// This is the primary abstraction for pre-viz - every output is a Display.
struct StageDisplay {
    enum class Type { Projector = 0, LED, TV, Monitor, Custom };
    static const char* typeName(Type t) {
        switch (t) {
            case Type::Projector: return "Projector";
            case Type::LED: return "LED Screen";
            case Type::TV: return "TV";
            case Type::Monitor: return "Monitor";
            case Type::Custom: return "Custom";
            default: return "Display";
        }
    }

    std::string name = "Display";
    Type type = Type::LED;

    // 3D transform (position in meters, rotation in degrees)
    glm::vec3 position = {0.0f, 1.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f}; // euler YXZ
    float width = 1.92f;  // meters
    float height = 1.08f; // meters

    // Content assignment
    int zoneIndex = 0; // which OutputZone feeds this display

    bool visible = true;
    bool selected = false;

    glm::mat4 getTransform() const;
};

class StageView {
public:
    bool init();

    // Render the 3D stage panel (call inside ImGui frame)
    void render(const std::vector<GLuint>& zoneTextures,
                std::function<void()> inlineTopSection = nullptr);
    // Render the Scene panel — displays / projectors / surfaces / clusters.
    // Split out of the Stage 3D panel so the 3D viewport gets full height.
    void renderSceneInspector(const std::vector<GLuint>& zoneTextures);

    // Model management
    bool loadModel(const std::string& path);
    bool hasModel() const;

    // Projector management
    int addProjector(const std::string& name = "Projector");
    void removeProjector(int index);
    VirtualProjector& projector(int index) { return m_projectors[index]; }
    int projectorCount() const { return (int)m_projectors.size(); }
    std::vector<VirtualProjector>& projectors() { return m_projectors; }

    // Surface management
    int addSurface(const std::string& name, int materialIdx, int projIdx);
    void removeSurface(int index);
    int surfaceCount() const { return (int)m_surfaces.size(); }
    std::vector<ProjectionSurface>& surfaces() { return m_surfaces; }

    // Cluster management
    int addCluster(const std::string& name = "Cluster");
    void removeCluster(int index);
    int clusterCount() const { return (int)m_clusters.size(); }
    std::vector<ScreenCluster>& clusters() { return m_clusters; }

    // Create a grid of surfaces inside a cluster, each mapped to consecutive zones
    void populateClusterGrid(int clusterIdx, int startZone);

    // Display management
    int addDisplay(const std::string& name = "Display", StageDisplay::Type type = StageDisplay::Type::LED);
    void removeDisplay(int index);
    int displayCount() const { return (int)m_displays.size(); }
    std::vector<StageDisplay>& displays() { return m_displays; }

    // Selected items
    int selectedProjector() const { return m_selectedProjector; }
    int selectedSurface() const { return m_selectedSurface; }
    int selectedCluster() const { return m_selectedCluster; }
    int selectedDisplay() const { return m_selectedDisplay; }

    // Import signal
    bool wantsImport() const { return m_wantsImport; }
    void clearImportSignal() { m_wantsImport = false; }

private:
    // 3D scene
    std::vector<MaterialGroup> m_materials;
    std::string m_modelPath;
    float m_modelScale = 1.0f;
    glm::vec3 m_bboxCenter = {0, 0, 0};
    float m_bboxExtent = 3.0f;

    // Camera for the stage view
    OrbitCamera m_camera;

    // Projectors, surfaces, clusters, and displays
    std::vector<VirtualProjector> m_projectors;
    std::vector<ProjectionSurface> m_surfaces;
    std::vector<ScreenCluster> m_clusters;
    std::vector<StageDisplay> m_displays;
    int m_selectedProjector = -1;
    int m_selectedSurface = -1;
    int m_selectedCluster = -1;
    int m_selectedDisplay = -1;
    bool m_wantsImport = false;

    // Rendering
    ShaderProgram m_shader;         // objmesh shader
    Framebuffer m_fbo;              // offscreen render target
    Mesh m_quad;
    int m_fboWidth = 800, m_fboHeight = 600;

    // Interaction
    bool m_orbitDragging = false;
    glm::vec2 m_orbitDragStart = {0, 0};
    float m_orbitStartAzimuth = 0;
    float m_orbitStartElevation = 0;
    bool m_panDragging = false;
    glm::vec2 m_panDragStart = {0, 0};
    glm::vec3 m_panStartTarget = {0, 0, 0};

    // Smooth camera animation (for F-to-frame)
    bool m_cameraAnimating = false;
    float m_cameraAnimTime = 0.0f;
    float m_cameraAnimDuration = 0.35f; // seconds
    glm::vec3 m_cameraAnimStartTarget = {0, 0, 0};
    glm::vec3 m_cameraAnimEndTarget = {0, 0, 0};
    float m_cameraAnimStartDist = 5.0f;
    float m_cameraAnimEndDist = 5.0f;

    // Drawing helpers
    void renderScene(const std::vector<GLuint>& zoneTextures, float aspect);
    void renderDisplays(const std::vector<GLuint>& zoneTextures, const glm::mat4& viewProj);
    void renderFrustum(const VirtualProjector& proj, const glm::mat4& viewProj, bool selected);
    void renderEnvironment(const glm::mat4& viewProj);   // floor + back wall
public:
    // Renders the 3D viewport + toolbar directly into the current window,
    // without opening its own window. Callers that already have a window open
    // (e.g. the Stage panel in Application.cpp) use this instead of render().
    void renderUI(const std::vector<GLuint>& zoneTextures);
    // Pinned top-of-panel toolbar: Floor/Wall + Display/Projector/Surface/
    // Import plus tab-focus buttons for Mapping and Masks. Rendered by the
    // host (Application) ABOVE the scrollable viewport child so it stays put.
    void renderToolbar();
private:

    // Stage environment — light floor + back wall so the 3D space has ground.
public:
    bool& environmentVisible() { return m_envVisible; }
private:
    Mesh3D m_floorMesh;
    Mesh3D m_wallMesh;
    bool m_envReady = false;
    bool m_envVisible = true;

    // Display quad mesh (unit quad, scaled per display)
    Mesh m_displayQuad;
    ShaderProgram m_displayShader; // simple textured quad with MVP
    bool m_displayShaderReady = false;

    // Gizmo state
    int m_gizmoOp = 0; // 0=translate, 1=rotate, 2=scale
};
