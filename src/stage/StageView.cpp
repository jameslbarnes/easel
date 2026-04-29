#include "stage/StageView.h"
#include <imgui.h>
#include "ui/ImGuizmo.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <functional>
#include <initializer_list>

#define TINYOBJLOADER_IMPLEMENTATION
// Already defined in ObjMeshWarp.cpp, so we use extern
#undef TINYOBJLOADER_IMPLEMENTATION

// --- VirtualProjector ---

glm::mat4 VirtualProjector::viewMatrix() const {
    return glm::lookAt(position, target, glm::vec3(0, 1, 0));
}

glm::mat4 VirtualProjector::projMatrix() const {
    return glm::perspective(glm::radians(fovDeg), aspectRatio, nearPlane, farPlane);
}

glm::mat4 VirtualProjector::vpMatrix() const {
    return projMatrix() * viewMatrix();
}

// --- StageView ---

bool StageView::init() {
    if (!m_shader.loadFromFiles("shaders/objmesh.vert", "shaders/objmesh.frag")) {
        std::cerr << "[Stage] Failed to load shader" << std::endl;
        return false;
    }
    m_quad.createQuad();
    m_fbo.create(m_fboWidth, m_fboHeight, true); // with depth

    // Friendly empty state — drop a default 1920x1080 display into the scene
    // so there's always something showing the test pattern. Users can delete
    // it, import a model over it, or just use it as their screen if the show
    // is that simple. Defaults in StageDisplay are already 1.92m x 1.08m and
    // zoneIndex = 0, which means it will pick up zone 0's live content.
    if (m_displays.empty()) {
        addDisplay("Default Screen", StageDisplay::Type::Monitor);
    }

    // Default camera: orbit slightly right + above the display so the user can
    // tell they're looking at a plane in 3D space (not a flat preview). A pure
    // head-on camera reads like a Canvas tab clone and hides the spatial
    // context Stage is meant to communicate.
    m_camera.azimuth   = 0.35f;
    m_camera.elevation = 0.22f;
    m_camera.distance  = 3.2f;
    m_camera.target    = {0.0f, 1.0f, 0.0f}; // display center (y=1 per StageDisplay default)
    return true;
}

bool StageView::loadModel(const std::string& path) {
    // Reuse ObjMeshWarp's loading logic
    ObjMeshWarp loader;
    if (!loader.loadModel(path)) return false;

    m_materials = std::move(loader.materials());
    m_modelPath = path;
    m_modelScale = loader.modelScale();
    m_bboxCenter = {0, 0, 0};

    // Compute bounding box
    float maxExtent = 0;
    for (const auto& mg : m_materials) {
        // We don't have direct vertex access, so use the scale from loader
        maxExtent = std::max(maxExtent, 1.0f);
    }
    m_bboxExtent = loader.modelScale() > 0 ? 3.0f / loader.modelScale() : 3.0f;

    // Reset camera to fit model
    m_camera.distance = m_bboxExtent * 2.5f;
    m_camera.target = m_bboxCenter;
    m_camera.azimuth = 0.4f;
    m_camera.elevation = 0.3f;

    std::cout << "[Stage] Loaded model: " << path << " (" << m_materials.size() << " materials)" << std::endl;
    return true;
}

bool StageView::hasModel() const {
    return !m_materials.empty();
}

int StageView::addProjector(const std::string& name) {
    VirtualProjector proj;
    proj.name = name;
    proj.position = {0, 2, 4};
    proj.target = {0, 0, 0};
    proj.zoneIndex = (int)m_projectors.size();
    m_projectors.push_back(proj);
    return (int)m_projectors.size() - 1;
}

void StageView::removeProjector(int index) {
    if (index >= 0 && index < (int)m_projectors.size()) {
        m_projectors.erase(m_projectors.begin() + index);
        if (m_selectedProjector >= (int)m_projectors.size())
            m_selectedProjector = (int)m_projectors.size() - 1;
    }
}

int StageView::addSurface(const std::string& name, int materialIdx, int projIdx) {
    ProjectionSurface surf;
    surf.name = name;
    surf.materialIndex = materialIdx;
    surf.projectorIndex = projIdx;
    m_surfaces.push_back(surf);
    return (int)m_surfaces.size() - 1;
}

void StageView::removeSurface(int index) {
    if (index >= 0 && index < (int)m_surfaces.size()) {
        m_surfaces.erase(m_surfaces.begin() + index);
    }
}

// --- ScreenCluster ---

glm::mat4 ScreenCluster::getTransform() const {
    glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
    t = glm::rotate(t, glm::radians(rotation.y), glm::vec3(0, 1, 0));
    t = glm::rotate(t, glm::radians(rotation.x), glm::vec3(1, 0, 0));
    t = glm::rotate(t, glm::radians(rotation.z), glm::vec3(0, 0, 1));
    t = glm::scale(t, glm::vec3(scale));
    return t;
}

int StageView::addCluster(const std::string& name) {
    ScreenCluster cluster;
    cluster.name = name;
    m_clusters.push_back(cluster);
    return (int)m_clusters.size() - 1;
}

void StageView::removeCluster(int index) {
    if (index >= 0 && index < (int)m_clusters.size()) {
        // Unlink any surfaces belonging to this cluster
        for (auto& surf : m_surfaces) {
            if (surf.clusterIndex == index) surf.clusterIndex = -1;
            else if (surf.clusterIndex > index) surf.clusterIndex--;
        }
        m_clusters.erase(m_clusters.begin() + index);
        if (m_selectedCluster >= (int)m_clusters.size()) m_selectedCluster = -1;
    }
}

void StageView::populateClusterGrid(int clusterIdx, int startZone) {
    if (clusterIdx < 0 || clusterIdx >= (int)m_clusters.size()) return;
    auto& cl = m_clusters[clusterIdx];

    // Remove existing surfaces in this cluster
    for (int i = (int)m_surfaces.size() - 1; i >= 0; i--) {
        if (m_surfaces[i].clusterIndex == clusterIdx)
            m_surfaces.erase(m_surfaces.begin() + i);
    }

    // Create grid surfaces
    int zone = startZone;
    for (int r = 0; r < cl.gridRows; r++) {
        for (int c = 0; c < cl.gridCols; c++) {
            ProjectionSurface surf;
            surf.name = cl.name + " " + std::to_string(r * cl.gridCols + c + 1);
            surf.clusterIndex = clusterIdx;
            surf.projectorIndex = std::min(zone, (int)m_projectors.size() - 1);
            if (surf.projectorIndex < 0) surf.projectorIndex = 0;
            m_surfaces.push_back(surf);
            zone++;
        }
    }
}

// --- StageDisplay ---

glm::mat4 StageDisplay::getTransform() const {
    glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
    t = glm::rotate(t, glm::radians(rotation.y), glm::vec3(0, 1, 0));
    t = glm::rotate(t, glm::radians(rotation.x), glm::vec3(1, 0, 0));
    t = glm::rotate(t, glm::radians(rotation.z), glm::vec3(0, 0, 1));
    // Scale the unit quad (-1..1) to actual display dimensions (half-extents)
    t = glm::scale(t, glm::vec3(width * 0.5f, height * 0.5f, 1.0f));
    return t;
}

