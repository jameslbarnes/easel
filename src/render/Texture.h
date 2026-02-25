#pragma once
#include <glad/glad.h>
#include <string>

class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    bool loadFromFile(const std::string& path);
    bool createEmpty(int width, int height, GLenum internalFormat = GL_RGBA8);
    void updateData(const void* data, int width, int height, GLenum format = GL_RGBA, GLenum type = GL_UNSIGNED_BYTE);

    void bind(int unit = 0) const;
    GLuint id() const { return m_texture; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    GLuint m_texture = 0;
    int m_width = 0;
    int m_height = 0;

    void destroy();
};
