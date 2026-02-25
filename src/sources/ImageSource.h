#pragma once
#include "sources/ContentSource.h"
#include "render/Texture.h"
#include <string>

class ImageSource : public ContentSource {
public:
    bool load(const std::string& path);

    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_texture.width(); }
    int height() const override { return m_texture.height(); }
    std::string typeName() const override { return "Image"; }
    std::string sourcePath() const override { return m_path; }

private:
    Texture m_texture;
    std::string m_path;
};
