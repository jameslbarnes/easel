#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "render/Texture.h"
#include <iostream>

Texture::~Texture() { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : m_texture(other.m_texture), m_width(other.m_width), m_height(other.m_height) {
    other.m_texture = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        destroy();
        m_texture = other.m_texture;
        m_width = other.m_width;
        m_height = other.m_height;
        other.m_texture = 0;
    }
    return *this;
}

void Texture::destroy() {
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
}

bool Texture::loadFromFile(const std::string& path) {
    stbi_set_flip_vertically_on_load(true);
    int channels;
    unsigned char* data = stbi_load(path.c_str(), &m_width, &m_height, &channels, 4);
    if (!data) {
        std::cerr << "Failed to load image: " << path << std::endl;
        return false;
    }

    destroy();
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    stbi_image_free(data);
    return true;
}

bool Texture::createEmpty(int width, int height, GLenum internalFormat) {
    destroy();
    m_width = width;
    m_height = height;
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void Texture::updateData(const void* data, int width, int height, GLenum format, GLenum type) {
    if (!m_texture) return;
    glBindTexture(GL_TEXTURE_2D, m_texture);
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, type, data);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
    }
}