int StageView::addDisplay(const std::string& name, StageDisplay::Type type) {
    StageDisplay d;
    d.name = name;
    d.type = type;
    d.zoneIndex = 0;
    // Spread displays out along X so they don't overlap
    d.position.x = (float)m_displays.size() * 2.5f;
    m_displays.push_back(d);
    m_selectedDisplay = (int)m_displays.size() - 1;
    return m_selectedDisplay;
}

void StageView::removeDisplay(int index) {
    if (index >= 0 && index < (int)m_displays.size()) {
        m_displays.erase(m_displays.begin() + index);
        if (m_selectedDisplay >= (int)m_displays.size()) m_selectedDisplay = -1;
    }
}

void StageView::renderDisplays(const std::vector<GLuint>& zoneTextures, const glm::mat4& viewProj) {
    if (m_displays.empty()) return;

    // Load display shader on first use
    if (!m_displayShaderReady) {
        if (m_displayShader.loadFromFiles("shaders/display3d.vert", "shaders/display3d.frag")) {
            m_displayShaderReady = true;
            m_displayQuad.createQuad(); // unit quad -1..1
        }
    }
    if (!m_displayShaderReady) return;

    glEnable(GL_DEPTH_TEST);

    for (int i = 0; i < (int)m_displays.size(); i++) {
        auto& d = m_displays[i];
        if (!d.visible) continue;

        glm::mat4 model = d.getTransform();
        glm::mat4 mvp = viewProj * model;

        m_displayShader.use();
        m_displayShader.setMat4("uMVP", mvp);
        m_displayShader.setFloat("uOpacity", 1.0f);
        m_displayShader.setInt("uTexture", 0);

        // Border color: cyan normally, yellow when selected
        bool sel = (i == m_selectedDisplay);
        if (sel) {
            m_displayShader.setVec4("uBorderColor", glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));
            m_displayShader.setFloat("uBorderWidth", 0.015f);
        } else {
            m_displayShader.setVec4("uBorderColor", glm::vec4(1.0f, 1.0f, 1.0f, 0.6f));
            m_displayShader.setFloat("uBorderWidth", 0.008f);
        }

        // Bind zone texture
        glActiveTexture(GL_TEXTURE0);
        if (d.zoneIndex >= 0 && d.zoneIndex < (int)zoneTextures.size() && zoneTextures[d.zoneIndex]) {
            glBindTexture(GL_TEXTURE_2D, zoneTextures[d.zoneIndex]);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        m_displayQuad.draw();
    }
}

// Floor + back wall. Lazily builds two large quads and draws them with the
// objmesh shader in solid-color mode. Keeps the 3D viewport from feeling
// like a void so users can read depth and orientation at a glance.
void StageView::renderEnvironment(const glm::mat4& viewProj) {
    if (!m_envReady) {
        // Floor: 10m × 10m horizontal quad, centered at origin, y = -0.55 so
        // the default 1.08m-tall display (centered at origin) sits just above it.
        const float fHalf = 5.0f;
        const float fY    = -0.55f;
        std::vector<Vertex3D> fv = {
            {-fHalf, fY, -fHalf, 0.0f, 0.0f},
            { fHalf, fY, -fHalf, 1.0f, 0.0f},
            { fHalf, fY,  fHalf, 1.0f, 1.0f},
            {-fHalf, fY,  fHalf, 0.0f, 1.0f},
        };
        std::vector<unsigned int> fi = { 0, 1, 2, 0, 2, 3 };
        m_floorMesh.upload(fv, fi);

        // Back wall: 10m wide × 3.5m tall, at z = -2 behind the origin.
        const float wHalfX = 5.0f;
        const float wYBot  = -0.55f;
        const float wYTop  =  2.95f;
        const float wZ     = -2.0f;
        std::vector<Vertex3D> wv = {
            {-wHalfX, wYBot, wZ, 0.0f, 0.0f},
            { wHalfX, wYBot, wZ, 1.0f, 0.0f},
            { wHalfX, wYTop, wZ, 1.0f, 1.0f},
            {-wHalfX, wYTop, wZ, 0.0f, 1.0f},
        };
        std::vector<unsigned int> wi = { 0, 1, 2, 0, 2, 3 };
        m_wallMesh.upload(wv, wi);

        m_envReady = true;
    }

    if (!m_envVisible) return;

    m_shader.use();
    m_shader.setMat4("uMVP", viewProj);  // identity model — vertices are in world space
    m_shader.setBool("uTextured", false);

    // Floor — slightly lighter to catch the eye as "ground".
    m_shader.setVec4("uSolidColor", glm::vec4(0.16f, 0.17f, 0.20f, 1.0f));
    m_floorMesh.draw();

    // Back wall — a touch darker so it reads as behind the floor.
    m_shader.setVec4("uSolidColor", glm::vec4(0.12f, 0.13f, 0.16f, 1.0f));
    m_wallMesh.draw();
}

void StageView::renderScene(const std::vector<GLuint>& zoneTextures, float aspect) {
    glm::mat4 mdl = glm::scale(glm::mat4(1.0f), glm::vec3(m_modelScale));
    glm::mat4 view = m_camera.viewMatrix();
    glm::mat4 proj = m_camera.projMatrix(aspect);
    glm::mat4 mvp = proj * view * mdl;
    glm::mat4 viewProjStage = proj * view;

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    // Floor + back wall first, so displays and frustums render on top of them.
    renderEnvironment(viewProjStage);

    m_shader.use();
    m_shader.setMat4("uMVP", mvp);
    m_shader.setInt("uTexture", 0);

    // Determine which materials have projection surfaces assigned
    std::vector<int> matToZone(m_materials.size(), -1);
    for (const auto& surf : m_surfaces) {
        if (surf.active && surf.materialIndex >= 0 && surf.materialIndex < (int)m_materials.size()) {
            if (surf.projectorIndex >= 0 && surf.projectorIndex < (int)m_projectors.size()) {
                matToZone[surf.materialIndex] = m_projectors[surf.projectorIndex].zoneIndex;
            }
        }
    }

    for (int i = 0; i < (int)m_materials.size(); i++) {
        auto& mg = m_materials[i];
        if (!mg.mesh.isLoaded()) continue;

        int zoneIdx = matToZone[i];
        if (zoneIdx >= 0 && zoneIdx < (int)zoneTextures.size() && zoneTextures[zoneIdx]) {
            // This material has a projection surface - show the zone's content
            m_shader.setBool("uTextured", true);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, zoneTextures[zoneIdx]);
        } else {
            // Untextured - dark solid
            m_shader.setBool("uTextured", false);
            m_shader.setVec4("uSolidColor", glm::vec4(0.06f, 0.07f, 0.09f, 1.0f));
        }
        mg.mesh.draw();
    }

    // Wireframe overlay for structure
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    m_shader.setBool("uTextured", false);
    m_shader.setVec4("uSolidColor", glm::vec4(0.2f, 0.22f, 0.28f, 1.0f));
    for (auto& mg : m_materials) {
        if (mg.mesh.isLoaded()) mg.mesh.draw();
    }
    glDisable(GL_POLYGON_OFFSET_LINE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Draw projector frustums
    for (int i = 0; i < (int)m_projectors.size(); i++) {
        if (!m_projectors[i].visible) continue;
        renderFrustum(m_projectors[i], viewProjStage, i == m_selectedProjector);
    }

    // Draw display planes (textured quads in 3D space)
    renderDisplays(zoneTextures, viewProjStage);

    glDisable(GL_DEPTH_TEST);
}

