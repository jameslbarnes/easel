#pragma once
#include "sources/ContentSource.h"
#include "render/ShaderProgram.h"
#include "render/Framebuffer.h"
#include "render/Mesh.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <variant>

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
    GLuint textureId() const override { return m_fbo.textureId(); }
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
    std::string m_path;
    bool m_initialized = false;

    // ISF metadata
    std::string m_description;
    std::string m_credit;
    std::vector<ISFInput> m_inputs;

    // Raw sources (kept for save/reload)
    std::string m_rawFragment;
    std::string m_rawVertex;

    // Parse ISF JSON header from shader source
    bool parseISF(const std::string& source);

    // Translate ISF GLSL to OpenGL 3.3 core
    std::string translateFragment(const std::string& isfBody);
    std::string translateVertex(const std::string& isfBody);
    std::string generateDefaultVertex();

    // Upload all uniforms to shader
    void uploadUniforms();
};
