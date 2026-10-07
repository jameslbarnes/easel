#pragma once
#include "sources/ContentSource.h"
#include "sources/AudioFeatures.h"
#include "render/ShaderProgram.h"
#include "render/Framebuffer.h"
#include "render/Mesh.h"
#include "render/GlyphBuffer.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <map>

// ISF input parameter types
struct ISFInput {
    std::string name;
    std::string type; // "float", "color", "bool", "point2D", "text"

    // Optional ISF JSON fields used only for display clarity — never touched
    // by persistence, audio/image/text bindings, or event-target lookups
    // (all of those key by `name`, confirmed safe to add without migration).
    //   "LABEL": human-readable param name, shown instead of the raw NAME.
    //   "GROUP": clusters params under a subtle sub-header in the panel;
    //            empty = ungrouped (renders exactly as before this field existed).
    std::string label;
    std::string group;

    // Current value (variant for type safety)
    std::variant<float, glm::vec4, bool, glm::vec2, std::string> value;

    // Float range
    float minVal = 0.0f;
    float maxVal = 1.0f;
    float defaultFloat = 0.0f;

    // Color default
    glm::vec4 defaultColor = {1.0f, 1.0f, 1.0f, 1.0f};

    // Bool default
    bool defaultBool = false;

    // Point2D range/default
    glm::vec2 minVec = {0.0f, 0.0f};
    glm::vec2 maxVec = {1.0f, 1.0f};
    glm::vec2 defaultVec = {0.5f, 0.5f};

    // Text default
    std::string defaultText;

    // SCTEXT/1 text-input options (Phase C). Optional ISF keys on TYPE:"text":
    //   "CASE": "upper"|"lower"|"preserve" (default preserve — the shader's
    //           decision now; the host no longer uppercases),
    //   "OVERFLOW": "head"|"tail" (default tail = keep the END of the text).
    // textShim: the GLSL body references `<name>_<digits>` — keep declaring +
    // uploading the legacy per-char int uniforms (A-Z 0..25, space 26, 0-9
    // 27..36). textBufRef: body references `textBuf_<name>` (glyph buffer
    // sampler + `<name>_cap` are declared/bound).
    std::string textCase = "preserve";
    bool textOverflowHead = false;
    bool textShim = false;
    bool textBufRef = false;
    TextRun textRun;            // current run (text + optional word timing/speaker)
    bool textDirty = true;      // re-encode + re-upload on next uploadUniforms
    GlyphEncoded textEncoded;   // last encode (len, texels, shim codes)

    // Long (enum) — parallel arrays for VALUES and LABELS so the UI can show
    // named pills instead of a numeric slider when labels are present.
    std::vector<int>         longValues;
    std::vector<std::string> longLabels;

    // Event (button) extras — set from custom ISF JSON fields:
    //   "MOMENTARY": true  -> press-and-hold; uniform mirrors held state.
    //   "TARGET": "seed"   -> on tap, randomize the named float param.
    bool        momentary  = false;
    std::string eventTarget;
};

// Ping-pong FBO pair for persistent ISF pass buffers
struct PingPongFBO {
    Framebuffer a;
    Framebuffer b;
    int current = 0; // 0: read from a, write to b; 1: read from b, write to a

    Framebuffer& readFBO()  { return current == 0 ? a : b; }
    Framebuffer& writeFBO() { return current == 0 ? b : a; }
    void swap() { current ^= 1; }
};

// ISF pass descriptor
struct ISFPass {
    std::string target;     // buffer name, empty for final pass
    bool persistent = false;
    int simWidth = 0, simHeight = 0; // downscaled size for simulation passes
    std::unique_ptr<PingPongFBO> ppFBO; // only for passes with a target
};

// External texture bound to an ISF image input
struct ImageBinding {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    uint32_t sourceLayerId = 0; // layer ID providing the texture (0 = none)
    bool flippedV = false;      // source is top-down (NDI etc.)
};

// AudioSignal + AudioBinding moved to a shared header so non-shader sources
// (FluidSource) can reuse the same binding model + follower.
#include "sources/AudioBinding.h"

class ShaderSource : public ContentSource {
public:
    ~ShaderSource() override;

    // Load from ISF .fs file (optionally with paired .vs)
    bool loadFromFile(const std::string& path);

    // GLSL compile/link error from the last failed load (for Push Further
    // retry feedback). Empty if the last load succeeded.
    std::string lastError() const { return m_lastError; }
    std::string m_lastError;

    // Load from raw ISF source code (for live bridge)
    bool loadFromCode(const std::string& isfSource);

    // Recompile with new source (hot-reload)
    bool reload(const std::string& isfSource);

    void update() override;
    GLuint textureId() const override { return m_initialized ? m_fbo.textureId() : 0; }
    bool isInitialized() const { return m_initialized; }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    std::string typeName() const override { return "Shader"; }
    std::string sourcePath() const override { return m_path; }

    // ISF parameter access
    const std::vector<ISFInput>& inputs() const { return m_inputs; }
    std::vector<ISFInput>& inputs() { return m_inputs; }

    // Set parameter by name
    void setFloat(const std::string& name, float v);
    void setColor(const std::string& name, const glm::vec4& v);
    void setBool(const std::string& name, bool v);
    void setPoint2D(const std::string& name, const glm::vec2& v);
    void setText(const std::string& name, const std::string& text);   // = setTextRun(name, TextRun{text})
    void setTextRun(const std::string& name, const TextRun& run);

