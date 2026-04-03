#pragma once
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include "compositing/Layer.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <glad/glad.h>

struct AudioState {
    float rms = 0;
    float bass = 0;
    float lowMid = 0;
    float highMid = 0;
    float treble = 0;
    float beatDecay = 0;
    bool beatDetected = false;
    GLuint fftTexture = 0;
    float time = 0;
    // BPM sync
    float bpm = 0;
    float beatPhase = 0;    // 0-1 sawtooth per beat
    float beatPulse = 0;    // 1.0 on beat, decays
    float barPhase = 0;     // 0-1 sawtooth per 4 beats
};

class CompositeEngine {
public:
    bool init(int width, int height);
    void resize(int width, int height);

    // Composite a list of layers into the result FBO
    void composite(const std::vector<std::shared_ptr<Layer>>& layers);

    void setAudioState(const AudioState& state) { m_audio = state; }
    float time() const { return m_audio.time; }

    GLuint resultTexture() const;
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Apply per-layer effect chain, returns texture ID to use for compositing
    GLuint applyEffects(const std::shared_ptr<Layer>& layer, GLuint srcTex);

private:
    AudioState m_audio;
    Framebuffer m_fbo[2]; // ping-pong
    int m_current = 0;
    int m_width = 0, m_height = 0;

    ShaderProgram m_compositeShader;
    ShaderProgram m_passthroughShader;
    ShaderProgram m_effectShader;
    Mesh m_quad;

    // Effect chain temp FBOs (ping-pong)
    Framebuffer m_effectFBO[2];
    // Per-layer feedback FBO (keyed by layer pointer)
    std::unordered_map<Layer*, Framebuffer> m_feedbackFBOs;

    void clear();
    void setAudioUniforms(ShaderProgram& shader, float audioStrength);
};
