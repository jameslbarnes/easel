#pragma once
#include "render/Mesh.h"
#include "render/ShaderProgram.h"
#include "render/Framebuffer.h"
#include <glm/glm.hpp>
#include <array>

class CornerPinWarp {
public:
    bool init();

    // Set corner positions in NDC (-1 to 1)
    void setCorners(const std::array<glm::vec2, 4>& corners);
    std::array<glm::vec2, 4>& corners() { return m_corners; }
    const std::array<glm::vec2, 4>& corners() const { return m_corners; }

    // Render sourceTexture warped into the output
    void render(GLuint sourceTexture);

    // Hit test: returns corner index or -1
    int hitTest(glm::vec2 pos, float radius = 0.05f) const;

    // Draw corner handles for the editor
    void drawHandles(const glm::vec2& viewportSize) const;

private:
    // Default corners: full quad
    std::array<glm::vec2, 4> m_corners = {{
        {-1.0f, -1.0f}, // bottom-left
        { 1.0f, -1.0f}, // bottom-right
        { 1.0f,  1.0f}, // top-right
        {-1.0f,  1.0f}, // top-left
    }};

    Mesh m_mesh;
    ShaderProgram m_shader;
    int m_subdivisions = 16;

    void rebuildMesh();
};
