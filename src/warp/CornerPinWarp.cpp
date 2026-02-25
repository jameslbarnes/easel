#include "warp/CornerPinWarp.h"
#include "warp/HomographyUtils.h"
#include <vector>
#include <imgui.h>

bool CornerPinWarp::init() {
    if (!m_shader.loadFromFiles("shaders/cornerpin.vert", "shaders/passthrough.frag")) {
        return false;
    }
    rebuildMesh();
    return true;
}

void CornerPinWarp::setCorners(const std::array<glm::vec2, 4>& corners) {
    m_corners = corners;
}

void CornerPinWarp::rebuildMesh() {
    int n = m_subdivisions + 1;
    std::vector<Vertex> verts;
    verts.reserve(n * n);

    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            float u = (float)i / m_subdivisions;
            float v = (float)j / m_subdivisions;
            float x = u * 2.0f - 1.0f;
            float y = v * 2.0f - 1.0f;
            verts.push_back({x, y, u, v});
        }
    }

    std::vector<unsigned int> indices;
    indices.reserve(m_subdivisions * m_subdivisions * 6);
    for (int j = 0; j < m_subdivisions; j++) {
        for (int i = 0; i < m_subdivisions; i++) {
            unsigned int tl = j * n + i;
            unsigned int tr = tl + 1;
            unsigned int bl = (j + 1) * n + i;
            unsigned int br = bl + 1;
            indices.push_back(tl);
            indices.push_back(tr);
            indices.push_back(br);
            indices.push_back(tl);
            indices.push_back(br);
            indices.push_back(bl);
        }
    }

    m_mesh.upload(verts, indices);
}

void CornerPinWarp::render(GLuint sourceTexture) {
    // Source corners (unit quad in NDC)
    std::array<glm::vec2, 4> src = {{
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}
    }};

    glm::mat3 H = HomographyUtils::solve(src, m_corners);

    m_shader.use();
    m_shader.setMat3("uHomography", H);
    m_shader.setInt("uTexture", 0);
    m_shader.setFloat("uOpacity", 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);

    m_mesh.draw();
}

int CornerPinWarp::hitTest(glm::vec2 pos, float radius) const {
    for (int i = 0; i < 4; i++) {
        float dx = pos.x - m_corners[i].x;
        float dy = pos.y - m_corners[i].y;
        if (dx * dx + dy * dy < radius * radius) {
            return i;
        }
    }
    return -1;
}

void CornerPinWarp::drawHandles(const glm::vec2& viewportSize) const {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();

    // Padding for the viewport area within the window
    ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
    ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
    float contentW = contentMax.x - contentMin.x;
    float contentH = contentMax.y - contentMin.y;

    auto ndcToScreen = [&](glm::vec2 ndc) -> ImVec2 {
        float sx = winPos.x + contentMin.x + (ndc.x * 0.5f + 0.5f) * contentW;
        float sy = winPos.y + contentMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentH;
        return ImVec2(sx, sy);
    };

    const char* labels[] = {"BL", "BR", "TR", "TL"};

    // Draw edges
    for (int i = 0; i < 4; i++) {
        ImVec2 a = ndcToScreen(m_corners[i]);
        ImVec2 b = ndcToScreen(m_corners[(i + 1) % 4]);
        draw->AddLine(a, b, IM_COL32(255, 255, 0, 200), 2.0f);
    }

    // Draw corner handles
    for (int i = 0; i < 4; i++) {
        ImVec2 p = ndcToScreen(m_corners[i]);
        draw->AddCircleFilled(p, 8.0f, IM_COL32(255, 200, 0, 255));
        draw->AddCircle(p, 8.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
        draw->AddText(ImVec2(p.x + 12, p.y - 8), IM_COL32(255, 255, 255, 255), labels[i]);
    }
}
