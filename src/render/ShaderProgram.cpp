#include "render/ShaderProgram.h"
#include <fstream>
#include <sstream>
#include <iostream>

ShaderProgram::~ShaderProgram() {
    if (m_program) glDeleteProgram(m_program);
}

std::string ShaderProgram::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader: " << path << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool ShaderProgram::compile(GLuint shader, const std::string& source) {
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error:\n" << log << std::endl;
        // Also log to file for debugging
        {
            FILE* f = fopen("etherea_debug.log", "a");
            if (f) { fprintf(f, "SHADER COMPILE ERROR:\n%s\n---\n", log); fclose(f); }
        }
        return false;
    }
    return true;
}

bool ShaderProgram::link(GLuint vert, GLuint frag) {
    if (m_program) glDeleteProgram(m_program);
    m_program = glCreateProgram();
    glAttachShader(m_program, vert);
    glAttachShader(m_program, frag);
    glLinkProgram(m_program);

    GLint success;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::cerr << "Shader link error:\n" << log << std::endl;
        return false;
    }
    return true;
}

bool ShaderProgram::loadFromSource(const std::string& vertSrc, const std::string& fragSrc) {
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

    bool ok = compile(vert, vertSrc) && compile(frag, fragSrc) && link(vert, frag);

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (!ok && m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }

    if (ok) {
        m_uniformCache.clear();
    }

    return ok;
}

bool ShaderProgram::loadFromFiles(const std::string& vertPath, const std::string& fragPath) {
    std::string vertSrc = readFile(vertPath);
    std::string fragSrc = readFile(fragPath);
    if (vertSrc.empty() || fragSrc.empty()) return false;
    return loadFromSource(vertSrc, fragSrc);
}

void ShaderProgram::use() const {
    glUseProgram(m_program);
}

GLint ShaderProgram::getUniformLocation(const std::string& name) {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) return it->second;
    GLint loc = glGetUniformLocation(m_program, name.c_str());
    m_uniformCache[name] = loc;
    return loc;
}

void ShaderProgram::setInt(const std::string& name, int value) {
    glUniform1i(getUniformLocation(name), value);
}

void ShaderProgram::setFloat(const std::string& name, float value) {
    glUniform1f(getUniformLocation(name), value);
}

void ShaderProgram::setBool(const std::string& name, bool value) {
    glUniform1i(getUniformLocation(name), (int)value);
}

void ShaderProgram::setVec2(const std::string& name, const glm::vec2& v) {
    glUniform2fv(getUniformLocation(name), 1, &v[0]);
}

void ShaderProgram::setVec3(const std::string& name, const glm::vec3& v) {
    glUniform3fv(getUniformLocation(name), 1, &v[0]);
}

void ShaderProgram::setVec4(const std::string& name, const glm::vec4& v) {
    glUniform4fv(getUniformLocation(name), 1, &v[0]);
}

void ShaderProgram::setMat3(const std::string& name, const glm::mat3& m) {
    glUniformMatrix3fv(getUniformLocation(name), 1, GL_FALSE, &m[0][0]);
}

void ShaderProgram::setMat4(const std::string& name, const glm::mat4& m) {
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, &m[0][0]);
}
