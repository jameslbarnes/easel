#include "warp/MeshWarp.h"
#include <imgui.h>
#include <vector>

bool MeshWarp::init(int cols, int rows) {
    if (!m_shader.loadFromFiles("shaders/meshwarp.vert", "shaders/passthrough.frag")) {
        return false;
    }
    setGridSize(cols, rows);
    return true;
}

glm::vec2 MeshWarp::defaultPoint(int col, int row) const {
    float u = (float)col / (m_cols - 1);
    float v = (float)row / (m_rows - 1);
    return glm::vec2(u * 2.0f - 1.0f, v * 2.0f - 1.0f);
}

void MeshWarp::setGridSize(int cols, int rows) {
    m_cols = std::max(2, cols);
    m_rows = std::max(2, rows);
    resetGrid();
}

void MeshWarp::resetGrid() {
    m_controlPoints.resize(m_cols * m_rows);
    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols; c++) {
            m_controlPoints[r * m_cols + c] = defaultPoint(c, r);
        }
    }
    rebuildMesh();
}

glm::vec2& MeshWarp::controlPoint(int col, int row) {
    return m_controlPoints[row * m_cols + col];
}

const glm::vec2& MeshWarp::controlPoint(int col, int row) const {
    return m_controlPoints[row * m_cols + col];
}

void MeshWarp::rebuildMesh() {
    std::vector<Vertex> verts;
    verts.reserve(m_cols * m_rows);

    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols; c++) {
            const auto& p = m_controlPoints[r * m_cols + c];
            float u = (float)c / (m_cols - 1);
            float v = (float)r / (m_rows - 1);
            verts.push_back({p.x, p.y, u, v});
        }
    }

    std::vector<unsigned int> indices;
    indices.reserve((m_cols - 1) * (m_rows - 1) * 6);
    for (int r = 0; r < m_rows - 1; r++) {
        for (int c = 0; c < m_cols - 1; c++) {
            unsigned int tl = r * m_cols + c;
            unsigned int tr = tl + 1;
            unsigned int bl = (r + 1) * m_cols + c;
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

void MeshWarp::render(GLuint sourceTexture) {
    // Update vertex positions from control points
    std::vector<Vertex> verts;
    verts.reserve(m_cols * m_rows);
    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols; c++) {
            const auto& p = m_controlPoints[r * m_cols + c];
            float u = (float)c / (m_cols - 1);
            float v = (float)r / (m_rows - 1);
            verts.push_back({p.x, p.y, u, v});
        }
    }
    m_mesh.updateVertices(verts);

    m_shader.use();
    m_shader.setInt("uTexture", 0);
    m_shader.setFloat("uOpacity", 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);

    m_mesh.draw();
}

int MeshWarp::hitTest(glm::vec2 pos, float radius) const {
    for (int i = 0; i < (int)m_controlPoints.size(); i++) {
        float dx = pos.x - m_controlPoints[i].x;
        float dy = pos.y - m_controlPoints[i].y;
        if (dx * dx + dy * dy < radius * radius) {
            return i;
        }
    }
    return -1;
}

void MeshWarp::drawHandles(const glm::vec2& viewportSize) const {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
    ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
    float contentW = contentMax.x - contentMin.x;
    float contentH = contentMax.y - contentMin.y;

    auto ndcToScreen = [&](glm::vec2 ndc) -> ImVec2 {
        float sx = winPos.x + contentMin.x + (ndc.x * 0.5f + 0.5f) * contentW;
        float sy = winPos.y + contentMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentH;
        return ImVec2(sx, sy);
    };

    // Draw grid lines
    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols - 1; c++) {
            ImVec2 a = ndcToScreen(m_controlPoints[r * m_cols + c]);
            ImVec2 b = ndcToScreen(m_controlPoints[r * m_cols + c + 1]);
            draw->AddLine(a, b, IM_COL32(100, 200, 255, 150), 1.0f);
        }
    }
    for (int c = 0; c < m_cols; c++) {
        for (int r = 0; r < m_rows - 1; r++) {
            ImVec2 a = ndcToScreen(m_controlPoints[r * m_cols + c]);
            ImVec2 b = ndcToScreen(m_controlPoints[(r + 1) * m_cols + c]);
            draw->AddLine(a, b, IM_COL32(100, 200, 255, 150), 1.0f);
        }
    }

    // Draw control points
    for (int i = 0; i < (int)m_controlPoints.size(); i++) {
        ImVec2 p = ndcToScreen(m_controlPoints[i]);
        draw->AddCircleFilled(p, 5.0f, IM_COL32(100, 200, 255, 255));
        draw->AddCircle(p, 5.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
    }
}

void MeshWarp::addColumn() {
    int newCols = m_cols + 1;
    std::vector<glm::vec2> newPoints(newCols * m_rows);

    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols; c++) {
            newPoints[r * newCols + c] = m_controlPoints[r * m_cols + c];
        }
        // New column: interpolate between last column and edge
        newPoints[r * newCols + m_cols] = defaultPoint(m_cols, r);
    }

    m_cols = newCols;
    m_controlPoints = newPoints;
    rebuildMesh();
}

void MeshWarp::removeColumn() {
    if (m_cols <= 2) return;
    int newCols = m_cols - 1;
    std::vector<glm::vec2> newPoints(newCols * m_rows);

    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < newCols; c++) {
            newPoints[r * newCols + c] = m_controlPoints[r * m_cols + c];
        }
    }

    m_cols = newCols;
    m_controlPoints = newPoints;
    rebuildMesh();
}

void MeshWarp::addRow() {
    int newRows = m_rows + 1;
    std::vector<glm::vec2> newPoints(m_cols * newRows);

    for (int r = 0; r < m_rows; r++) {
        for (int c = 0; c < m_cols; c++) {
            newPoints[r * m_cols + c] = m_controlPoints[r * m_cols + c];
        }
    }
    for (int c = 0; c < m_cols; c++) {
        newPoints[m_rows * m_cols + c] = defaultPoint(c, m_rows);
    }

    m_rows = newRows;
    m_controlPoints = newPoints;
    rebuildMesh();
}

void MeshWarp::removeRow() {
    if (m_rows <= 2) return;
    int newRows = m_rows - 1;
    std::vector<glm::vec2> newPoints(m_cols * newRows);

    for (int r = 0; r < newRows; r++) {
        for (int c = 0; c < m_cols; c++) {
            newPoints[r * m_cols + c] = m_controlPoints[r * m_cols + c];
        }
    }

    m_rows = newRows;
    m_controlPoints = newPoints;
    rebuildMesh();
}
