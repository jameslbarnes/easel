#include "render/Mesh.h"

Mesh::~Mesh() { destroy(); }

void Mesh::destroy() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    m_vao = m_vbo = m_ebo = 0;
}

void Mesh::createQuad() {
    std::vector<Vertex> verts = {
        {-1.0f, -1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 0.0f},
        { 1.0f,  1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f},
    };
    std::vector<unsigned int> indices = {0, 1, 2, 0, 2, 3};
    upload(verts, indices);
}

void Mesh::createGrid(int subdivisions) {
    int n = subdivisions + 1;
    std::vector<Vertex> verts;
    verts.reserve(n * n);

    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            float u = (float)i / subdivisions;
            float v = (float)j / subdivisions;
            // Map to NDC: -1 to 1
            float x = u * 2.0f - 1.0f;
            float y = v * 2.0f - 1.0f;
            verts.push_back({x, y, u, v});
        }
    }

    std::vector<unsigned int> indices;
    indices.reserve(subdivisions * subdivisions * 6);
    for (int j = 0; j < subdivisions; j++) {
        for (int i = 0; i < subdivisions; i++) {
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
    upload(verts, indices);
}

void Mesh::upload(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
    destroy();

    m_indexCount = (int)indices.size();
    m_vertexCapacity = (int)vertices.size();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    // Position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // TexCoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void Mesh::updateVertices(const std::vector<Vertex>& vertices) {
    if (!m_vbo) return;
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(Vertex), vertices.data());
}

void Mesh::draw() const {
    if (!m_vao) return;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}
