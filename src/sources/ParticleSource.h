#pragma once
#include "sources/ContentSource.h"
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include <glad/glad.h>

class ParticleSource : public ContentSource {
public:
    ~ParticleSource() override;
    bool init(int resolutionW = 1920, int resolutionH = 1080, int particleSide = 1024);
    void update() override;
    GLuint textureId() const override { return m_output.textureId(); }
    int width() const override  { return m_output.width(); }
    int height() const override { return m_output.height(); }
    std::string typeName() const override { return "Particles"; }
    bool isShader() const override { return true; }

    // Tunable params (all 0..1 unless noted)
    float speed        = 0.6f;
    float vortex       = 0.5f;
    float chaos        = 0.35f;
    float pointSize    = 2.0f;
    float glow         = 1.0f;
    float color1[3]    = {1.0f, 1.0f, 1.0f};
    float color2[3]    = {0.2f, 0.7f, 1.0f};

private:
    // State textures (RGBA32F) — manual since Framebuffer class doesn't
    // expose a 32F variant. We hold FBO+texture pairs ourselves.
    struct StateBuf {
        GLuint fbo = 0;
        GLuint tex = 0;
    };
    bool createStateBuf(StateBuf& b, int side);
    void destroyStateBuf(StateBuf& b);

    int m_w = 0, m_h = 0;
    int m_side = 0;        // sqrt(N) — particle texture side
    int m_count = 0;       // N total
    bool m_initialized = false;
    bool m_seeded = false; // first-frame reset needed

    ShaderProgram m_updateShader;
    ShaderProgram m_renderShader;
    StateBuf m_state[2];      // ping-pong state textures (positions + velocities)
    Framebuffer m_output;     // final rendered frame the compositor reads
    int m_read = 0;           // index of the current read buffer
    Mesh m_quad;              // full-screen quad for update pass
    GLuint m_pointsVAO = 0;   // dummy VAO for instanced draw
    float m_time = 0.0f;
};
