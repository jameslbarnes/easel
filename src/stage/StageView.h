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
};

class StageView {
public:
    bool init();

    // Render the 3D stage panel (call inside ImGui frame)
    void render(const std::vector<GLuint>& zoneTextures);

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

    // Selected items
    int selectedProjector() const { return m_selectedProjector; }
    int selectedSurface() const { return m_selectedSurface; }

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

    // Projectors and surfaces
    std::vector<VirtualProjector> m_projectors;
    std::vector<ProjectionSurface> m_surfaces;
    int m_selectedProjector = -1;
    int m_selectedSurface = -1;
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

    // Drawing helpers
    void renderScene(const std::vector<GLuint>& zoneTextures, float aspect);
    void renderFrustum(const VirtualProjector& proj, const glm::mat4& viewProj, bool selected);
    void renderUI(const std::vector<GLuint>& zoneTextures);
};
