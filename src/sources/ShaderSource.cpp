#include "sources/ShaderSource.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <GLFW/glfw3.h>

using json = nlohmann::json;

// --- ISF JSON Parsing ---

// Helper: get a bool from JSON that might be bool, int, or string
static bool jsonToBool(const json& j, bool fallback) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<int>() != 0;
    if (j.is_string()) return j.get<std::string>() == "true" || j.get<std::string>() == "1";
    return fallback;
}

// Helper: get a float from JSON that might be float, int, or string
static float jsonToFloat(const json& j, float fallback) {
    if (j.is_number()) return j.get<float>();
    if (j.is_string()) {
        try { return std::stof(j.get<std::string>()); } catch (...) {}
    }
    return fallback;
}

bool ShaderSource::parseISF(const std::string& source) {
    // ISF shaders have a JSON block inside /* { ... } */
    auto start = source.find("/*");
    if (start == std::string::npos) return false;

    // Find the opening brace within the comment
    auto jsonStart = source.find('{', start);
    if (jsonStart == std::string::npos) return false;

    // Find the closing */ to bound our search
    auto commentEnd = source.find("*/", start);
    if (commentEnd == std::string::npos) return false;

    // Find matching closing brace (handle nested braces) within the comment
    int depth = 0;
    size_t jsonEnd = std::string::npos;
    for (size_t i = jsonStart; i < commentEnd; i++) {
        if (source[i] == '{') depth++;
        else if (source[i] == '}') {
            depth--;
            if (depth == 0) {
                jsonEnd = i;
                break;
            }
        }
    }
    if (jsonEnd == std::string::npos) return false;

    std::string jsonStr = source.substr(jsonStart, jsonEnd - jsonStart + 1);

    try {
        json j = json::parse(jsonStr);

        m_description = j.value("DESCRIPTION", "");
        m_credit = j.value("CREDIT", "");

        m_inputs.clear();
        if (j.contains("INPUTS") && j["INPUTS"].is_array()) {
            for (const auto& input : j["INPUTS"]) {
                ISFInput param;
                param.name = input.value("NAME", "");
                param.type = input.value("TYPE", "float");

                if (param.type == "float") {
                    param.minVal = input.contains("MIN") ? jsonToFloat(input["MIN"], 0.0f) : 0.0f;
                    param.maxVal = input.contains("MAX") ? jsonToFloat(input["MAX"], 1.0f) : 1.0f;
                    param.defaultFloat = input.contains("DEFAULT") ? jsonToFloat(input["DEFAULT"], 0.5f) : 0.5f;
                    param.value = param.defaultFloat;
                } else if (param.type == "color") {
                    param.defaultColor = {1.0f, 1.0f, 1.0f, 1.0f};
                    if (input.contains("DEFAULT") && input["DEFAULT"].is_array()) {
                        auto& def = input["DEFAULT"];
                        if (def.size() >= 4) {
                            param.defaultColor = {
                                def[0].get<float>(), def[1].get<float>(),
                                def[2].get<float>(), def[3].get<float>()
                            };
                        } else if (def.size() >= 3) {
                            param.defaultColor = {
                                def[0].get<float>(), def[1].get<float>(),
                                def[2].get<float>(), 1.0f
                            };
                        }
                    }
                    param.value = param.defaultColor;
                } else if (param.type == "bool") {
                    param.defaultBool = input.contains("DEFAULT") ? jsonToBool(input["DEFAULT"], false) : false;
                    param.value = param.defaultBool;
                } else if (param.type == "point2D") {
                    param.defaultVec = {0.5f, 0.5f};
                    param.minVec = {0.0f, 0.0f};
                    param.maxVec = {1.0f, 1.0f};
                    if (input.contains("DEFAULT") && input["DEFAULT"].is_array()) {
                        auto& def = input["DEFAULT"];
                        if (def.size() >= 2) {
                            param.defaultVec = {def[0].get<float>(), def[1].get<float>()};
                        }
                    }
                    if (input.contains("MIN") && input["MIN"].is_array()) {
                        auto& m = input["MIN"];
                        if (m.size() >= 2) param.minVec = {m[0].get<float>(), m[1].get<float>()};
                    }
                    if (input.contains("MAX") && input["MAX"].is_array()) {
                        auto& m = input["MAX"];
                        if (m.size() >= 2) param.maxVec = {m[0].get<float>(), m[1].get<float>()};
                    }
                    param.value = param.defaultVec;
                } else if (param.type == "long") {
                    // Enumeration type — represented as int with VALUES array
                    param.type = "long";
                    param.minVal = 0;
                    param.maxVal = 0;
                    if (input.contains("VALUES") && input["VALUES"].is_array()) {
                        param.maxVal = (float)(input["VALUES"].size() - 1);
                    }
                    param.defaultFloat = input.contains("DEFAULT") ? jsonToFloat(input["DEFAULT"], 0.0f) : 0.0f;
                    param.value = param.defaultFloat;
                    // Store labels for UI
                    if (input.contains("LABELS") && input["LABELS"].is_array()) {
                        // Store in description for now
                        std::string labels;
                        for (const auto& l : input["LABELS"]) {
                            if (!labels.empty()) labels += "|";
                            labels += l.get<std::string>();
                        }
                        // We'll use this later for dropdown UI
                    }
                } else if (param.type == "event") {
                    // Event trigger — behaves like a momentary bool
                    param.defaultBool = false;
                    param.value = false;
                } else if (param.type == "text") {
                    // Text input — Shader-Claw compiles to NAME_0..NAME_N + NAME_len uniforms
                    // Store default text and max length
                    if (input.contains("MAX_LENGTH") && input["MAX_LENGTH"].is_number()) {
                        param.maxVal = input["MAX_LENGTH"].get<float>();
                    } else {
                        param.maxVal = 12.0f;
                    }
                    param.defaultText = input.value("DEFAULT", "");
                    // Uppercase the default text
                    for (auto& c : param.defaultText) c = (char)toupper((unsigned char)c);
                    param.value = param.defaultText;
                } else if (param.type == "image") {
                    // Image input — texture sampler, stub for generators
                    param.value = false; // placeholder
                }

                if (!param.name.empty()) {
                    m_inputs.push_back(std::move(param));
                }
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "ISF parse error: " << e.what() << std::endl;
        return false;
    }

    return true;
}

// --- ISF -> GLSL 330 core translation ---

std::string ShaderSource::translateFragment(const std::string& isfBody) {
    // Strip the ISF JSON comment block
    std::string body = isfBody;
    auto commentStart = body.find("/*");
    if (commentStart != std::string::npos) {
        auto commentEnd = body.find("*/", commentStart);
        if (commentEnd != std::string::npos) {
            body = body.substr(commentEnd + 2);
        }
    }

    // Strip any embedded #version directive (we provide our own)
    std::regex versionRe(R"(#version\s+\d+[^\n]*)");
    body = std::regex_replace(body, versionRe, "// (version stripped by Easel)");

    // Replace legacy GLSL keywords for 330 core compatibility
    // varying -> in (for fragment shader)
    body = std::regex_replace(body, std::regex(R"(\bvarying\b)"), "in");
    // texture2D -> texture
    body = std::regex_replace(body, std::regex(R"(\btexture2D\b)"), "texture");

    std::stringstream out;
    out << "#version 330 core\n";
    out << "out vec4 FragColor;\n";
    out << "in vec2 isf_FragNormCoord;\n";
    out << "\n";

    // ISF built-in uniforms
    out << "uniform float TIME;\n";
    out << "uniform vec2 RENDERSIZE;\n";
    out << "uniform int PASSINDEX;\n";
    out << "uniform int FRAMEINDEX;\n";
    out << "\n";

    // ISF image input stubs — provide dummy samplers and macros
    bool hasImageInputs = false;
    for (const auto& input : m_inputs) {
        if (input.type == "image") {
            hasImageInputs = true;
            out << "uniform sampler2D " << input.name << ";\n";
        }
    }
    if (hasImageInputs) {
        // ISF texture sampling macros
        out << "#define IMG_NORM_PIXEL(img, coord) texture(img, coord)\n";
        out << "#define IMG_THIS_NORM_PIXEL(img) texture(img, isf_FragNormCoord)\n";
        out << "#define IMG_PIXEL(img, coord) texture(img, (coord) / RENDERSIZE)\n";
        out << "#define IMG_THIS_PIXEL(img) texture(img, gl_FragCoord.xy / RENDERSIZE)\n";
        out << "#define IMG_SIZE(img) RENDERSIZE\n";
        out << "\n";
    }

    // Declare user ISF INPUTS as uniforms
    for (const auto& input : m_inputs) {
        if (input.type == "float") {
            out << "uniform float " << input.name << ";\n";
        } else if (input.type == "color") {
            out << "uniform vec4 " << input.name << ";\n";
        } else if (input.type == "bool" || input.type == "event") {
            out << "uniform bool " << input.name << ";\n";
        } else if (input.type == "point2D") {
            out << "uniform vec2 " << input.name << ";\n";
        } else if (input.type == "long") {
            out << "uniform int " << input.name << ";\n";
        } else if (input.type == "text") {
            // Text inputs in Shader-Claw compile to int arrays: name_0..name_N + name_len
            int maxLen = (int)input.maxVal;
            if (maxLen <= 0) maxLen = 12;
            out << "uniform int " << input.name << "_len;\n";
            for (int i = 0; i < maxLen; i++) {
                out << "uniform int " << input.name << "_" << i << ";\n";
            }
        }
        // image type already declared above
    }
    out << "\n";

    // Redirect gl_FragColor -> FragColor
    out << "#define gl_FragColor FragColor\n";
    out << "\n";
    out << body << "\n";

    return out.str();
}

std::string ShaderSource::translateVertex(const std::string& isfBody) {
    // Strip ISF comment if present
    std::string body = isfBody;
    auto commentStart = body.find("/*");
    if (commentStart != std::string::npos) {
        auto commentEnd = body.find("*/", commentStart);
        if (commentEnd != std::string::npos) {
            body = body.substr(commentEnd + 2);
        }
    }

    // Strip embedded #version and legacy keywords
    std::regex versionRe(R"(#version\s+\d+[^\n]*)");
    body = std::regex_replace(body, versionRe, "// (version stripped by Easel)");
    body = std::regex_replace(body, std::regex(R"(\bvarying\b)"), "out");
    body = std::regex_replace(body, std::regex(R"(\battribute\b)"), "in");

    std::stringstream out;
    out << "#version 330 core\n";
    out << "layout(location = 0) in vec2 aPos;\n";
    out << "layout(location = 1) in vec2 aTexCoord;\n";
    out << "out vec2 isf_FragNormCoord;\n";
    out << "\n";

    // Provide both naming conventions for the vertex init function
    out << "void isf_vertShaderInit() {\n";
    out << "    gl_Position = vec4(aPos, 0.0, 1.0);\n";
    out << "    isf_FragNormCoord = aTexCoord;\n";
    out << "}\n";
    out << "#define vv_vertShaderInit isf_vertShaderInit\n";
    out << "\n";
    out << body << "\n";

    return out.str();
}

std::string ShaderSource::generateDefaultVertex() {
    return
        "#version 330 core\n"
        "layout(location = 0) in vec2 aPos;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "out vec2 isf_FragNormCoord;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "    isf_FragNormCoord = aTexCoord;\n"
        "}\n";
}

// --- Loading ---

bool ShaderSource::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader: " << path << std::endl;
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    m_rawFragment = ss.str();
    m_path = path;

    // Check for paired .vs file
    std::string vsPath = path;
    if (vsPath.size() > 3 && vsPath.substr(vsPath.size() - 3) == ".fs") {
        vsPath = vsPath.substr(0, vsPath.size() - 3) + ".vs";
        std::ifstream vsFile(vsPath);
        if (vsFile.is_open()) {
            std::stringstream vss;
            vss << vsFile.rdbuf();
            m_rawVertex = vss.str();
        }
    }

    return loadFromCode(m_rawFragment);
}

bool ShaderSource::loadFromCode(const std::string& isfSource) {
    m_rawFragment = isfSource;

    if (!parseISF(isfSource)) {
        std::cerr << "Failed to parse ISF metadata" << std::endl;
        // Still try to compile - might be plain GLSL
    }

    std::string fragSrc = translateFragment(isfSource);
    std::string vertSrc;

    if (!m_rawVertex.empty()) {
        vertSrc = translateVertex(m_rawVertex);
    } else {
        vertSrc = generateDefaultVertex();
    }

    if (!m_shader.loadFromSource(vertSrc, fragSrc)) {
        std::cerr << "Failed to compile ISF shader" << std::endl;
        return false;
    }

    if (!m_initialized) {
        if (!m_fbo.create(m_width, m_height)) {
            std::cerr << "Failed to create shader FBO" << std::endl;
            return false;
        }
        m_quad.createQuad();
        m_initialized = true;
    }

    return true;
}

bool ShaderSource::reload(const std::string& isfSource) {
    // Try to compile new source; keep old shader if it fails
    ShaderProgram newShader;
    std::string oldRawFrag = m_rawFragment;
    auto oldInputs = m_inputs;

    m_rawFragment = isfSource;
    parseISF(isfSource);

    std::string fragSrc = translateFragment(isfSource);
    std::string vertSrc;
    if (!m_rawVertex.empty()) {
        vertSrc = translateVertex(m_rawVertex);
    } else {
        vertSrc = generateDefaultVertex();
    }

    if (!newShader.loadFromSource(vertSrc, fragSrc)) {
        // Restore old state
        m_rawFragment = oldRawFrag;
        m_inputs = oldInputs;
        std::cerr << "Shader reload failed, keeping previous version" << std::endl;
        return false;
    }

    // Transfer matching parameter values from old inputs
    for (auto& newInput : m_inputs) {
        for (const auto& oldInput : oldInputs) {
            if (newInput.name == oldInput.name && newInput.type == oldInput.type) {
                newInput.value = oldInput.value;
                break;
            }
        }
    }

    // Swap in the new shader (old one gets destroyed)
    m_shader.loadFromSource(vertSrc, fragSrc);
    return true;
}

// --- Rendering ---

void ShaderSource::update() {
    if (!m_initialized) return;

    m_fbo.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    m_shader.use();
    uploadUniforms();
    m_quad.draw();

    Framebuffer::unbind();
}

void ShaderSource::uploadUniforms() {
    // ISF built-ins
    m_shader.setFloat("TIME", (float)glfwGetTime());
    m_shader.setVec2("RENDERSIZE", glm::vec2((float)m_width, (float)m_height));
    m_shader.setInt("PASSINDEX", 0);
    m_shader.setInt("FRAMEINDEX", (int)(glfwGetTime() * 60.0)); // approximate frame count

    // User inputs
    for (const auto& input : m_inputs) {
        if (input.type == "float") {
            m_shader.setFloat(input.name, std::get<float>(input.value));
        } else if (input.type == "color") {
            m_shader.setVec4(input.name, std::get<glm::vec4>(input.value));
        } else if (input.type == "bool" || input.type == "event") {
            m_shader.setBool(input.name, std::get<bool>(input.value));
        } else if (input.type == "point2D") {
            m_shader.setVec2(input.name, std::get<glm::vec2>(input.value));
        } else if (input.type == "long") {
            m_shader.setInt(input.name, (int)std::get<float>(input.value));
        } else if (input.type == "text") {
            // Send text as character code uniforms
            // Shader-Claw encoding: A=0, B=1, ..., Z=25, space=26
            std::string text = std::get<std::string>(input.value);
            int maxLen = (int)input.maxVal;
            if (maxLen <= 0) maxLen = 12;
            m_shader.setInt(input.name + "_len", (int)text.size());
            for (int i = 0; i < maxLen; i++) {
                int ch = 26; // space/empty
                if (i < (int)text.size()) {
                    char c = text[i];
                    if (c >= 'A' && c <= 'Z') ch = c - 'A';
                    else if (c >= 'a' && c <= 'z') ch = c - 'a';
                    else ch = 26; // space or unknown
                }
                m_shader.setInt(input.name + "_" + std::to_string(i), ch);
            }
        }
        // image inputs: would bind textures if we had them
    }
}

// --- Parameter setters ---

void ShaderSource::setFloat(const std::string& name, float v) {
    for (auto& input : m_inputs) {
        if (input.name == name && (input.type == "float" || input.type == "long")) {
            input.value = v;
            return;
        }
    }
}

void ShaderSource::setColor(const std::string& name, const glm::vec4& v) {
    for (auto& input : m_inputs) {
        if (input.name == name && input.type == "color") {
            input.value = v;
            return;
        }
    }
}

void ShaderSource::setBool(const std::string& name, bool v) {
    for (auto& input : m_inputs) {
        if (input.name == name && (input.type == "bool" || input.type == "event")) {
            input.value = v;
            return;
        }
    }
}

void ShaderSource::setPoint2D(const std::string& name, const glm::vec2& v) {
    for (auto& input : m_inputs) {
        if (input.name == name && input.type == "point2D") {
            input.value = v;
            return;
        }
    }
}

void ShaderSource::setText(const std::string& name, const std::string& text) {
    for (auto& input : m_inputs) {
        if (input.name == name && input.type == "text") {
            input.value = text;
            return;
        }
    }
}

void ShaderSource::setResolution(int w, int h) {
    if (w == m_width && h == m_height) return;
    m_width = w;
    m_height = h;
    if (m_initialized) {
        m_fbo.resize(w, h);
    }
}
