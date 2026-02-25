#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

class ShaderProgram {
public:
    ShaderProgram() = default;
    ~ShaderProgram();

    bool loadFromFiles(const std::string& vertPath, const std::string& fragPath);
    bool loadFromSource(const std::string& vertSrc, const std::string& fragSrc);

    void use() const;
    GLuint id() const { return m_program; }

    void setInt(const std::string& name, int value);
    void setFloat(const std::string& name, float value);
    void setBool(const std::string& name, bool value);
    void setVec2(const std::string& name, const glm::vec2& v);
    void setVec3(const std::string& name, const glm::vec3& v);
    void setVec4(const std::string& name, const glm::vec4& v);
    void setMat3(const std::string& name, const glm::mat3& m);
    void setMat4(const std::string& name, const glm::mat4& m);

private:
    GLuint m_program = 0;
    std::unordered_map<std::string, GLint> m_uniformCache;

    GLint getUniformLocation(const std::string& name);
    bool compile(GLuint shader, const std::string& source);
    bool link(GLuint vert, GLuint frag);
    std::string readFile(const std::string& path);
};