void StageView::renderFrustum(const VirtualProjector& proj, const glm::mat4& stageVP, bool selected) {
    // Compute frustum corners in world space
    glm::mat4 invVP = glm::inverse(proj.vpMatrix());
    glm::vec3 corners[8];
    int idx = 0;
    for (float z : {0.0f, 1.0f}) {
        for (float y : {-1.0f, 1.0f}) {
            for (float x : {-1.0f, 1.0f}) {
                glm::vec4 ndc(x, y, z * 2.0f - 1.0f, 1.0f);
                glm::vec4 world = invVP * ndc;
                corners[idx++] = glm::vec3(world) / world.w;
            }
        }
    }

    // Draw lines using immediate-mode style via the shader
    // Project corners to screen and draw with ImGui foreground
    // (simpler than setting up a line VAO)
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 imgMin, imgMax;
    // We need the image region from the current ImGui context
    // This is set during renderUI, so we store it
    // For now, use the window content region
    ImVec2 wMin = ImGui::GetWindowPos();
    ImVec2 wSize = ImGui::GetWindowSize();
    float aspect = wSize.x / std::max(1.0f, wSize.y);

    auto project = [&](glm::vec3 worldPos) -> ImVec2 {
        glm::vec4 clip = stageVP * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.001f) return ImVec2(-999, -999);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = wMin.x + (ndc.x * 0.5f + 0.5f) * wSize.x;
        float sy = wMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * wSize.y;
        return ImVec2(sx, sy);
    };

    ImU32 col = selected ? IM_COL32(255, 200, 50, 200) : IM_COL32(255, 255, 255, 120);
    ImU32 colDim = selected ? IM_COL32(255, 200, 50, 60) : IM_COL32(255, 255, 255, 40);

    // Near plane
    for (int i = 0; i < 4; i++) {
        ImVec2 a = project(corners[i]);
        ImVec2 b = project(corners[(i + 1) % 4]);
        draw->AddLine(a, b, col, 1.5f);
    }
    // Far plane
    for (int i = 4; i < 8; i++) {
        ImVec2 a = project(corners[i]);
        ImVec2 b = project(corners[4 + (i - 4 + 1) % 4]);
        draw->AddLine(a, b, colDim, 1.0f);
    }
    // Edges connecting near to far
    for (int i = 0; i < 4; i++) {
        draw->AddLine(project(corners[i]), project(corners[i + 4]), colDim, 1.0f);
    }

    // Projector position dot
    ImVec2 posScreen = project(proj.position);
    draw->AddCircleFilled(posScreen, selected ? 6.0f : 4.0f, col);
    draw->AddText(ImVec2(posScreen.x + 10, posScreen.y - 8), col, proj.name.c_str());
}

void StageView::render(const std::vector<GLuint>& zoneTextures,
                       std::function<void()> inlineTopSection) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Stage");

    // Composition/output inline sections from Application live at the top
    // of the Stage tab so that canvas size and output target are set where
    // you're physically staging the projection.
    if (inlineTopSection) {
        inlineTopSection();
        ImGui::Dummy(ImVec2(0, 2));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 25));
        ImGui::Dummy(ImVec2(0, 4));
    }

    renderUI(zoneTextures);

    ImGui::End();
}

