#pragma once
#include "render/Mesh.h"
#include "render/ShaderProgram.h"
#include <glm/glm.hpp>
#include <vector>

class MeshWarp {
public:
    bool init(int cols = 4, int rows = 4);

    void setGridSize(int cols, int rows);
    int cols() const { return m_cols; }
    int rows() const { return m_rows; }

    // Get/set control point positions (in NDC)
    glm::vec2& controlPoint(int col, int row);
    const glm::vec2& controlPoint(int col, int row) const;

    // Reset all control points to default grid positions
    void resetGrid();

    void render(GLuint sourceTexture);

    // Hit test: returns flat index or -1
    int hitTest(glm::vec2 pos, float radius = 0.04f) const;

    // Draw grid lines and control points
    void drawHandles(const glm::vec2& viewportSize) const;

    // Add/remove rows and columns
    void addColumn();
    void removeColumn();
    void addRow();
    void removeRow();

    std::vector<glm::vec2>& points() { return m_controlPoints; }
    const std::vector<glm::vec2>& points() const { return m_controlPoints; }

private:
    int m_cols = 4, m_rows = 4;
    std::vector<glm::vec2> m_controlPoints;
    Mesh m_mesh;
    ShaderProgram m_shader;

    void rebuildMesh();
    glm::vec2 defaultPoint(int col, int row) const;
};
