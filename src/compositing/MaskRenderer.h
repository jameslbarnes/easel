#pragma once
#include "compositing/MaskPath.h"
#include "render/Texture.h"
#include "render/ShaderProgram.h"
#include <glad/glad.h>

class MaskRenderer {
public:
    ~MaskRenderer();

    bool init();

    // Render the mask path into the given texture.
    // Creates/resizes the texture as needed. Uses stencil buffer for fill.
    void render(const MaskPath& path, int width, int height, Texture& outTexture);

private:
    ShaderProgram m_shader;
    GLuint m_fbo = 0;
    GLuint m_rbo = 0;     // depth-stencil renderbuffer
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    int m_fboWidth = 0, m_fboHeight = 0;

    void ensureFBO(int width, int height, GLuint colorTex);
    void destroy();
};