void StageView::renderToolbar() {
    // Floating pill toolbar — long rounded container with circular icon
    // buttons inside. Sits at the top of the Stage workspace as an
    // overlay above the 3D viewport. Tools, left-to-right:
    //   Floor (toggle) | Wall (toggle) | + Display | + Surface ▾ | Import
    // The Projector button is hidden until that workflow is real.
    // Mapping/Masks live in the right-rail Properties panel now,
    // not in this toolbar.
    const float kBtnSize  = 36.0f;        // diameter of each circular button
    const float kBtnGap   = 6.0f;         // gap between buttons
    const float kPillPad  = 6.0f;         // inner padding inside the pill
    const float kPillH    = kBtnSize + kPillPad * 2.0f;

    struct ToolBtn {
        const char* id;
        const char* tip;
        char glyph;          // 0..3 = floor/wall/display/surface, 4 = import
        int  kind;           // matches glyph
        bool toggleOn;       // for stateful toggles (Floor/Wall)
        bool isToggle;
    };

    bool floorOn = m_envVisible; // current Floor/Wall flag covers both
    bool wallOn  = m_envVisible;
    ToolBtn buttons[] = {
        {"##Floor",   "Show floor",       0, 0, floorOn, true},
        {"##Wall",    "Show wall",        0, 1, wallOn,  true},
        {"##AddDisp", "Add display",      0, 2, false,   false},
        {"##AddSurf", "Add surface",      0, 3, false,   false},
        {"##Import",  "Import...",        0, 4, false,   false},
    };
    const int kBtnCount = (int)(sizeof(buttons) / sizeof(buttons[0]));

    float pillW = kPillPad * 2.0f + kBtnSize * kBtnCount + kBtnGap * (kBtnCount - 1);

    // Center the pill horizontally inside the Stage panel.
    float availX = ImGui::GetContentRegionAvail().x;
    float startX = ImGui::GetCursorPosX() + std::max(0.0f, (availX - pillW) * 0.5f);
    ImVec2 pillPos = ImVec2(ImGui::GetWindowPos().x + startX,
                            ImGui::GetCursorScreenPos().y);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Pill background — same dark widget tint as other floating chrome
    // (matches the CANVAS/STAGE/SHOW segmented control track).
    dl->AddRectFilled(
        pillPos,
        ImVec2(pillPos.x + pillW, pillPos.y + kPillH),
        IM_COL32(28, 30, 34, 235),
        kPillH * 0.5f);
    dl->AddRect(
        pillPos,
        ImVec2(pillPos.x + pillW, pillPos.y + kPillH),
        IM_COL32(255, 255, 255, 24),
        kPillH * 0.5f, 0, 1.0f);

    // Per-button glyph drawing — minimal line art, scales with kBtnSize.
    auto drawGlyph = [&](ImVec2 c, int kind, ImU32 col) {
        const float r = kBtnSize * 0.32f;
        switch (kind) {
        case 0: { // Floor — horizontal slab in perspective
            dl->AddLine(ImVec2(c.x - r, c.y + r * 0.3f),
                        ImVec2(c.x + r, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y - r * 0.2f),
                        ImVec2(c.x + r * 0.6f, c.y - r * 0.2f), col, 1.4f);
            dl->AddLine(ImVec2(c.x - r, c.y + r * 0.3f),
                        ImVec2(c.x - r * 0.6f, c.y - r * 0.2f), col, 1.4f);
            dl->AddLine(ImVec2(c.x + r, c.y + r * 0.3f),
                        ImVec2(c.x + r * 0.6f, c.y - r * 0.2f), col, 1.4f);
            break; }
        case 1: { // Wall — vertical rectangle
            dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r),
                        ImVec2(c.x + r * 0.7f, c.y + r), col, 0, 0, 1.5f);
            break; }
        case 2: { // Display — screen with stand
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.7f),
                        ImVec2(c.x + r, c.y + r * 0.4f), col, 2, 0, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.4f),
                        ImVec2(c.x, c.y + r * 0.8f), col, 1.5f);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y + r * 0.8f),
                        ImVec2(c.x + r * 0.4f, c.y + r * 0.8f), col, 1.5f);
            break; }
        case 3: { // Surface — generic flat plane (rotated rect)
            dl->AddQuad(ImVec2(c.x - r, c.y - r * 0.4f),
                        ImVec2(c.x, c.y - r),
                        ImVec2(c.x + r, c.y - r * 0.4f),
                        ImVec2(c.x, c.y + r * 0.2f), col, 1.5f);
            break; }
        case 4: { // Import — down arrow into tray
            dl->AddLine(ImVec2(c.x, c.y - r * 0.8f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.1f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.1f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x - r, c.y + r * 0.7f),
                        ImVec2(c.x + r, c.y + r * 0.7f), col, 1.6f);
            break; }
        }
    };

    // Hit zones + circular fills.
    float bx = pillPos.x + kPillPad;
    float by = pillPos.y + kPillPad;
    int clickedIdx = -1;

    for (int i = 0; i < kBtnCount; i++) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushID(buttons[i].id);
        bool clicked = ImGui::InvisibleButton("##btn",
                                              ImVec2(kBtnSize, kBtnSize));
        bool hov     = ImGui::IsItemHovered();
        ImGui::PopID();

        bool active = buttons[i].isToggle && buttons[i].toggleOn;

        ImVec2 center(bx + kBtnSize * 0.5f, by + kBtnSize * 0.5f);
        ImU32 fill = active
            ? IM_COL32(247, 248, 248, 255)             // active = white
            : (hov
                ? IM_COL32(255, 255, 255, 28)
                : IM_COL32(255, 255, 255, 12));
        dl->AddCircleFilled(center, kBtnSize * 0.5f - 1.0f, fill);

        ImU32 fg = active
            ? IM_COL32(13, 18, 26, 255)
            : IM_COL32(220, 226, 235, 230);
        drawGlyph(center, buttons[i].kind, fg);

        if (hov) ImGui::SetTooltip("%s", buttons[i].tip);
        if (clicked) clickedIdx = i;

        bx += kBtnSize + kBtnGap;
    }

    // Advance the cursor past the pill so subsequent rows lay out below.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kPillH + 6.0f);

    // Action dispatch.
    if (clickedIdx == 0 || clickedIdx == 1) {
        // Floor / Wall toggle — currently both share m_envVisible.
        // (Independent toggles need a backend split that's pending.)
        m_envVisible = !m_envVisible;
    } else if (clickedIdx == 2) {
        char name[32];
        snprintf(name, sizeof(name), "Display %d", (int)m_displays.size() + 1);
        addDisplay(name, StageDisplay::Type::LED);
    } else if (clickedIdx == 3) {
        // + Surface opens a shape picker popup so the user can choose
        // a primitive (rect / circle / triangle / octagon).
        ImGui::OpenPopup("##SurfaceShapePicker");
    } else if (clickedIdx == 4) {
        m_wantsImport = true;
    }

    if (ImGui::BeginPopup("##SurfaceShapePicker")) {
        ImGui::TextDisabled("Add surface");
        ImGui::Separator();
        const char* shapes[] = {"Rectangle", "Circle", "Triangle", "Octagon"};
        for (int s = 0; s < (int)(sizeof(shapes) / sizeof(shapes[0])); s++) {
            if (ImGui::MenuItem(shapes[s])) {
                char nm[32];
                snprintf(nm, sizeof(nm), "%s %d", shapes[s], (int)m_surfaces.size() + 1);
                int matIdx = m_materials.empty() ? -1 : 0;
                int prjIdx = m_projectors.empty() ? -1 : 0;
                addSurface(nm, matIdx, prjIdx);
            }
        }
        ImGui::EndPopup();
    }
}

