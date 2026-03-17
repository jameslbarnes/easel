#pragma once
#include <glad/glad.h>
#include <vector>

struct Vertex {
    float x, y;     // position
    float u, v;     // texcoord
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Create a simple fullscreen quad (-1 to 1)
    void createQuad();

    // Create a subdivided quad for warp (subdivisions x subdivisions)
    void createGrid(int subdivisions);

    // Upload arbitrary vertex + index data
    void upload(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);

    // Update vertex data without recreating buffers
    void updateVertices(const std::vector<Vertex>& vertices);

    void draw() const;
    void destroy();

    int indexCount() const { return m_indexCount; }

private:
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    int m_indexCount = 0;
    int m_vertexCapacity = 0;
};
