#pragma once
#include <glad/glad.h>
#include <vector>

struct Vertex3D {
    float x, y, z;  // position
    float u, v;      // texcoord
};

class Mesh3D {
public:
    Mesh3D() = default;
    ~Mesh3D();

    Mesh3D(const Mesh3D&) = delete;
    Mesh3D& operator=(const Mesh3D&) = delete;
    Mesh3D(Mesh3D&& other) noexcept
        : m_vao(other.m_vao), m_vbo(other.m_vbo), m_ebo(other.m_ebo), m_indexCount(other.m_indexCount) {
        other.m_vao = other.m_vbo = other.m_ebo = 0;
        other.m_indexCount = 0;
    }
    Mesh3D& operator=(Mesh3D&& other) noexcept {
        if (this != &other) {
            destroy();
            m_vao = other.m_vao; m_vbo = other.m_vbo; m_ebo = other.m_ebo;
            m_indexCount = other.m_indexCount;
            other.m_vao = other.m_vbo = other.m_ebo = 0;
            other.m_indexCount = 0;
        }
        return *this;
    }

    void upload(const std::vector<Vertex3D>& vertices, const std::vector<unsigned int>& indices);
    void draw() const;
    bool isLoaded() const { return m_vao != 0; }

private:
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    int m_indexCount = 0;

    void destroy();
};