void StageView::renderFloatingToolbar() {
    // Vertical pill at the left edge of the viewport, where the Layers
    // panel sits in Canvas mode. Four circular icon buttons stacked
    // top-to-bottom: Room (env preset dropdown), Add Display,
    // Add Surface (shape picker), Import. Bigger buttons + more inner
    // padding so each glyph reads cleanly with breathing room.
    const float kBtnSize = 44.0f;
    const float kBtnGap  = 8.0f;
    const float kPillPad = 10.0f;
    const float kPillW   = kBtnSize + kPillPad * 2.0f;
    const int   kBtnCount = 4;
    const float kPillH   = kPillPad * 2.0f
                         + kBtnSize * kBtnCount
                         + kBtnGap  * (kBtnCount - 1);

    // Position: left edge, vertically CENTRED in the available canvas
    // height (so the rounded ends never get clipped against the
    // secondary nav above or the timeline below).
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float headerReserve = ImGui::GetFrameHeight() + 22.0f;
    float bandTop    = vp->WorkPos.y + headerReserve + 18.0f;
    float bandHeight = vp->WorkSize.y - headerReserve - 18.0f - 80.0f; // -timeline strip
    float topY       = bandTop + std::max(0.0f, (bandHeight - kPillH) * 0.5f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar;

    // Pad the host window so the pill's rounded ends fit inside its
    // content rect without being clipped by the window's own rounding.
    const float kHostExtra = 8.0f;
    ImGui::SetNextWindowPos (ImVec2(vp->WorkPos.x + 12.0f, topY - kHostExtra),
                             ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPillW + kHostExtra * 2.0f,
                                    kPillH + kHostExtra * 2.0f),
                             ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##StageToolbarFloat", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }
    ImGui::PopStyleVar(3);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Inset the pill from the host window so its rounded ends sit
    // inside the host's content rect without being clipped.
    ImVec2 pillPos(ImGui::GetWindowPos().x + kHostExtra,
                   ImGui::GetWindowPos().y + kHostExtra);
    dl->AddRectFilled(
        pillPos,
        ImVec2(pillPos.x + kPillW, pillPos.y + kPillH),
        IM_COL32(28, 30, 34, 235),
        kPillW * 0.5f);
    dl->AddRect(
        pillPos,
        ImVec2(pillPos.x + kPillW, pillPos.y + kPillH),
        IM_COL32(255, 255, 255, 24),
        kPillW * 0.5f, 0, 1.0f);

    auto drawGlyph = [&](ImVec2 c, int kind, ImU32 col) {
        const float r = kBtnSize * 0.32f;
        switch (kind) {
        case 0: { // Room — isometric box (floor + back wall + side wall)
            // Front-left rect (floor footprint)
            dl->AddLine(ImVec2(c.x - r,           c.y + r * 0.5f),
                        ImVec2(c.x + r * 0.4f,    c.y + r * 0.85f), col, 1.4f);
            dl->AddLine(ImVec2(c.x + r * 0.4f,    c.y + r * 0.85f),
                        ImVec2(c.x + r,           c.y + r * 0.5f),  col, 1.4f);
            dl->AddLine(ImVec2(c.x + r,           c.y + r * 0.5f),
                        ImVec2(c.x - r * 0.4f,    c.y + r * 0.15f), col, 1.4f);
            dl->AddLine(ImVec2(c.x - r * 0.4f,    c.y + r * 0.15f),
                        ImVec2(c.x - r,           c.y + r * 0.5f),  col, 1.4f);
            // Vertical walls — back-left and back-right edges going up
            dl->AddLine(ImVec2(c.x - r,           c.y + r * 0.5f),
                        ImVec2(c.x - r,           c.y - r * 0.3f),  col, 1.4f);
            dl->AddLine(ImVec2(c.x + r,           c.y + r * 0.5f),
                        ImVec2(c.x + r,           c.y - r * 0.3f),  col, 1.4f);
            dl->AddLine(ImVec2(c.x - r * 0.4f,    c.y + r * 0.15f),
                        ImVec2(c.x - r * 0.4f,    c.y - r * 0.65f), col, 1.4f);
            // Back wall top edge
            dl->AddLine(ImVec2(c.x - r,           c.y - r * 0.3f),
                        ImVec2(c.x - r * 0.4f,    c.y - r * 0.65f), col, 1.4f);
            dl->AddLine(ImVec2(c.x - r * 0.4f,    c.y - r * 0.65f),
                        ImVec2(c.x + r,           c.y - r * 0.3f),  col, 1.4f);
            break; }
        case 1: { // Display
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.7f),
                        ImVec2(c.x + r, c.y + r * 0.4f), col, 2, 0, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.4f),
                        ImVec2(c.x, c.y + r * 0.8f), col, 1.5f);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y + r * 0.8f),
                        ImVec2(c.x + r * 0.4f, c.y + r * 0.8f), col, 1.5f);
            break; }
        case 2: { // Surface (generic plane)
            dl->AddQuad(ImVec2(c.x - r, c.y - r * 0.4f),
                        ImVec2(c.x, c.y - r),
                        ImVec2(c.x + r, c.y - r * 0.4f),
                        ImVec2(c.x, c.y + r * 0.2f), col, 1.5f);
            break; }
        case 3: { // Import (down arrow into tray)
            dl->AddLine(ImVec2(c.x, c.y - r * 0.8f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.1f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.1f),
                        ImVec2(c.x, c.y + r * 0.3f), col, 1.6f);
            dl->AddLine(ImVec2(c.x - r, c.y + r * 0.7f),
                        ImVec2(c.x + r, c.y + r * 0.7f), col, 1.6f);
            break; }
        }
    };

    const char* tips[]     = {"Environment", "Add display", "Add surface", "Import..."};
    const bool  isToggle[] = {false,         false,         false,         false};

    float bx = pillPos.x + kPillPad;
    float by = pillPos.y + kPillPad;
    int clickedIdx = -1;
    for (int i = 0; i < kBtnCount; i++) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushID(i + 100);
        bool clicked = ImGui::InvisibleButton("##fb", ImVec2(kBtnSize, kBtnSize));
        bool hov     = ImGui::IsItemHovered();
        ImGui::PopID();
        bool active = isToggle[i] && m_envVisible;

        ImVec2 center(bx + kBtnSize * 0.5f, by + kBtnSize * 0.5f);
        ImU32 fill = active
            ? IM_COL32(247, 248, 248, 255)
            : (hov ? IM_COL32(255, 255, 255, 28) : IM_COL32(255, 255, 255, 12));
        dl->AddCircleFilled(center, kBtnSize * 0.5f - 1.0f, fill);

        ImU32 fg = active ? IM_COL32(13, 18, 26, 255)
                          : IM_COL32(220, 226, 235, 230);
        drawGlyph(center, i, fg);

        if (hov) ImGui::SetTooltip("%s", tips[i]);
        if (clicked) clickedIdx = i;

        by += kBtnSize + kBtnGap;
    }

    // Toggle sub-menus on click. Mutually exclusive — opening one
    // closes the other so we never stack two pickers on top of each
    // other next to the toolbar.
    if (clickedIdx == 0) {
        m_envMenuOpen     = !m_envMenuOpen;
        m_surfaceMenuOpen = false;
    } else if (clickedIdx == 1) {
        char name[32];
        snprintf(name, sizeof(name), "Display %d", (int)m_displays.size() + 1);
        addDisplay(name, StageDisplay::Type::LED);
        m_envMenuOpen = m_surfaceMenuOpen = false;
    } else if (clickedIdx == 2) {
        m_surfaceMenuOpen = !m_surfaceMenuOpen;
        m_envMenuOpen     = false;
    } else if (clickedIdx == 3) {
        m_wantsImport = true;
        m_envMenuOpen = m_surfaceMenuOpen = false;
    }

    ImGui::End();

    // ── Sub-menus rendered as separate peer floating windows ──
    // Anchored to the right of the toolbar's button row at the
    // appropriate Y for the button that opened them. Plain Begin
    // windows (not ImGui popups) so they don't fight with the host
    // window's borderless flags.
    auto subMenu = [&](const char* id, int btnIndex,
                       const char* title,
                       const std::initializer_list<const char*>& items,
                       bool& openFlag,
                       std::function<void(int idx)> onPick)
    {
        if (!openFlag) return;
        const float kMenuW = 200.0f;
        float menuY = pillPos.y + kPillPad + btnIndex * (kBtnSize + kBtnGap);
        float menuX = pillPos.x + kPillW + 8.0f;
        ImGui::SetNextWindowPos (ImVec2(menuX, menuY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(kMenuW, 0),    ImGuiCond_Always);
        ImGuiWindowFlags mf = ImGuiWindowFlags_NoTitleBar
                            | ImGuiWindowFlags_NoResize
                            | ImGuiWindowFlags_NoMove
                            | ImGuiWindowFlags_NoCollapse
                            | ImGuiWindowFlags_NoDocking
                            | ImGuiWindowFlags_NoSavedSettings
                            | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        if (ImGui::Begin(id, &openFlag, mf)) {
            ImGui::TextDisabled("%s", title);
            ImGui::Separator();
            int i = 0;
            for (const char* it : items) {
                if (ImGui::Selectable(it)) {
                    onPick(i);
                    openFlag = false;
                }
                i++;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    };

    subMenu("##StageEnvMenu", 0, "Environment",
            {"Floor", "Wall", "3D Room", "Seamless backdrop", "Frame"},
            m_envMenuOpen,
            [this](int idx) {
                // Every item toggles visibility for now so the click is
                // visibly responsive. Floor/Wall/Room/Seamless/Frame
                // presets will get distinct geometry when authoring lands.
                (void)idx;
                m_envVisible = !m_envVisible;
            });

    subMenu("##StageSurfaceMenu", 2, "Add surface",
            {"Rectangle", "Circle", "Triangle", "Octagon"},
            m_surfaceMenuOpen,
            [this](int idx) {
                static const char* names[] = {"Rectangle","Circle","Triangle","Octagon"};
                char nm[32];
                snprintf(nm, sizeof(nm), "%s %d", names[idx], (int)m_surfaces.size() + 1);
                int matIdx = m_materials.empty() ? -1 : 0;
                int prjIdx = m_projectors.empty() ? -1 : 0;
                addSurface(nm, matIdx, prjIdx);
            });
}

void StageView::renderUI(const std::vector<GLuint>& zoneTextures) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float panelW = ImGui::GetContentRegionAvail().x;
    float panelH = ImGui::GetContentRegionAvail().y;

    // Empty state only if BOTH materials and displays are empty.
    // A default display is added during init() so the 3D view shows a plane
    // with the live canvas content out of the box.
    if (m_materials.empty() && m_displays.empty()) {
        ImGui::Dummy(ImVec2(0, panelH * 0.3f));
        float btnW = 200.0f;
        ImGui::SetCursorPosX((panelW - btnW) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Import 3D Model", ImVec2(btnW, 40))) {
            // Open file dialog - handled by signal
            m_wantsImport = true;
        }
        ImGui::PopStyleColor(3);

        ImGui::Dummy(ImVec2(0, 10));
        float tw = ImGui::CalcTextSize("OBJ / GLTF / GLB").x;
        ImGui::SetCursorPosX((panelW - tw) * 0.5f);
        ImGui::TextDisabled("OBJ / GLTF / GLB");
        return;
    }

    // Toolbar moved out of renderUI and into renderToolbar() — host pins it
    // above the scrollable viewport child so it stays visible while Setup /
    // Scenes are open below.

    // 3D viewport area fills the full child now that the toolbar is external.
    float viewH = panelH;
    if (viewH < 100) viewH = 100;

    ImVec2 viewStart = ImGui::GetCursorScreenPos();
    ImVec2 viewSize(panelW, viewH);

    // SSAA: render FBO at 3× viewport size so the GL_LINEAR filter on the
    // display texture downsamples — cheap anti-aliasing on the 3D edges,
    // no MSAA FBO plumbing needed. 3× → 9 samples per final pixel.
    const int ssaa = 3;
    int needW = (int)viewSize.x * ssaa, needH = (int)viewSize.y * ssaa;
    if (needW < 1) needW = 1;
    if (needH < 1) needH = 1;
    if (needW != m_fboWidth || needH != m_fboHeight) {
        m_fboWidth = needW;
        m_fboHeight = needH;
        m_fbo.create(m_fboWidth, m_fboHeight, true);
    }

    // Render 3D scene to FBO
    m_fbo.bind();
    glViewport(0, 0, m_fboWidth, m_fboHeight);
    glClearColor(0.04f, 0.045f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = viewSize.x / std::max(1.0f, viewSize.y);
    renderScene(zoneTextures, aspect);

    Framebuffer::unbind();

    // Display FBO as ImGui image
    ImGui::Image((ImTextureID)(intptr_t)m_fbo.textureId(), ImVec2(viewSize.x, viewSize.y),
                 ImVec2(0, 1), ImVec2(1, 0));
    // Capture the viewport-image hover state NOW, before subsequent toolbar buttons
    // overwrite ImGui's "last item" — without this, zoom/pan/orbit only register
    // when the mouse happens to be over the "Scale" toolbar button.
    bool viewportHovered = ImGui::IsItemHovered();

    // (Move / Rotate / Scale buttons moved to the Properties panel —
    // see PropertyPanel::render Stage section. Keyboard shortcuts
    // V/R/S still toggle m_gizmoOp from anywhere in the app.)
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)) m_gizmoOp = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) m_gizmoOp = 2;
    }

    // --- 3D Gizmo overlay ---
    {
        glm::mat4 view = m_camera.viewMatrix();
        glm::mat4 proj = m_camera.projMatrix(aspect);

        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(viewStart.x, viewStart.y, viewSize.x, viewSize.y);

        // Determine gizmo operation
        ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
        if (m_gizmoOp == 1) op = ImGuizmo::ROTATE;
        else if (m_gizmoOp == 2) op = ImGuizmo::SCALE;

        // Apply gizmo to selected Display
        if (m_selectedDisplay >= 0 && m_selectedDisplay < (int)m_displays.size()) {
            auto& d = m_displays[m_selectedDisplay];
            glm::mat4 model = d.getTransform();
            float* vPtr = glm::value_ptr(view);
            float* pPtr = glm::value_ptr(proj);
            float* mPtr = glm::value_ptr(model);

            ImGuizmo::Manipulate(vPtr, pPtr, op, ImGuizmo::WORLD, mPtr);
            if (ImGuizmo::IsUsing()) {
                // Decompose back to position/rotation/scale
                float matDecompTranslation[3], matDecompRotation[3], matDecompScale[3];
                ImGuizmo::DecomposeMatrixToComponents(mPtr, matDecompTranslation, matDecompRotation, matDecompScale);
                d.position = {matDecompTranslation[0], matDecompTranslation[1], matDecompTranslation[2]};
                d.rotation = {matDecompRotation[0], matDecompRotation[1], matDecompRotation[2]};
                if (op == ImGuizmo::SCALE) {
                    d.width = std::abs(matDecompScale[0]) * 2.0f;
                    d.height = std::abs(matDecompScale[1]) * 2.0f;
                }
                m_orbitDragging = false;
            }
        }
        // Apply gizmo to selected Projector
        else if (m_selectedProjector >= 0 && m_selectedProjector < (int)m_projectors.size()) {
            auto& p = m_projectors[m_selectedProjector];
            glm::mat4 model = glm::translate(glm::mat4(1.0f), p.position);
            float* vPtr = glm::value_ptr(view);
            float* pPtr = glm::value_ptr(proj);
            float* mPtr = glm::value_ptr(model);

            ImGuizmo::Manipulate(vPtr, pPtr, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, mPtr);
            if (ImGuizmo::IsUsing()) {
                float t[3], r[3], s[3];
                ImGuizmo::DecomposeMatrixToComponents(mPtr, t, r, s);
                p.position = {t[0], t[1], t[2]};
                m_orbitDragging = false;
            }
        }
    }

    // --- Viewport interaction (UE5/Spline hybrid) ---
    // Right-drag = orbit, Middle-drag/Space+Left = pan, Scroll = distance-proportional zoom
    // Left-click = select object, Left-drag on gizmo = manipulate
    bool hovered = viewportHovered && !ImGuizmo::IsOver();
    bool gizmoUsing = ImGuizmo::IsUsing();
    ImVec2 mouse = ImGui::GetMousePos();
    float dt = ImGui::GetIO().DeltaTime;

    // --- Smooth camera animation (F to frame) ---
    if (m_cameraAnimating) {
        m_cameraAnimTime += dt;
        float t = std::min(1.0f, m_cameraAnimTime / m_cameraAnimDuration);
        // Ease-out cubic: 1 - (1-t)^3
        float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        m_camera.target = m_cameraAnimStartTarget + (m_cameraAnimEndTarget - m_cameraAnimStartTarget) * ease;
        m_camera.distance = m_cameraAnimStartDist + (m_cameraAnimEndDist - m_cameraAnimStartDist) * ease;
        if (t >= 1.0f) m_cameraAnimating = false;
    }

    // Right-drag: Orbit camera (UE5 style - direct 1:1 mapping, no inertia)
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_orbitDragging = true;
        m_orbitDragStart = {mouse.x, mouse.y};
        m_orbitStartAzimuth = m_camera.azimuth;
        m_orbitStartElevation = m_camera.elevation;
        m_cameraAnimating = false; // cancel any animation
    }
    if (m_orbitDragging && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        float dx = mouse.x - m_orbitDragStart.x;
        float dy = mouse.y - m_orbitDragStart.y;
        m_camera.azimuth = m_orbitStartAzimuth - dx * 0.005f;
        m_camera.elevation = std::max(-1.5f, std::min(1.5f, m_orbitStartElevation + dy * 0.005f));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) m_orbitDragging = false;

    // Middle-drag OR Space+Left-drag: Pan camera
    // Pan speed proportional to distance (feels natural at any zoom level)
    bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    bool panStart = (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) ||
                    (hovered && spaceHeld && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    bool panHold = (m_panDragging && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) ||
                   (m_panDragging && spaceHeld && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    if (panStart) {
        m_panDragging = true;
        m_panDragStart = {mouse.x, mouse.y};
        m_panStartTarget = m_camera.target;
        m_cameraAnimating = false;
    }
    if (m_panDragging && panHold) {
        // Pan speed scales with distance (infinite canvas feel)
        float panScale = m_camera.distance * 0.002f;
        float dx = (mouse.x - m_panDragStart.x) * panScale;
        float dy = (mouse.y - m_panDragStart.y) * panScale;
        // Pan in camera-local right/up axes
        float ca = cosf(m_camera.azimuth), sa = sinf(m_camera.azimuth);
        glm::vec3 right = {ca, 0.0f, sa};
        glm::vec3 up = {0.0f, 1.0f, 0.0f};
        m_camera.target = m_panStartTarget - right * dx + up * dy;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) ||
        (m_panDragging && !spaceHeld && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
        m_panDragging = false;
    }

    // Scroll: Distance-proportional zoom (THE key to infinite canvas feel)
    // Zoom speed = 10% of current distance per scroll notch
    if (hovered) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float zoomFactor = 1.0f - scroll * 0.1f; // 10% per notch
            float newDist = m_camera.distance * zoomFactor;
            // Soft clamp: no hard limits, just very gentle resistance at extremes
            newDist = std::max(0.01f, std::min(1000.0f, newDist));
            m_camera.distance = newDist;
            m_cameraAnimating = false;
        }
    }

    // Left-click: Select display/projector in viewport (when not using gizmo or panning)
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoUsing && !spaceHeld) {
        glm::mat4 vp = m_camera.projMatrix(aspect) * m_camera.viewMatrix();
        float bestDist = 40.0f; // pixel threshold
        int bestIdx = -1;
        // Check displays
        for (int i = 0; i < (int)m_displays.size(); i++) {
            if (!m_displays[i].visible) continue;
            glm::vec4 clip = vp * glm::vec4(m_displays[i].position, 1.0f);
            if (clip.w <= 0) continue;
            float sx = viewStart.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize.x;
            float sy = viewStart.y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * viewSize.y;
            float d = sqrtf((mouse.x-sx)*(mouse.x-sx) + (mouse.y-sy)*(mouse.y-sy));
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        // Check projectors
        for (int i = 0; i < (int)m_projectors.size(); i++) {
            if (!m_projectors[i].visible) continue;
            glm::vec4 clip = vp * glm::vec4(m_projectors[i].position, 1.0f);
            if (clip.w <= 0) continue;
            float sx = viewStart.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize.x;
            float sy = viewStart.y + (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * viewSize.y;
            float d = sqrtf((mouse.x-sx)*(mouse.x-sx) + (mouse.y-sy)*(mouse.y-sy));
            if (d < bestDist) { bestDist = d; bestIdx = -(i + 100); }
        }
        if (bestIdx >= 0) { m_selectedDisplay = bestIdx; m_selectedProjector = -1; }
        else if (bestIdx <= -100) { m_selectedProjector = -(bestIdx + 100); m_selectedDisplay = -1; }
        else { m_selectedDisplay = -1; m_selectedProjector = -1; }
    }

    // F: Frame selected (smooth animated fly-to)
    if (hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F)) {
        glm::vec3 targetPos = {0, 0, 0};
        float targetDist = 5.0f;
        bool found = false;
        if (m_selectedDisplay >= 0 && m_selectedDisplay < (int)m_displays.size()) {
            targetPos = m_displays[m_selectedDisplay].position;
            targetDist = std::max(m_displays[m_selectedDisplay].width, m_displays[m_selectedDisplay].height) * 2.5f;
            found = true;
        } else if (m_selectedProjector >= 0 && m_selectedProjector < (int)m_projectors.size()) {
            targetPos = m_projectors[m_selectedProjector].position;
            targetDist = 5.0f;
            found = true;
        }
        if (found) {
            m_cameraAnimating = true;
            m_cameraAnimTime = 0.0f;
            m_cameraAnimStartTarget = m_camera.target;
            m_cameraAnimEndTarget = targetPos;
            m_cameraAnimStartDist = m_camera.distance;
            m_cameraAnimEndDist = targetDist;
        }
    }

    // Delete/Backspace: Remove selected object
    if (!ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        if (m_selectedDisplay >= 0 && m_selectedDisplay < (int)m_displays.size()) {
            removeDisplay(m_selectedDisplay);
        } else if (m_selectedProjector >= 0 && m_selectedProjector < (int)m_projectors.size()) {
            removeProjector(m_selectedProjector);
            m_selectedProjector = -1;
        }
    }
}

