#pragma once
#include <glad/glad.h>

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    bool create(int width, int height);
    void resize(int width, int height);
    void bind() const;
    static void unbind();

    GLuint textureId() const { return m_texture; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    GLuint m_fbo = 0;
    GLuint m_texture = 0;
    int m_width = 0, m_height = 0;

    void destroy();
};
