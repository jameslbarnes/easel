#include "sources/ParticleSource.h"
#include <iostream>

ParticleSource::~ParticleSource() {
    destroyStateBuf(m_state[0]);
    destroyStateBuf(m_state[1]);
    if (m_pointsVAO) glDeleteVertexArrays(1, &m_pointsVAO);
    m_pointsVAO = 0;
}

bool ParticleSource::createStateBuf(StateBuf& b, int side) {
    glGenFramebuffers(1, &b.fbo);
    glGenTextures(1, &b.tex);
    glBindTexture(GL_TEXTURE_2D, b.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, side, side, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "ParticleSource: RGBA32F state FBO incomplete (0x"
                  << std::hex << status << std::dec << ")" << std::endl;
        return false;
    }
    return true;
}

void ParticleSource::destroyStateBuf(StateBuf& b) {
    if (b.fbo) glDeleteFramebuffers(1, &b.fbo);
    if (b.tex) glDeleteTextures(1, &b.tex);
    b.fbo = 0;
    b.tex = 0;
}

bool ParticleSource::init(int resolutionW, int resolutionH, int particleSide) {
    m_w = resolutionW;
    m_h = resolutionH;
    m_side = particleSide;
    m_count = m_side * m_side;

    if (!m_updateShader.loadFromFiles("shaders/passthrough.vert", "shaders/particles/update.frag")) {
        std::cerr << "ParticleSource: failed to load update shader" << std::endl;
        return false;
    }
    if (!m_renderShader.loadFromFiles("shaders/particles/render.vert", "shaders/particles/render.frag")) {
        std::cerr << "ParticleSource: failed to load render shader" << std::endl;
        return false;
    }

    if (!createStateBuf(m_state[0], m_side)) return false;
    if (!createStateBuf(m_state[1], m_side)) return false;

    if (!m_output.create(m_w, m_h, false)) {
        std::cerr << "ParticleSource: failed to create output framebuffer" << std::endl;
        return false;
    }

    m_quad.createQuad();

    glGenVertexArrays(1, &m_pointsVAO);

    m_read = 0;
    m_seeded = false;
    m_initialized = true;
    return true;
}

void ParticleSource::update() {
    if (!m_initialized) return;

    m_time += 1.0f / 60.0f;

    // Preserve GL state we'll clobber
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);

    // -------- Update pass: integrate physics into the write buffer --------
    int write = 1 - m_read;
    glBindFramebuffer(GL_FRAMEBUFFER, m_state[write].fbo);
    glViewport(0, 0, m_side, m_side);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    m_updateShader.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_state[m_read].tex);
    m_updateShader.setInt("uState", 0);
    m_updateShader.setFloat("uTime", m_time);
    m_updateShader.setFloat("uSpeed", speed);
    m_updateShader.setFloat("uVortex", vortex);
    m_updateShader.setFloat("uChaos", chaos);
    m_updateShader.setBool("uSeed", !m_seeded);
    m_quad.draw();

    m_seeded = true;
    m_read = write;

    // -------- Render pass: draw N instanced points into output FBO --------
    m_output.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);           // additive
    glDisable(GL_DEPTH_TEST);

    m_renderShader.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_state[m_read].tex);
    m_renderShader.setInt("uState", 0);
    m_renderShader.setVec2("uStateSize", glm::vec2((float)m_side, (float)m_side));
    m_renderShader.setFloat("uPointSize", pointSize);
    m_renderShader.setVec3("uColor1", glm::vec3(color1[0], color1[1], color1[2]));
    m_renderShader.setVec3("uColor2", glm::vec3(color2[0], color2[1], color2[2]));
    m_renderShader.setFloat("uGlow", glow);
    m_renderShader.setVec2("uResolution", glm::vec2((float)m_w, (float)m_h));

    glBindVertexArray(m_pointsVAO);
    glDrawArraysInstanced(GL_POINTS, 0, 1, m_count);
    glBindVertexArray(0);

    // Restore prior GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