// Lists of displays / projectors / surfaces / clusters — rendered in the
// separate "Scene" panel so they don't eat vertical space from the 3D
// viewport. Everything below still refers to the same member state, so
// selection / add / remove flow exactly as before.
void StageView::renderSceneInspector(const std::vector<GLuint>& zoneTextures) {
    // --- Displays list ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("Displays");
    ImGui::PopStyleColor();

    int dispRemove = -1;
    for (int i = 0; i < (int)m_displays.size(); i++) {
        ImGui::PushID(53000 + i);
        auto& d = m_displays[i];
        bool sel = (i == m_selectedDisplay);

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, sel ? 0.15f : 0.0f));
        char dlabel[128];
        snprintf(dlabel, sizeof(dlabel), "%s  [%s]", d.name.c_str(), StageDisplay::typeName(d.type));
        if (ImGui::Selectable(dlabel, sel)) {
            m_selectedDisplay = i;
        }
        ImGui::PopStyleColor();

        if (sel) {
            // Type selector
            static const char* typeNames[] = { "Projector", "LED Screen", "TV", "Monitor", "Custom" };
            int typeIdx = (int)d.type;
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("Type##disp", &typeIdx, typeNames, 5)) {
                d.type = (StageDisplay::Type)typeIdx;
            }

            // Zone assignment
            ImGui::SetNextItemWidth(-1);
            char zoneLabel[32];
            snprintf(zoneLabel, sizeof(zoneLabel), "Zone %d", d.zoneIndex);
            if (ImGui::BeginCombo("Zone##disp", zoneLabel)) {
                for (int zi = 0; zi < std::max(1, (int)zoneTextures.size()); zi++) {
                    char zl[32]; snprintf(zl, sizeof(zl), "Zone %d", zi);
                    if (ImGui::Selectable(zl, d.zoneIndex == zi)) d.zoneIndex = zi;
                }
                ImGui::EndCombo();
            }

            // Transform
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Pos##disp", &d.position[0], 0.05f, -50.0f, 50.0f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Rot##disp", &d.rotation[0], 0.5f, -180.0f, 180.0f, "%.1f");

            float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(halfW);
            ImGui::DragFloat("W##disp", &d.width, 0.01f, 0.1f, 20.0f, "%.2fm");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(halfW);
            ImGui::DragFloat("H##disp", &d.height, 0.01f, 0.1f, 20.0f, "%.2fm");

            ImGui::Checkbox("Visible##disp", &d.visible);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
            if (ImGui::SmallButton("Delete##disp")) dispRemove = i;
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (dispRemove >= 0) removeDisplay(dispRemove);

    ImGui::Dummy(ImVec2(0, 4));

    // --- Projector list ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
    ImGui::Text("Projectors");
    ImGui::PopStyleColor();

    int projRemove = -1;
    for (int i = 0; i < (int)m_projectors.size(); i++) {
        ImGui::PushID(50000 + i);
        auto& p = m_projectors[i];
        bool sel = (i == m_selectedProjector);

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, sel ? 0.15f : 0.0f));
        if (ImGui::Selectable(p.name.c_str(), sel)) {
            m_selectedProjector = i;
        }
        ImGui::PopStyleColor();

        if (sel) {
            ImGui::DragFloat3("Pos", &p.position.x, 0.1f);
            ImGui::DragFloat3("Target", &p.target.x, 0.1f);
            ImGui::DragFloat("FOV", &p.fovDeg, 0.5f, 10.0f, 170.0f);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
            if (ImGui::SmallButton("Del")) projRemove = i;
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (projRemove >= 0) removeProjector(projRemove);

    // --- Surface list ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
    ImGui::Text("Surfaces");
    ImGui::PopStyleColor();

    int surfRemove = -1;
    for (int i = 0; i < (int)m_surfaces.size(); i++) {
        ImGui::PushID(51000 + i);
        auto& s = m_surfaces[i];
        bool sel = (i == m_selectedSurface);

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, sel ? 0.15f : 0.0f));
        if (ImGui::Selectable(s.name.c_str(), sel)) {
            m_selectedSurface = i;
        }
        ImGui::PopStyleColor();

        if (sel) {
            // Material selector — guard both bounds: -1 (unassigned) would index out.
            const char* matLabel = (s.materialIndex >= 0 && s.materialIndex < (int)m_materials.size())
                ? m_materials[s.materialIndex].name.c_str() : "None";
            if (ImGui::BeginCombo("Material", matLabel)) {
                for (int m = 0; m < (int)m_materials.size(); m++) {
                    if (ImGui::Selectable(m_materials[m].name.c_str(), s.materialIndex == m)) {
                        s.materialIndex = m;
                    }
                }
                ImGui::EndCombo();
            }
            // Projector selector — guard both bounds: addSurface sets projectorIndex = -1
            // when no projectors exist; the old `< size` check allowed -1 through.
            const char* projLabel = (s.projectorIndex >= 0 && s.projectorIndex < (int)m_projectors.size())
                ? m_projectors[s.projectorIndex].name.c_str() : "None";
            if (ImGui::BeginCombo("Projector", projLabel)) {
                for (int p = 0; p < (int)m_projectors.size(); p++) {
                    if (ImGui::Selectable(m_projectors[p].name.c_str(), s.projectorIndex == p)) {
                        s.projectorIndex = p;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
            if (ImGui::SmallButton("Del")) surfRemove = i;
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (surfRemove >= 0) removeSurface(surfRemove);

    ImGui::Dummy(ImVec2(0, 6));

    // --- Screen Clusters ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
    ImGui::Text("Screen Clusters");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    if (ImGui::SmallButton("+Cluster")) {
        int idx = addCluster("Cluster " + std::to_string(m_clusters.size() + 1));
        m_selectedCluster = idx;
    }
    ImGui::PopStyleColor(3);

    int clRemove = -1;
    for (int ci = 0; ci < (int)m_clusters.size(); ci++) {
        ImGui::PushID(52000 + ci);
        auto& cl = m_clusters[ci];
        bool sel = (ci == m_selectedCluster);

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, sel ? 0.15f : 0.0f));
        if (ImGui::Selectable(cl.name.c_str(), sel)) {
            m_selectedCluster = ci;
        }
        ImGui::PopStyleColor();

        if (sel) {
            // Position
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Pos##cl", &cl.position[0], 0.05f, -50.0f, 50.0f, "%.2f");
            // Rotation
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Rot##cl", &cl.rotation[0], 0.5f, -180.0f, 180.0f, "%.1f");
            // Scale
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("Scale##cl", &cl.scale, 0.01f, 0.01f, 10.0f, "%.2f");

            ImGui::Dummy(ImVec2(0, 3));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
            ImGui::Text("Grid");
            ImGui::PopStyleColor();

            // Grid layout
            int prevCols = cl.gridCols, prevRows = cl.gridRows;
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Cols##cl", &cl.gridCols, 1, 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Rows##cl", &cl.gridRows, 1, 1);
            cl.gridCols = std::max(1, std::min(8, cl.gridCols));
            cl.gridRows = std::max(1, std::min(8, cl.gridRows));

            // Screen dimensions
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("W##clsw", &cl.screenW, 0.01f, 0.1f, 20.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("H##clsh", &cl.screenH, 0.01f, 0.1f, 20.0f, "%.2f");

            // Gap
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("Gap X##clg", &cl.gapX, 0.005f, 0.0f, 1.0f, "%.3f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("Gap Y##clg", &cl.gapY, 0.005f, 0.0f, 1.0f, "%.3f");

            // Populate grid button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            char gridLabel[64];
            snprintf(gridLabel, sizeof(gridLabel), "Generate %dx%d Grid", cl.gridCols, cl.gridRows);
            if (ImGui::Button(gridLabel, ImVec2(-1, 0))) {
                populateClusterGrid(ci, 0);
            }
            ImGui::PopStyleColor(3);

            // Count surfaces in this cluster
            int surfCount = 0;
            for (auto& s : m_surfaces) if (s.clusterIndex == ci) surfCount++;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
            ImGui::Text("%d screens", surfCount);
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
            if (ImGui::SmallButton("Del##cl")) clRemove = ci;
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (clRemove >= 0) removeCluster(clRemove);
}
