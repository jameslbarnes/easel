#pragma once
#include "sources/ContentSource.h"
#include "render/ShaderProgram.h"
#include "render/Framebuffer.h"
#include "render/Mesh.h"
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

    // Long (enum) — parallel arrays for VALUES and LABELS so the UI can show
    // named pills instead of a numeric slider when labels are present.
    std::vector<int>         longValues;
    std::vector<std::string> longLabels;
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

// Signal sources for parameter binding
enum class AudioSignal {
    None = 0,
    Level,   // RMS
    Bass,
    Mid,
    High,
    Beat,    // beat decay (0-1 pulse)
    MidiCC,  // MIDI control change (uses midiCC/midiChannel fields)
};

// Per-parameter audio/MIDI binding
struct AudioBinding {
    AudioSignal signal = AudioSignal::None;
    float rangeMin = 0.0f;  // output min (maps to param min by default)
    float rangeMax = 1.0f;  // output max (maps to param max by default)
    float smoothing = 0.3f; // 0 = instant, 1 = very slow
    float smoothedValue = 0.0f; // internal state
    // MIDI fields (used when signal == MidiCC)
    int midiCC = -1;        // CC number 0-127, -1 = unassigned
    int midiChannel = -1;   // MIDI channel 0-15, -1 = any
};

class ShaderSource : public ContentSource {
public:
    ~ShaderSource() override = default;

    // Load from ISF .fs file (optionally with paired .vs)
    bool loadFromFile(const std::string& path);

    // Load from raw ISF source code (for live bridge)
    bool loadFromCode(const std::string& isfSource);

    // Recompile with new source (hot-reload)
    bool reload(const std::string& isfSource);

    void update() override;
    GLuint textureId() const override { return m_initialized ? m_fbo.textureId() : 0; }
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
    void setText(const std::string& name, const std::string& text);

    // Audio state for ISF shaders (Shader-Claw naming convention)
    void setAudioState(float rms, float bass, float mid, float high, GLuint fftTex);

    // Mouse state for interactive shaders (CFD paint, etc.)
    void setMouseState(float x, float y, bool down);

    // Bind an external texture to an ISF image input by name
    void bindImageInput(const std::string& name, GLuint texId, int w, int h, uint32_t sourceLayerId, bool flippedV = false);
    void unbindImageInput(const std::string& name);
    const std::map<std::string, ImageBinding>& imageBindings() const { return m_imageBindings; }
    std::map<std::string, ImageBinding>& imageBindings() { return m_imageBindings; }

    // Audio-reactive parameter bindings
    std::map<std::string, AudioBinding>& audioBindings() { return m_audioBindings; }
    const std::map<std::string, AudioBinding>& audioBindings() const { return m_audioBindings; }
    void applyAudioBindings(float level, float bass, float mid, float high, float beat,
                            class MIDIManager* midi = nullptr);

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

    // Image input bindings (input name -> external texture)
    std::map<std::string, ImageBinding> m_imageBindings;

    // Audio-reactive bindings (param name -> binding)
    std::map<std::string, AudioBinding> m_audioBindings;

    // Upload all uniforms to shader (pass index for multi-pass)
    void uploadUniforms(int passIndex = 0, int passWidth = 0, int passHeight = 0);
};
