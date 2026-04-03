#include "stage/StageView.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
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

void StageView::renderScene(const std::vector<GLuint>& zoneTextures, float aspect) {
    glm::mat4 mdl = glm::scale(glm::mat4(1.0f), glm::vec3(m_modelScale));
    glm::mat4 view = m_camera.viewMatrix();
    glm::mat4 proj = m_camera.projMatrix(aspect);
    glm::mat4 mvp = proj * view * mdl;

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

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
    glm::mat4 viewProjStage = proj * view;
    for (int i = 0; i < (int)m_projectors.size(); i++) {
        if (!m_projectors[i].visible) continue;
        renderFrustum(m_projectors[i], viewProjStage, i == m_selectedProjector);
    }

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

    ImU32 col = selected ? IM_COL32(255, 200, 50, 200) : IM_COL32(0, 200, 255, 120);
    ImU32 colDim = selected ? IM_COL32(255, 200, 50, 60) : IM_COL32(0, 200, 255, 40);

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

void StageView::render(const std::vector<GLuint>& zoneTextures) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Stage");

    renderUI(zoneTextures);

    ImGui::End();
}

void StageView::renderUI(const std::vector<GLuint>& zoneTextures) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float panelW = ImGui::GetContentRegionAvail().x;
    float panelH = ImGui::GetContentRegionAvail().y;

    if (!hasModel()) {
        // No model loaded - show import button
        ImGui::Dummy(ImVec2(0, panelH * 0.3f));
        float btnW = 200.0f;
        ImGui::SetCursorPosX((panelW - btnW) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
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

    // Toolbar row
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 0.9f));

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
        ImGui::PopStyleColor(3);
    }

    // 3D viewport area
    float viewH = panelH - 120; // leave room for projector/surface lists
    if (viewH < 100) viewH = 100;

    ImVec2 viewStart = ImGui::GetCursorScreenPos();
    ImVec2 viewSize(panelW, viewH);

    // Resize FBO if needed
    int needW = (int)viewSize.x, needH = (int)viewSize.y;
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

    // Orbit camera interaction
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetMousePos();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_orbitDragging = true;
        m_orbitDragStart = {mouse.x, mouse.y};
        m_orbitStartAzimuth = m_camera.azimuth;
        m_orbitStartElevation = m_camera.elevation;
    }
    if (m_orbitDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float dx = mouse.x - m_orbitDragStart.x;
        float dy = mouse.y - m_orbitDragStart.y;
        m_camera.azimuth = m_orbitStartAzimuth - dx * 0.005f;
        m_camera.elevation = std::max(-1.5f, std::min(1.5f, m_orbitStartElevation + dy * 0.005f));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_orbitDragging = false;
    }

    // Middle mouse pan
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_panDragging = true;
        m_panDragStart = {mouse.x, mouse.y};
        m_panStartTarget = m_camera.target;
    }
    if (m_panDragging && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        float dx = (mouse.x - m_panDragStart.x) * 0.01f;
        float dy = (mouse.y - m_panDragStart.y) * 0.01f;
        m_camera.target = m_panStartTarget + glm::vec3(-dx, dy, 0.0f);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
        m_panDragging = false;
    }

    // Scroll zoom
    if (hovered) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            m_camera.distance = std::max(0.5f, std::min(50.0f, m_camera.distance - scroll * 0.5f));
        }
    }

    // --- Projector list ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.65f, 1.0f));
    ImGui::Text("Projectors");
    ImGui::PopStyleColor();

    int projRemove = -1;
    for (int i = 0; i < (int)m_projectors.size(); i++) {
        ImGui::PushID(50000 + i);
        auto& p = m_projectors[i];
        bool sel = (i == m_selectedProjector);

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, sel ? 0.15f : 0.0f));
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

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, sel ? 0.15f : 0.0f));
        if (ImGui::Selectable(s.name.c_str(), sel)) {
            m_selectedSurface = i;
        }
        ImGui::PopStyleColor();

        if (sel) {
            // Material selector
            if (ImGui::BeginCombo("Material", s.materialIndex < (int)m_materials.size() ?
                                  m_materials[s.materialIndex].name.c_str() : "None")) {
                for (int m = 0; m < (int)m_materials.size(); m++) {
                    if (ImGui::Selectable(m_materials[m].name.c_str(), s.materialIndex == m)) {
                        s.materialIndex = m;
                    }
                }
                ImGui::EndCombo();
            }
            // Projector selector
            if (ImGui::BeginCombo("Projector", s.projectorIndex < (int)m_projectors.size() ?
                                  m_projectors[s.projectorIndex].name.c_str() : "None")) {
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
}
