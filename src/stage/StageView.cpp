#include "stage/StageView.h"
#include <imgui.h>
#include "ui/ImGuizmo.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>

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
    // Pinned toolbar — matched styling with the rest of the app's pill buttons.
    // Left cluster: stage-edit actions. Right cluster: Mapping / Masks focus
    // buttons that bring those docked panels forward in their tab group.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.9f));

    ImGui::Checkbox("Floor/Wall", &m_envVisible);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Display")) {
        char name[32];
        snprintf(name, sizeof(name), "Display %d", (int)m_displays.size() + 1);
        addDisplay(name, StageDisplay::Type::LED);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Projector")) {
        char name[32];
        snprintf(name, sizeof(name), "Proj %d", (int)m_projectors.size() + 1);
        addProjector(name);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Surface") && !m_materials.empty()) {
        char name[32];
        snprintf(name, sizeof(name), "Screen %d", (int)m_surfaces.size() + 1);
        addSurface(name, 0, m_projectors.empty() ? -1 : 0);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Import...")) {
        m_wantsImport = true;
    }

    // Right-side cluster: Mapping / Masks focus buttons. Clicking focuses the
    // corresponding docked panel so users can jump to the related workflow
    // without leaving the Stage tab.
    ImGui::SameLine();
    float rightBtnsW = ImGui::CalcTextSize("Mapping").x
                     + ImGui::CalcTextSize("Masks").x
                     + ImGui::GetStyle().FramePadding.x * 4.0f
                     + ImGui::GetStyle().ItemSpacing.x
                     + 16.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rightBtnsW) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - rightBtnsW);
    }
    if (ImGui::SmallButton("Mapping")) ImGui::SetWindowFocus("\xE2\x96\xA2###Mapping");
    ImGui::SameLine();
    if (ImGui::SmallButton("Masks"))   ImGui::SetWindowFocus("Masks");

    ImGui::PopStyleColor(3);
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

    // --- Gizmo mode toolbar ---
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        auto gizBtn = [&](const char* label, int op) {
            bool active = (m_gizmoOp == op);
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
            if (ImGui::SmallButton(label)) m_gizmoOp = op;
            ImGui::PopStyleColor();
        };
        gizBtn("Move", 0); ImGui::SameLine();
        gizBtn("Rotate", 1); ImGui::SameLine();
        gizBtn("Scale", 2);
        // Keyboard shortcuts: V=Move, R=Rotate, S=Scale (Spline-style)
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_V)) m_gizmoOp = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_S)) m_gizmoOp = 2;
        }
        ImGui::PopStyleColor(2);
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