    // Audio state for ISF shaders (Shader-Claw naming convention)
    void setAudioState(float rms, float bass, float mid, float high, GLuint fftTex);

    // New Audio Feature Bus — full per-frame perceptual feature set. Stores the
    // struct AND mirrors the legacy audioLevel/Bass/Mid/High/FFT fields so old
    // shaders keep working unchanged.
    void setAudioFeatures(const AudioFeatures& f);
    // Master gate for the complete feature bus and audio bindings. MIDI and
    // ordinary TIME animation remain independent. Off preserves binding recipes.
    void setAudioEnabled(bool enabled);

    // Mouse state for interactive shaders (CFD paint, etc.)
    void setMouseState(float x, float y, bool down);

    // Age (seconds) of the currently displayed `msg` text. Set by Application
    // from the typewriter timer in the DataBus binding push. Drives spawn/
    // exit lifecycle for shaders like text_clusters that animate around the
    // utterance window. -1 = no active message; 0+ = seconds since the
    // current utterance started typing.
    void setMsgAge(float seconds) { m_msgAge = seconds; }

    // Bind an external texture to an ISF image input by name
    void bindImageInput(const std::string& name, GLuint texId, int w, int h, uint32_t sourceLayerId, bool flippedV = false);
    void unbindImageInput(const std::string& name);
    const std::map<std::string, ImageBinding>& imageBindings() const { return m_imageBindings; }
    std::map<std::string, ImageBinding>& imageBindings() { return m_imageBindings; }

    // Audio-reactive parameter bindings
    std::map<std::string, AudioBinding>& audioBindings() { return m_audioBindings; }
    const std::map<std::string, AudioBinding>& audioBindings() const { return m_audioBindings; }
    void applyAudioBindings(float level, float bass, float mid, float high, float beat,
                            float dt, class MIDIManager* midi = nullptr,
                            float energy = 0.0f, float build = 0.0f, float drop = 0.0f,
                            float silence = 0.0f, float momentum = 0.5f);

    // Resolution (defaults to 1920x1080, can be changed)
    void setResolution(int w, int h);

    // ISF metadata
    const std::string& description() const { return m_description; }
    const std::string& credit() const { return m_credit; }

    bool isShader() const { return true; }

private:
    ShaderProgram m_shader;
    Framebuffer m_fbo;
    Mesh m_quad;
    int m_width = 1920;
    int m_height = 1080;
    float m_mouseX = 0.5f;
    float m_mouseY = 0.5f;
    float m_prevMouseX = 0.5f;
    float m_prevMouseY = 0.5f;
    float m_mouseDown = 0.0f;
    float m_msgAge = -1.0f;       // -1 until first text bound; else seconds-since-utterance-start
    std::string m_path;
    bool m_initialized = false;
    int m_frameIndex = 0;
    float m_lastTime = 0;

    // ISF metadata
    std::string m_description;
    std::string m_credit;
    std::vector<ISFInput> m_inputs;

    // Raw sources (kept for save/reload)
    std::string m_rawFragment;
    std::string m_rawVertex;

    // Multi-pass support
    std::vector<ISFPass> m_passes;
    std::vector<std::string> m_passBuffers; // kept for translateFragment

    // Parse ISF JSON header from shader source
    bool parseISF(const std::string& source);

    // Create ping-pong FBOs for multi-pass
    void createPassFBOs();

    // Translate ISF GLSL to OpenGL 3.3 core
    std::string translateFragment(const std::string& isfBody);
    std::string translateVertex(const std::string& isfBody);
    std::string generateDefaultVertex();

    // Audio state cached for upload
    float m_audioRMS = 0, m_audioBass = 0, m_audioMid = 0, m_audioHigh = 0;
    GLuint m_audioFFTTex = 0;
    AudioFeatures m_af;   // full feature bus (uploaded alongside legacy fields)
    bool m_audioEnabled = true;

    // Image input bindings (input name -> external texture)
    std::map<std::string, ImageBinding> m_imageBindings;

    // Audio-reactive bindings (param name -> binding)
    std::map<std::string, AudioBinding> m_audioBindings;

    // Upload all uniforms to shader (pass index for multi-pass)
    void uploadUniforms(int passIndex = 0, int passWidth = 0, int passHeight = 0);

    // SCTEXT/1 glyph-buffer textures (`textBuf_<input>`), keyed by text input
    // name and owned by this source (created lazily in uploadUniforms,
    // deleted in pruneTextBufTextures()/destructor).
    std::map<std::string, GLuint> m_textBufTex;
    void pruneTextBufTextures(bool all);
    // Cached "body references X" flags (set in parseISF from the GLSL body).
    bool m_bodyHasGlyphAtlas = false;   // `glyphAtlasTex` -> bind FontAtlas::glyphTexture() at unit 12
    bool m_bodyHasScText = false;       // `scText`/`scGlyph` -> inject the sc_text v1 helper block
    // SCFONT/1 MSDF face references (Phase D) -- units 10 (atlas) / 11 (metrics)
    bool m_bodyHasMSDFTex = false;      // `glyphMSDFTex`
    bool m_bodyHasMetricsTex = false;   // `glyphMetricsTex`
    bool m_bodyHasAtlasInfo = false;    // `glyphAtlasInfo` vec4
    bool m_bodyHasFaceInfo = false;     // `glyphFaceInfo` vec4
    bool m_loggedMSDFCollision = false; // once: >2 image inputs collide with units 10/11
    bool m_loggedTextBufOverflow = false;
};
