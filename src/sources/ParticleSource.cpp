#include "sources/ParticleSource.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

// ─── Small utilities ───────────────────────────────────────────────────

static float frand(uint32_t& s) {
    // Cheap LCG — we don't need cryptographic noise, just per-particle
    // variation that's stable under repeat seeds.
    s = s * 1664525u + 1013904223u;
    return (s >> 8) * (1.0f / 16777216.0f);
}
static float frand_s(uint32_t& s, float lo, float hi) {
    return lo + (hi - lo) * frand(s);
}

const char* particleSpawnShapeName(ParticleSpawnShape s) {
    switch (s) {
        case ParticleSpawnShape::Point:  return "Point";
        case ParticleSpawnShape::Sphere: return "Sphere";
        case ParticleSpawnShape::Box:    return "Box";
        case ParticleSpawnShape::Disk:   return "Disk";
        case ParticleSpawnShape::Ring:   return "Ring";
        default: return "?";
    }
}
const char* particleModuleTypeName(ParticleModuleType t) {
    switch (t) {
        case ParticleModuleType::InitialVelocity:    return "Initial Velocity";
        case ParticleModuleType::Gravity:            return "Gravity";
        case ParticleModuleType::Drag:               return "Drag";
        case ParticleModuleType::Orbital:            return "Orbital";
        case ParticleModuleType::Turbulence:         return "Turbulence";
        case ParticleModuleType::SizeOverLife:       return "Size Over Life";
        case ParticleModuleType::ColorOverLife:      return "Color Over Life";
        case ParticleModuleType::RotationOverLife:   return "Rotation Over Life";
        case ParticleModuleType::TextureSampleColor: return "Texture Sample Color";
        default: return "?";
    }
}

// ─── Inline GLSL — render shader ───────────────────────────────────────
// Instanced quad billboards. Each instance carries position (xyz) + size
// + rotation + color (rgba) packed as two vec4s.

static const char* kParticleVert = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aCorner;       // -0.5..0.5 quad corner
layout(location = 1) in vec4 aPosSize;      // xyz = position, w = size
layout(location = 2) in vec4 aRotColor;     // x = rotation (rad), yzw = rgb
layout(location = 3) in float aAlpha;       // a
out vec2 vUV;
out vec4 vColor;
uniform float uAspect;

void main() {
    float c = cos(aRotColor.x);
    float s = sin(aRotColor.x);
    vec2 rotated = mat2(c, -s, s, c) * aCorner;
    vec2 offset = rotated * aPosSize.w;
    // Screen-space billboard: ignore Z for positioning but retain it for
    // depth sort if we ever want it.
    vec2 pos2D = aPosSize.xy + offset * vec2(1.0, uAspect);
    gl_Position = vec4(pos2D, 0.0, 1.0);
    vUV = aCorner + 0.5;
    vColor = vec4(aRotColor.yzw, aAlpha);
}
)GLSL";

static const char* kParticleFrag = R"GLSL(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform int uRenderMode; // 0 = soft sprite, 1 = textured, 2 = ring

void main() {
    vec2 p = vUV * 2.0 - 1.0;
    float r = length(p);
    if (r > 1.0) discard;

    float alpha = 0.0;
    if (uRenderMode == 2) {
        // Ring: bright at edge, dark center
        alpha = 1.0 - abs(r - 0.75) * 4.0;
        alpha = clamp(alpha, 0.0, 1.0);
    } else {
        // Soft sprite — gaussian-ish falloff
        alpha = exp(-r * r * 3.0);
    }

    vec3 rgb = vColor.rgb * vColor.a;
    FragColor = vec4(rgb, alpha * vColor.a);
}
)GLSL";

// ─── ParticleSource ────────────────────────────────────────────────────

ParticleSource::ParticleSource() {
    // Default emitter — a friendly "out-of-the-box" particle system so
    // adding a particle layer shows something immediately.
    m_emitter.modules.push_back({
        ParticleModuleType::SizeOverLife,
        true, {}, {}, {}, {}, 1.0f, 0.0f, 0.0f  // sizeStart=1.0 (mul), sizeEnd=0.0
    });
    m_emitter.modules.push_back({
        ParticleModuleType::ColorOverLife,
        true, {}, {},
        {1.0f, 0.8f, 0.4f, 1.0f}, {1.0f, 0.2f, 0.0f, 0.0f},
        0.0f, 0.0f, 0.0f
    });
    m_emitter.modules.push_back({
        ParticleModuleType::Drag,
        true, {}, {}, {}, {}, 0.5f, 0.0f, 0.0f
    });
}

