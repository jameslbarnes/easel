#pragma once
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include "compositing/LayerStack.h"

class CompositeEngine {
public:
    bool init(int width, int height);
    void resize(int width, int height);

    // Composite all visible layers into the result FBO
    void composite(const LayerStack& stack);

    GLuint resultTexture() const;
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    Framebuffer m_fbo[2]; // ping-pong
    int m_current = 0;
    int m_width = 0, m_height = 0;

    ShaderProgram m_compositeShader;
    ShaderProgram m_passthroughShader;
    Mesh m_quad;

    void clear();
};
