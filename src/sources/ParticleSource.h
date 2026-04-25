#pragma once
#include "sources/ContentSource.h"
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

// ─── Particle module model (Niagara-style) ─────────────────────────────
//
// A ParticleSystem holds ONE Emitter (first cut — multi-emitter is a follow
// -up). The emitter defines spawn behavior and owns a stack of Modules that
// execute per-particle every frame. Modules can be reordered, enabled/
// disabled, and added/removed at runtime.
//
// Simulation runs on CPU for MVP (up to ~10k particles is cheap). Rendering
// uses instanced quad billboards — each instance uploads position, color,
// size, rotation from a packed VBO. An optional ContentSource can be bound
// as a "texture sampler" so particles borrow color from an image/video/
// shader source at their spawn UV.

enum class ParticleSpawnShape {
    Point = 0,
    Sphere,
    Box,
    Disk,
    Ring,
    COUNT
};
const char* particleSpawnShapeName(ParticleSpawnShape s);

enum class ParticleModuleType {
    InitialVelocity = 0,    // spawn-time: set initial direction+speed
    Gravity,                 // per-frame: apply constant acceleration
    Drag,                    // per-frame: exponential velocity decay
    Orbital,                 // per-frame: rotate velocity around an axis
    Turbulence,              // per-frame: pseudo-noise jitter
    SizeOverLife,            // per-frame: size = lerp(start, end, t)
    ColorOverLife,           // per-frame: color = lerp(start, end, t)
    RotationOverLife,        // per-frame: spin
    TextureSampleColor,      // spawn-time: color = texture(uv)
    COUNT
};
const char* particleModuleTypeName(ParticleModuleType t);

struct ParticleModule {
    ParticleModuleType type = ParticleModuleType::InitialVelocity;
    bool enabled = true;

    // Polymorphic parameter bag — each module reads whatever it needs.
    // Keeping it flat instead of variant<> simplifies JSON ser/de and the UI.
    glm::vec3 vec3A      = {0.0f, 0.0f, 0.0f};
    glm::vec3 vec3B      = {0.0f, 0.0f, 0.0f};
    glm::vec4 colorA     = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 colorB     = {1.0f, 1.0f, 1.0f, 0.0f};
    float     floatA     = 0.0f;
    float     floatB     = 0.0f;
    float     randomness = 0.0f;  // 0=deterministic, 1=full variance per particle
};

struct ParticleEmitter {
    // Spawn config
    ParticleSpawnShape spawnShape = ParticleSpawnShape::Point;
    glm::vec3 spawnCenter  = {0.0f, 0.0f, 0.0f};
    glm::vec3 spawnExtents = {0.5f, 0.5f, 0.5f}; // radius for sphere/disk/ring, half-size for box
    float     spawnRate    = 100.0f;              // particles/sec
    int       maxParticles = 4000;
    float     lifetimeMin  = 1.5f;
    float     lifetimeMax  = 3.0f;
    glm::vec3 initialVelocity = {0.0f, 0.2f, 0.0f};
    float     velocityJitter  = 0.3f;
    float     initialSize     = 0.04f;
    float     sizeJitter      = 0.5f;

    // Render config
    bool      additive = true;                  // additive blend vs alpha blend
    bool      billboard = true;                  // always face camera (currently only mode)
    int       renderMode = 0;                    // 0 = soft sprite, 1 = textured, 2 = ring

    // Modules (stackable behaviors) — execute in order each frame
    std::vector<ParticleModule> modules;
};

class ParticleSource : public ContentSource {
public:
    ParticleSource();
    ~ParticleSource() override;

    bool init(int resolutionW = 1920, int resolutionH = 1080);

    void update() override;
    GLuint textureId() const override { return m_output.textureId(); }
    int width() const override  { return m_output.width(); }
    int height() const override { return m_output.height(); }
    std::string typeName() const override { return "Particles"; }
    bool isShader() const override { return false; }

    // Bind an arbitrary ContentSource as the "texture sampler" — particles
    // with a TextureSampleColor module will read color from this texture at
    // their spawn UV.
    void bindColorTexture(GLuint texId, int w, int h, bool flippedV);
    void clearColorTexture() { m_colorTexId = 0; }
    GLuint colorTextureId() const { return m_colorTexId; }

    // Emitter access — PropertyPanel edits this directly.
    ParticleEmitter& emitter()             { return m_emitter; }
    const ParticleEmitter& emitter() const { return m_emitter; }

    // Helpers for the UI
    void addModule(ParticleModuleType t);
    void removeModule(int index);
    void moveModule(int index, int direction); // -1 up, +1 down

    // Live stats (for debug overlay / UI)
    int liveParticleCount() const { return (int)m_particles.size(); }

private:
    struct Particle {
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        glm::vec4 color{1.0f};
        glm::vec2 uv{0.5f};        // spawn UV into the color texture (for TextureSampleColor)
        float     size       = 0.05f;
        float     sizeStart  = 0.05f;
        glm::vec4 colorStart{1.0f};
        float     rotation   = 0.0f;
        float     age        = 0.0f;
        float     lifetime   = 2.0f;
        uint32_t  seed       = 0;
    };

    void spawnParticles(float dt);
    void applyModules(float dt);
    void removeDead();
    void uploadInstances();
    void renderParticles();

    // Emitter + simulation state
    ParticleEmitter       m_emitter;
    std::vector<Particle> m_particles;
    float                 m_spawnAccumulator = 0.0f; // fractional leftover spawn budget
    uint32_t              m_nextSeed         = 1;
    double                m_lastTime         = 0.0;

    // Rendering
    Framebuffer   m_output;
    ShaderProgram m_renderShader;
    GLuint        m_quadVAO    = 0;
    GLuint        m_quadVBO    = 0;
    GLuint        m_instanceVBO= 0;
    int           m_instanceCapacity = 0;
    int           m_w = 0, m_h = 0;
    bool          m_initialized = false;

    // Bound color texture (from a sibling layer)
    GLuint m_colorTexId     = 0;
    int    m_colorTexW      = 0;
    int    m_colorTexH      = 0;
    bool   m_colorTexFlipped = false;

    // Downsampled CPU cache of the bound color texture for per-particle
    // spawn-time sampling. glGetTexImage refills it every m_colorCacheInterval
    // seconds — infrequent enough that the readback stall is cheap, frequent
    // enough that the particles track moving video content.
    std::vector<uint8_t> m_colorCache;      // RGBA8 of size cacheW×cacheH
    int   m_colorCacheW          = 0;
    int   m_colorCacheH          = 0;
    double m_colorCacheLastFill  = 0.0;
    double m_colorCacheInterval  = 0.15;
    GLuint m_colorCacheFBO       = 0;       // used to downsample the bound tex
    GLuint m_colorCacheTex       = 0;       // attachment for the downsample FBO
    void refreshColorCache();
};