ParticleSource::~ParticleSource() {
    if (m_quadVBO)     glDeleteBuffers(1, &m_quadVBO);
    if (m_instanceVBO) glDeleteBuffers(1, &m_instanceVBO);
    if (m_quadVAO)     glDeleteVertexArrays(1, &m_quadVAO);
    if (m_colorCacheFBO) glDeleteFramebuffers(1, &m_colorCacheFBO);
    if (m_colorCacheTex) glDeleteTextures(1, &m_colorCacheTex);
}

void ParticleSource::refreshColorCache() {
    if (m_colorTexId == 0) return;

    // Lazy-create the downsample FBO + 64×64 attachment on first use.
    const int cacheW = 64, cacheH = 64;
    if (m_colorCacheFBO == 0) {
        glGenFramebuffers(1, &m_colorCacheFBO);
        glGenTextures(1, &m_colorCacheTex);
        glBindTexture(GL_TEXTURE_2D, m_colorCacheTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cacheW, cacheH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_colorCacheFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_colorCacheTex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_colorCacheW = cacheW;
        m_colorCacheH = cacheH;
        m_colorCache.resize(cacheW * cacheH * 4, 0);
    }

    // Blit the bound color texture into the cache FBO using the particle
    // render shader's passthrough-ish capability isn't available here. Keep
    // it simple: bind, glCopyTexImage2D-equivalent via glBlitFramebuffer using
    // a temporary read FBO with the source texture attached.
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    GLuint readFBO = 0;
    glGenFramebuffers(1, &readFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_colorTexId, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_colorCacheFBO);
        glViewport(0, 0, m_colorCacheW, m_colorCacheH);
        glBlitFramebuffer(0, 0, m_colorTexW, m_colorTexH,
                          0, 0, m_colorCacheW, m_colorCacheH,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        // Read the downsampled pixels back into the CPU cache.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_colorCacheFBO);
        glReadPixels(0, 0, m_colorCacheW, m_colorCacheH,
                     GL_RGBA, GL_UNSIGNED_BYTE, m_colorCache.data());
    }
    glDeleteFramebuffers(1, &readFBO);

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

bool ParticleSource::init(int resolutionW, int resolutionH) {
    m_w = resolutionW;
    m_h = resolutionH;

    if (!m_renderShader.loadFromSource(kParticleVert, kParticleFrag)) {
        std::cerr << "ParticleSource: failed to compile render shader\n";
        return false;
    }
    if (!m_output.create(m_w, m_h, false)) {
        std::cerr << "ParticleSource: failed to create output FBO\n";
        return false;
    }

    // Unit quad corners: 2 triangles, 4 verts
    static const float verts[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
         0.5f,  0.5f,
        -0.5f,  0.5f,
    };
    glGenVertexArrays(1, &m_quadVAO);
    glBindVertexArray(m_quadVAO);

    glGenBuffers(1, &m_quadVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Instance VBO: per-particle {posSize(vec4), rotColor(vec4), alpha(float)}
    // Laid out as 9 floats per instance, stride = 36 bytes.
    glGenBuffers(1, &m_instanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(8 * sizeof(float)));
    glVertexAttribDivisor(3, 1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // GL_ELEMENT_ARRAY for triangle-fan quad. Actually simpler: use
    // glDrawArrays with GL_TRIANGLE_FAN on 4 corners.

    m_initialized = true;
    m_lastTime = 0.0;
    return true;
}

void ParticleSource::bindColorTexture(GLuint texId, int w, int h, bool flippedV) {
    m_colorTexId = texId;
    m_colorTexW = w;
    m_colorTexH = h;
    m_colorTexFlipped = flippedV;
}

void ParticleSource::addModule(ParticleModuleType t) {
    ParticleModule m;
    m.type = t;
    // Sensible defaults per module type.
    switch (t) {
        case ParticleModuleType::Gravity:
            m.vec3A = {0.0f, -0.5f, 0.0f};
            break;
        case ParticleModuleType::Drag:
            m.floatA = 0.5f;
            break;
        case ParticleModuleType::Orbital:
            m.vec3A  = {0.0f, 0.0f, 1.0f}; // rotation axis
            m.floatA = 1.0f;                 // angular speed
            break;
        case ParticleModuleType::Turbulence:
            m.floatA = 0.3f; // strength
            m.floatB = 1.0f; // frequency
            break;
        case ParticleModuleType::SizeOverLife:
            m.floatA = 1.0f; m.floatB = 0.0f; // multiplier start → end
            break;
        case ParticleModuleType::ColorOverLife:
            m.colorA = {1.0f, 1.0f, 1.0f, 1.0f};
            m.colorB = {1.0f, 1.0f, 1.0f, 0.0f};
            break;
        case ParticleModuleType::RotationOverLife:
            m.floatA = 0.0f; m.floatB = 3.14f;
            break;
        case ParticleModuleType::TextureSampleColor:
            // No params — uses the bound color texture.
            break;
        default:
            break;
    }
    m_emitter.modules.push_back(m);
}

void ParticleSource::removeModule(int index) {
    if (index < 0 || index >= (int)m_emitter.modules.size()) return;
    m_emitter.modules.erase(m_emitter.modules.begin() + index);
}

void ParticleSource::moveModule(int index, int direction) {
    int target = index + direction;
    if (index < 0 || index >= (int)m_emitter.modules.size()) return;
    if (target < 0 || target >= (int)m_emitter.modules.size()) return;
    std::swap(m_emitter.modules[index], m_emitter.modules[target]);
}

void ParticleSource::spawnParticles(float dt) {
    m_spawnAccumulator += m_emitter.spawnRate * dt;
    int toSpawn = (int)m_spawnAccumulator;
    m_spawnAccumulator -= toSpawn;

    int room = m_emitter.maxParticles - (int)m_particles.size();
    if (toSpawn > room) toSpawn = room;
    if (toSpawn <= 0) return;

    m_particles.reserve(m_particles.size() + toSpawn);

    for (int i = 0; i < toSpawn; i++) {
        Particle p;
        p.seed = m_nextSeed++;
        uint32_t s = p.seed;

        // Spawn position
        glm::vec3 local{0.0f};
        switch (m_emitter.spawnShape) {
            case ParticleSpawnShape::Point:
                local = {0.0f, 0.0f, 0.0f};
                break;
            case ParticleSpawnShape::Sphere: {
                // Uniform in unit sphere (via rejection on the ball).
                for (int k = 0; k < 6; k++) {
                    glm::vec3 v{frand(s) * 2.0f - 1.0f,
                                 frand(s) * 2.0f - 1.0f,
                                 frand(s) * 2.0f - 1.0f};
                    if (glm::dot(v, v) <= 1.0f) { local = v; break; }
                }
                break;
            }
            case ParticleSpawnShape::Box:
                local = {frand(s) * 2.0f - 1.0f,
                         frand(s) * 2.0f - 1.0f,
                         frand(s) * 2.0f - 1.0f};
                break;
            case ParticleSpawnShape::Disk: {
                float a = frand(s) * 6.2831853f;
                float r = std::sqrt(frand(s));
                local = {std::cos(a) * r, std::sin(a) * r, 0.0f};
                break;
            }
            case ParticleSpawnShape::Ring: {
                float a = frand(s) * 6.2831853f;
                local = {std::cos(a), std::sin(a), 0.0f};
                break;
            }
            default: break;
        }
        p.position = m_emitter.spawnCenter + local * m_emitter.spawnExtents;

        // Initial velocity
        glm::vec3 jitter = {frand_s(s, -1.0f, 1.0f),
                            frand_s(s, -1.0f, 1.0f),
                            frand_s(s, -1.0f, 1.0f)};
        p.velocity = m_emitter.initialVelocity
                   + jitter * m_emitter.velocityJitter;

        // Initial size
        float sizeScale = 1.0f + (frand(s) * 2.0f - 1.0f) * m_emitter.sizeJitter * 0.5f;
        p.size      = m_emitter.initialSize * std::max(0.01f, sizeScale);
        p.sizeStart = p.size;

        // Lifetime
        p.lifetime = frand_s(s, m_emitter.lifetimeMin, m_emitter.lifetimeMax);
        p.age = 0.0f;

        // UV for texture sampling (spawn-time lookup)
        p.uv = {frand(s), frand(s)};
        p.color = {1.0f, 1.0f, 1.0f, 1.0f};
        p.colorStart = p.color;

        // Execute spawn-time modules (currently: TextureSampleColor sets
        // color at spawn by reading the bound texture at p.uv; we do this
        // on CPU by not actually reading the texture, just recording the UV
        // — the vertex shader could sample it if we bound it. For the MVP
        // we use a procedural color gradient on UV.)
        for (const auto& mod : m_emitter.modules) {
            if (!mod.enabled) continue;
            if (mod.type == ParticleModuleType::InitialVelocity) {
                p.velocity = mod.vec3A
                           + jitter * mod.randomness * glm::length(mod.vec3A);
            } else if (mod.type == ParticleModuleType::TextureSampleColor) {
                // Real sample from the bound source texture via the CPU cache.
                // The cache is a 64×64 RGBA8 downsample of m_colorTexId,
                // refreshed at m_colorCacheInterval cadence. Falls back to a
                // procedural UV gradient when no texture is bound yet.
                if (!m_colorCache.empty() && m_colorCacheW > 0 && m_colorCacheH > 0) {
                    float u = p.uv.x;
                    float v = m_colorTexFlipped ? (1.0f - p.uv.y) : p.uv.y;
                    int cx = (int)(u * (m_colorCacheW - 1));
                    int cy = (int)(v * (m_colorCacheH - 1));
                    if (cx < 0) cx = 0; if (cx >= m_colorCacheW) cx = m_colorCacheW - 1;
                    if (cy < 0) cy = 0; if (cy >= m_colorCacheH) cy = m_colorCacheH - 1;
                    const uint8_t* px = &m_colorCache[(cy * m_colorCacheW + cx) * 4];
                    p.color = {px[0] / 255.0f, px[1] / 255.0f,
                               px[2] / 255.0f, px[3] / 255.0f};
                } else {
                    p.color = {p.uv.x, 1.0f - p.uv.x, p.uv.y, 1.0f};
                }
            }
        }
        p.colorStart = p.color;

        m_particles.push_back(p);
    }
}

void ParticleSource::applyModules(float dt) {
    for (auto& p : m_particles) {
        p.age += dt;
        float lifeT = std::min(1.0f, p.age / std::max(p.lifetime, 1e-3f));

        for (const auto& mod : m_emitter.modules) {
            if (!mod.enabled) continue;
            switch (mod.type) {
                case ParticleModuleType::Gravity:
                    p.velocity += mod.vec3A * dt;
                    break;
                case ParticleModuleType::Drag:
                    // Exponential decay toward zero velocity.
                    p.velocity *= std::exp(-mod.floatA * dt);
                    break;
                case ParticleModuleType::Orbital: {
                    // Rotate velocity around axis (normalized vec3A) at floatA rad/s.
                    glm::vec3 axis = mod.vec3A;
                    float len = glm::length(axis);
                    if (len > 1e-4f) axis /= len;
                    float ang = mod.floatA * dt;
                    float c = std::cos(ang), s = std::sin(ang);
                    glm::vec3 v = p.velocity;
                    p.velocity = v * c
                               + glm::cross(axis, v) * s
                               + axis * glm::dot(axis, v) * (1.0f - c);
                    break;
                }
                case ParticleModuleType::Turbulence: {
                    // Pseudo-noise jitter — good enough for MVP.
                    uint32_t s = p.seed ^ (uint32_t)(p.age * 1000.0f);
                    glm::vec3 n{frand_s(s, -1.0f, 1.0f),
                                frand_s(s, -1.0f, 1.0f),
                                frand_s(s, -1.0f, 1.0f)};
                    p.velocity += n * (mod.floatA * dt * mod.floatB);
                    break;
                }
                case ParticleModuleType::SizeOverLife: {
                    // floatA = start mul, floatB = end mul (applied to sizeStart).
                    float k = mod.floatA + (mod.floatB - mod.floatA) * lifeT;
                    p.size = p.sizeStart * k;
                    break;
                }
                case ParticleModuleType::ColorOverLife: {
                    glm::vec4 cA = mod.colorA;
                    glm::vec4 cB = mod.colorB;
                    p.color = cA + (cB - cA) * lifeT;
                    // Tint by the spawn-time colorStart if texture-sample was
                    // active — that way texture color and gradient combine.
                    p.color.r *= p.colorStart.r;
                    p.color.g *= p.colorStart.g;
                    p.color.b *= p.colorStart.b;
                    break;
                }
                case ParticleModuleType::RotationOverLife:
                    p.rotation = mod.floatA + (mod.floatB - mod.floatA) * lifeT;
                    break;
                default: break;
            }
        }

        p.position += p.velocity * dt;
    }
}

void ParticleSource::removeDead() {
    m_particles.erase(
        std::remove_if(m_particles.begin(), m_particles.end(),
                       [](const Particle& p){ return p.age >= p.lifetime; }),
        m_particles.end());
}

void ParticleSource::uploadInstances() {
    if (m_particles.empty()) return;
    // Pack 9 floats/particle: posSize(4), rotColor(4), alpha(1).
    // Note position uses .xy for screen-space; z retained for future depth.
    int n = (int)m_particles.size();
    std::vector<float> buf;
    buf.reserve(n * 9);
    for (const auto& p : m_particles) {
        buf.push_back(p.position.x);
        buf.push_back(p.position.y);
        buf.push_back(p.position.z);
        buf.push_back(p.size);
        buf.push_back(p.rotation);
        buf.push_back(p.color.r);
        buf.push_back(p.color.g);
        buf.push_back(p.color.b);
        buf.push_back(p.color.a);
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    if (n > m_instanceCapacity) {
        glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float),
                     buf.data(), GL_DYNAMIC_DRAW);
        m_instanceCapacity = n;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, buf.size() * sizeof(float), buf.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void ParticleSource::renderParticles() {
    // Preserve state so we don't stomp the compositor's ping-pong.
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint prevBlendSrc = 0, prevBlendDst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDst);

    m_output.bind();
    glViewport(0, 0, m_w, m_h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_particles.empty()) {
        glEnable(GL_BLEND);
        if (m_emitter.additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        m_renderShader.use();
        m_renderShader.setInt("uRenderMode", m_emitter.renderMode);
        float aspect = (m_h > 0) ? (float)m_w / (float)m_h : 1.0f;
        m_renderShader.setFloat("uAspect", aspect);

        glBindVertexArray(m_quadVAO);
        glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, (GLsizei)m_particles.size());
        glBindVertexArray(0);
    }

    // Restore state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    (void)prevBlendSrc; (void)prevBlendDst;
}

void ParticleSource::update() {
    if (!m_initialized) {
        // Lazy init with default resolution — first update() call creates
        // GL resources so callers don't have to remember to init().
        if (!init(m_w > 0 ? m_w : 1920, m_h > 0 ? m_h : 1080)) return;
    }

    // dt from wall clock, clamped (same reasoning as Timeline's clamp).
    double now = glfwGetTime();
    float dt = (float)(now - m_lastTime);
    if (m_lastTime == 0.0 || dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;
    m_lastTime = now;

    // Refresh the CPU color cache on a slow tick so TextureSampleColor
    // reads stay honest to the live source without stalling every frame.
    bool needColorCache = m_colorTexId != 0;
    if (needColorCache) {
        for (const auto& mod : m_emitter.modules) {
            if (!mod.enabled) continue;
            if (mod.type == ParticleModuleType::TextureSampleColor) {
                needColorCache = true;
                break;
            }
        }
        if (needColorCache &&
            (m_colorCache.empty() || now - m_colorCacheLastFill > m_colorCacheInterval))
        {
            refreshColorCache();
            m_colorCacheLastFill = now;
        }
    }

    spawnParticles(dt);
    applyModules(dt);
    removeDead();
    uploadInstances();
    renderParticles();
}
