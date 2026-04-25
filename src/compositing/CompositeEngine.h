#pragma once
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include "compositing/Layer.h"
#include <vector>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <glad/glad.h>

class ShaderSource;

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
    ShaderProgram m_shadowShader; // silhouette renderer for drop shadows
    Mesh m_quad;

    // Effect chain temp FBOs (ping-pong)
    Framebuffer m_effectFBO[2];
    // Per-layer mask combination FBOs (ping-pong for multi-mask union)
    Framebuffer m_maskFBO[2];
    // Drop shadow ping-pong FBOs (full-res silhouette + quarter-res for soft blur)
    Framebuffer m_shadowFBO[2];
    Framebuffer m_shadowLoRes[2]; // quarter resolution for smooth large blurs
    // Per-layer feedback FBO (keyed by layer ID)
    std::unordered_map<uint32_t, Framebuffer> m_feedbackFBOs;

    // GL-Transition scratch FBO — receives the A→B blend each frame a
    // gl-transitions shader is running on any layer. Single buffer is fine
    // because one layer's transition is resolved into the composite before
    // the next layer's transition begins rendering.
    Framebuffer m_glTransitionFBO;

    // Between-row transition scratch — holds the "to" layer rendered alone so
    // it can be fed as the second input to the blend shader. Separate from
    // m_glTransitionFBO to avoid conflicts when both layer-level and
    // between-row transitions run in the same frame.
    Framebuffer m_betweenRowFBO;

    // Lazy-loaded ISF transition shaders, keyed by their file path. Kept alive
    // across frames so the shader isn't recompiled every time a transition
    // window is active. Populated on first render that references the path.
    std::map<std::string, std::shared_ptr<ShaderSource>> m_isfTransitions;

    float m_lastTime = 0;

    void clear();
    void setAudioUniforms(ShaderProgram& shader, float audioStrength);
};
