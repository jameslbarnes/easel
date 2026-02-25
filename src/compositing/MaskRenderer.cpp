#include "compositing/MaskRenderer.h"
#include <iostream>
#include <vector>

MaskRenderer::~MaskRenderer() {
    destroy();
}

void MaskRenderer::destroy() {
    if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_rbo) { glDeleteRenderbuffers(1, &m_rbo); m_rbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
}

bool MaskRenderer::init() {
    if (!m_shader.loadFromFiles("shaders/mask.vert", "shaders/mask.frag")) {
        std::cerr << "Failed to load mask shaders" << std::endl;
        return false;
    }

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    return true;
}

void MaskRenderer::ensureFBO(int width, int height, GLuint colorTex) {
    bool needsCreate = (m_fbo == 0 || width != m_fboWidth || height != m_fboHeight);
    if (!needsCreate) {
        // Re-attach the color texture in case it was recreated
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_rbo) glDeleteRenderbuffers(1, &m_rbo);

    m_fboWidth = width;
    m_fboHeight = height;

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    // Attach the mask texture as color
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    // Create depth-stencil renderbuffer
    glGenRenderbuffers(1, &m_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Mask FBO incomplete!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void MaskRenderer::render(const MaskPath& path, int width, int height, Texture& outTexture) {
    if (path.count() < 3) {
        // Less than 3 points - create a fully white (no mask) texture
        if (outTexture.id() == 0) outTexture.createEmpty(width, height);
        std::vector<uint8_t> white(width * height * 4, 255);
        outTexture.updateData(white.data(), width, height);
        return;
    }

    // Ensure texture exists
    if (outTexture.id() == 0 || outTexture.width() != width || outTexture.height() != height) {
        outTexture.createEmpty(width, height);
    }

    ensureFBO(width, height, outTexture.id());

    // Tessellate the path
    auto polyVerts = path.tessellate(24);
    if (polyVerts.size() < 3) return;

    // Build triangle fan: centroid + polygon vertices + closing vertex
    glm::vec2 center = path.centroid();
    std::vector<glm::vec2> fanVerts;
    fanVerts.reserve(polyVerts.size() + 2);
    fanVerts.push_back(center);
    for (auto& v : polyVerts) fanVerts.push_back(v);
    fanVerts.push_back(polyVerts[0]); // close the fan

    // Upload vertices
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, fanVerts.size() * sizeof(glm::vec2), fanVerts.data(), GL_DYNAMIC_DRAW);

    // Build a full-screen quad for the fill pass (in UV space 0-1)
    glm::vec2 quadVerts[] = {
        {0, 0}, {1, 0}, {1, 1},
        {0, 0}, {1, 1}, {0, 1}
    };

    // Save GL state
    GLint prevFBO;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Bind mask FBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, width, height);

    // Clear to black (fully masked out)
    glClearColor(0, 0, 0, 1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    m_shader.use();
    glBindVertexArray(m_vao);

    // ---- Pass 1: Stencil fill using triangle fan ----
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    m_shader.setVec4("uColor", glm::vec4(1.0f));

    // Draw triangle fan (polygon fill with even-odd rule)
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
    glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)fanVerts.size());

    // ---- Pass 2: Fill white where stencil is set ----
    glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Upload and draw the full-screen quad
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
    m_shader.setVec4("uColor", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisable(GL_STENCIL_TEST);
    glBindVertexArray(0);

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}
