#include "sources/ShaderSource.h"
#include "app/MIDIManager.h"
#include "render/FontAtlas.h"
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

        // Parse PASSES — multi-pass buffer names
        m_passBuffers.clear();
        if (j.contains("PASSES") && j["PASSES"].is_array()) {
            for (const auto& pass : j["PASSES"]) {
                if (pass.contains("TARGET") && pass["TARGET"].is_string()) {
                    m_passBuffers.push_back(pass["TARGET"].get<std::string>());
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

    // Strip precision qualifiers (GLSL ES, not valid in 330 core)
    body = std::regex_replace(body, std::regex(R"(precision\s+(highp|mediump|lowp)\s+\w+\s*;)"), "// (precision stripped)");

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
    out << "uniform float TIMEDELTA;\n";
    out << "uniform vec2 RENDERSIZE;\n";
    out << "uniform int PASSINDEX;\n";
    out << "uniform int FRAMEINDEX;\n";
    out << "uniform vec2 mousePos;\n";
    out << "uniform vec2 mouseDelta;\n";
    out << "uniform float pinchHold;\n";
    out << "\n";

    // Mouse interaction (for painting shaders)
    out << "uniform float mouseDown;\n";
    out << "\n";

    // ISF image input stubs — provide samplers + IMG_SIZE + flip uniforms
    bool hasImageInputs = false;
    for (const auto& input : m_inputs) {
        if (input.type == "image") {
            hasImageInputs = true;
            out << "uniform sampler2D " << input.name << ";\n";
            out << "uniform vec2 IMG_SIZE_" << input.name << ";\n";
            out << "uniform bool _flip_" << input.name << ";\n";
        }
    }

    // Multi-pass buffer textures
    for (const auto& buf : m_passBuffers) {
        out << "uniform sampler2D " << buf << ";\n";
        hasImageInputs = true;
    }

    // Shader-Claw font atlas (dummy sampler for text shaders)
    if (isfBody.find("fontAtlasTex") != std::string::npos) {
        out << "uniform sampler2D fontAtlasTex;\n";
        hasImageInputs = true;
    }

    // Shader-Claw audio FFT sampler (always declared — bound by setAudioState)
    out << "uniform sampler2D audioFFT;\n";
    hasImageInputs = true;

    // Shader-Claw audio reactivity builtins (always declared)
    out << "uniform float audioLevel;\n";
    out << "uniform float audioBass;\n";
    out << "uniform float audioMid;\n";
    out << "uniform float audioHigh;\n";

    // Shader-Claw voice reactivity builtins
    if (isfBody.find("_voiceGlitch") != std::string::npos) {
        out << "uniform float _voiceGlitch;\n";
    }
    out << "uniform float _voiceLevel;\n";

    // ISF texture sampling macros
    // IMG_SIZE returns the actual size of the named image (via IMG_SIZE_<name> uniform),
    // falling back to RENDERSIZE for pass buffers and unknown textures.
    // IMG_PIXEL uses the image's own size for pixel-coordinate lookups.
    out << "#define IMG_NORM_PIXEL(img, coord) texture(img, coord)\n";
    out << "#define IMG_THIS_NORM_PIXEL(img) texture(img, isf_FragNormCoord)\n";
    out << "#define IMG_THIS_PIXEL(img) texture(img, gl_FragCoord.xy / RENDERSIZE)\n";
    out << "\n";
    // Per-image IMG_SIZE and IMG_PIXEL overloads for each image input
    for (const auto& input : m_inputs) {
        if (input.type == "image") {
            // IMG_SIZE(inputName) -> IMG_SIZE_inputName (actual image dimensions)
            out << "vec2 _isf_img_size_" << input.name << "() { return IMG_SIZE_" << input.name << "; }\n";
            // IMG_PIXEL(inputName, coord) -> sample using image's own dimensions
            out << "vec4 _isf_img_pixel_" << input.name << "(vec2 coord) { return texture(" << input.name << ", coord / IMG_SIZE_" << input.name << "); }\n";
        }
    }
    // Default fallbacks for pass buffers and unknown textures
    out << "#define IMG_SIZE(img) RENDERSIZE\n";
    out << "#define IMG_PIXEL(img, coord) texture(img, (coord) / RENDERSIZE)\n";
    out << "\n";

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

    // Strip declarations that the preamble already provides (custom .vs files redeclare these)
    body = std::regex_replace(body, std::regex(R"(out\s+vec2\s+isf_FragNormCoord\s*;)"), "// (provided by Easel)");
    body = std::regex_replace(body, std::regex(R"(in\s+vec2\s+position\s*;)"), "// (provided by Easel)");

    std::stringstream out;
    out << "#version 330 core\n";
    out << "layout(location = 0) in vec2 aPos;\n";
    out << "layout(location = 1) in vec2 aTexCoord;\n";
    out << "out vec2 isf_FragNormCoord;\n";
    out << "#define position aPos\n";
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

void ShaderSource::createPassFBOs() {
    m_passes.clear();
    m_frameIndex = 0; // reset frame counter so shader seeds correctly

    // Build ISFPass structs from parsed PASSES metadata
    // m_passBuffers has only the named targets; we also need the final pass (no target)
    // Re-parse from raw source to get full PASSES array including final empty pass
    auto start = m_rawFragment.find("/*");
    auto commentEnd = m_rawFragment.find("*/", start);
    auto jsonStart = m_rawFragment.find('{', start);
    if (jsonStart != std::string::npos && jsonStart < commentEnd) {
        int depth = 0;
        size_t jsonEnd = std::string::npos;
        for (size_t i = jsonStart; i < commentEnd; i++) {
            if (m_rawFragment[i] == '{') depth++;
            else if (m_rawFragment[i] == '}') { depth--; if (depth == 0) { jsonEnd = i; break; } }
        }
        if (jsonEnd != std::string::npos) {
            try {
                auto j = json::parse(m_rawFragment.substr(jsonStart, jsonEnd - jsonStart + 1));
                if (j.contains("PASSES") && j["PASSES"].is_array()) {
                    for (const auto& p : j["PASSES"]) {
                        ISFPass pass;
                        if (p.contains("TARGET") && p["TARGET"].is_string())
                            pass.target = p["TARGET"].get<std::string>();
                        if (p.contains("PERSISTENT") && p["PERSISTENT"].is_boolean())
                            pass.persistent = p["PERSISTENT"].get<bool>();

                        if (!pass.target.empty()) {
                            // Auto-downscale simulation passes on large canvases
                            // (fluid sim is low-frequency, half-res looks identical)
                            int pw = m_width, ph = m_height;
                            if (m_width > 4096) {
                                pw = std::max(1, m_width / 2);
                                ph = std::max(1, m_height / 2);
                            }
                            pass.simWidth = pw;
                            pass.simHeight = ph;
                            pass.ppFBO = std::make_unique<PingPongFBO>();
                            pass.ppFBO->a.createHalfFloat(pw, ph);
                            pass.ppFBO->b.createHalfFloat(pw, ph);
                        }
                        m_passes.push_back(std::move(pass));
                    }
                }
            } catch (...) {}
        }
    }
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
        {
            FILE* f = fopen("etherea_debug.log", "a");
            if (f) {
                fprintf(f, "FAILED SHADER: %s\n", m_path.c_str());
                fprintf(f, "GENERATED FRAG:\n%s\n===\n", fragSrc.c_str());
                fclose(f);
            }
        }
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

    // Create ping-pong FBOs for multi-pass shaders
    if (!m_passBuffers.empty()) {
        createPassFBOs();
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

    m_shader.use();

    if (m_passes.empty()) {
        // Single-pass shader (no PASSES in ISF header)
        m_fbo.bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        uploadUniforms(0, m_width, m_height);
        m_quad.draw();
        Framebuffer::unbind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    } else {
        // Multi-pass rendering with ping-pong persistent buffers
        // Reserve texture units for pass buffers (start after audio=2, font=3)
        int targetBaseUnit = 4;

        for (int i = 0; i < (int)m_passes.size(); i++) {
            auto& pass = m_passes[i];
            bool isFinal = pass.target.empty();

            // Determine output FBO
            Framebuffer* outFBO;
            if (isFinal) {
                outFBO = &m_fbo;
            } else {
                outFBO = &pass.ppFBO->writeFBO();
            }

            // Bind output
            glBindFramebuffer(GL_FRAMEBUFFER, outFBO->fboId());
            glViewport(0, 0, outFBO->width(), outFBO->height());

            // Clear: skip for persistent passes, always clear final
            if (!pass.persistent || isFinal) {
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            // Upload uniforms with current pass index and dimensions
            uploadUniforms(i, outFBO->width(), outFBO->height());

            // Bind all pass buffer textures (read side of ping-pong)
            int unit = targetBaseUnit;
            for (int pi = 0; pi < (int)m_passes.size(); pi++) {
                auto& p = m_passes[pi];
                if (p.target.empty() || !p.ppFBO) continue;

                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, p.ppFBO->readFBO().textureId());
                m_shader.setInt(p.target, unit);
                unit++;
            }

            m_quad.draw();

            // Swap ping-pong for persistent passes
            if (pass.persistent && pass.ppFBO) {
                pass.ppFBO->swap();
            }
        }

        Framebuffer::unbind();
        glViewport(0, 0, m_width, m_height);
    }

    // Restore default GL state so other sources (NDI, etc.) aren't affected
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    m_frameIndex++;
}

void ShaderSource::uploadUniforms(int passIndex, int passWidth, int passHeight) {
    if (passWidth <= 0) passWidth = m_width;
    if (passHeight <= 0) passHeight = m_height;

    // ISF built-ins
    float now = (float)glfwGetTime();
    m_shader.setFloat("TIME", now);
    m_shader.setFloat("TIMEDELTA", now - m_lastTime);
    m_lastTime = now;
    m_shader.setVec2("RENDERSIZE", glm::vec2((float)passWidth, (float)passHeight));
    m_shader.setInt("PASSINDEX", passIndex);
    m_shader.setInt("FRAMEINDEX", m_frameIndex);

    // Mouse state
    float dx = m_mouseX - m_prevMouseX;
    float dy = m_mouseY - m_prevMouseY;
    m_shader.setVec2("mousePos", glm::vec2(m_mouseX, m_mouseY));
    m_shader.setVec2("mouseDelta", glm::vec2(dx, dy));
    m_shader.setFloat("mouseDown", m_mouseDown);
    m_shader.setFloat("pinchHold", 0.0f);

    // Audio state (Shader-Claw naming convention)
    m_shader.setFloat("audioLevel", m_audioRMS);
    m_shader.setFloat("audioBass", m_audioBass);
    m_shader.setFloat("audioMid", m_audioMid);
    m_shader.setFloat("audioHigh", m_audioHigh);
    m_shader.setFloat("_voiceLevel", m_audioRMS);
    m_shader.setInt("audioFFT", 2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_audioFFTTex ? m_audioFFTTex : 0);

    // Font atlas for text shaders
    GLuint fontAtlas = FontAtlas::texture();
    m_shader.setInt("fontAtlasTex", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, fontAtlas);

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
            // Shader-Claw 3 encoding: A=0..Z=25, space=26, 0-9=27-36
            std::string text = std::get<std::string>(input.value);
            int maxLen = (int)input.maxVal;
            if (maxLen <= 0) maxLen = 12;
            if ((int)text.size() > maxLen) {
                text = text.substr(text.size() - maxLen);
            }
            m_shader.setInt(input.name + "_len", (int)text.size());
            for (int i = 0; i < maxLen; i++) {
                int ch = 26; // space/empty
                if (i < (int)text.size()) {
                    char c = text[i];
                    if (c >= 'A' && c <= 'Z') ch = c - 'A';
                    else if (c >= 'a' && c <= 'z') ch = c - 'a';
                    else if (c >= '0' && c <= '9') ch = 27 + (c - '0');
                    else ch = 26; // space or unknown
                }
                m_shader.setInt(input.name + "_" + std::to_string(i), ch);
            }
        }
        // image inputs: bind external texture if available
        if (input.type == "image") {
            auto it = m_imageBindings.find(input.name);
            if (it != m_imageBindings.end() && it->second.textureId != 0) {
                // Bind to texture unit 8+ (after audio=2, font=3, pass buffers=4+)
                int imgUnit = 8;
                for (const auto& inp : m_inputs) {
                    if (inp.type == "image" && inp.name == input.name) break;
                    if (inp.type == "image") imgUnit++;
                }
                glActiveTexture(GL_TEXTURE0 + imgUnit);
                glBindTexture(GL_TEXTURE_2D, it->second.textureId);
                m_shader.setInt(input.name, imgUnit);
                m_shader.setVec2("IMG_SIZE_" + input.name,
                    glm::vec2((float)it->second.width, (float)it->second.height));
                m_shader.setBool("_flip_" + input.name, it->second.flippedV);
            } else {
                int imgUnit = 8;
                for (const auto& inp : m_inputs) {
                    if (inp.type == "image" && inp.name == input.name) break;
                    if (inp.type == "image") imgUnit++;
                }
                glActiveTexture(GL_TEXTURE0 + imgUnit);
                glBindTexture(GL_TEXTURE_2D, 0);
                m_shader.setInt(input.name, imgUnit);
                m_shader.setVec2("IMG_SIZE_" + input.name, glm::vec2(0.0f, 0.0f));
                m_shader.setBool("_flip_" + input.name, false);
            }
        }
    }

    // Update previous mouse position (only on pass 0 to avoid multi-update)
    if (passIndex == 0) {
        m_prevMouseX = m_mouseX;
        m_prevMouseY = m_mouseY;
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

void ShaderSource::setMouseState(float x, float y, bool down) {
    m_mouseX = x;
    m_mouseY = y;
    m_mouseDown = down ? 1.0f : 0.0f;
}

void ShaderSource::bindImageInput(const std::string& name, GLuint texId, int w, int h, uint32_t sourceLayerId, bool flippedV) {
    m_imageBindings[name] = {texId, w, h, sourceLayerId, flippedV};
}

void ShaderSource::unbindImageInput(const std::string& name) {
    m_imageBindings.erase(name);
}

void ShaderSource::applyAudioBindings(float level, float bass, float mid, float high, float beat,
                                      MIDIManager* midi) {
    for (auto& [paramName, binding] : m_audioBindings) {
        if (binding.signal == AudioSignal::None) continue;
        if (binding.signal == AudioSignal::MIDI) continue; // handled by applyMidiBindings

        // Get raw signal value (0-1)
        float raw = 0.0f;
        bool haveRaw = true;
        switch (binding.signal) {
            case AudioSignal::Level: raw = level; break;
            case AudioSignal::Bass:  raw = bass; break;
            case AudioSignal::Mid:   raw = mid; break;
            case AudioSignal::High:  raw = high; break;
            case AudioSignal::Beat:  raw = beat; break;
            case AudioSignal::MidiCC: {
                if (midi && binding.midiCC >= 0) {
                    float v = midi->getCCValue(binding.midiChannel, binding.midiCC);
                    if (v < 0.0f) { haveRaw = false; } // no value received yet
                    else raw = v;
                } else {
                    haveRaw = false;
                }
                break;
            }
            default: haveRaw = false; break;
        }
        if (!haveRaw) continue;

        // Asymmetric smoothing: fast attack, slow release
        float attackAlpha = 1.0f - binding.smoothing * 0.8f;
        float releaseAlpha = 1.0f - binding.smoothing * 0.95f;
        float alpha = (raw > binding.smoothedValue) ? attackAlpha : releaseAlpha;
        binding.smoothedValue += (raw - binding.smoothedValue) * alpha;

        // Map to parameter range
        float mapped = binding.rangeMin + binding.smoothedValue * (binding.rangeMax - binding.rangeMin);

        // Find the input and set its value
        for (auto& input : m_inputs) {
            if (input.name == paramName && (input.type == "float" || input.type == "long")) {
                mapped = std::max(input.minVal, std::min(input.maxVal, mapped));
                input.value = mapped;
                break;
            }
        }
    }
}

void ShaderSource::applyMidiBindings(const float ccVals[16][128]) {
    for (auto& [paramName, binding] : m_audioBindings) {
        if (binding.signal != AudioSignal::MIDI) continue;
        if (binding.midiChannel < 0 || binding.midiChannel > 15) continue;
        if (binding.midiCC < 0 || binding.midiCC > 127) continue;

        float raw = ccVals[binding.midiChannel][binding.midiCC]; // 0..1

        // Symmetric smoothing — MIDI knobs/sliders don't need attack/release shaping
        float alpha = 1.0f - binding.smoothing;
        binding.smoothedValue += (raw - binding.smoothedValue) * alpha;

        float mapped = binding.rangeMin + binding.smoothedValue * (binding.rangeMax - binding.rangeMin);

        for (auto& input : m_inputs) {
            if (input.name == paramName && (input.type == "float" || input.type == "long")) {
                mapped = std::max(input.minVal, std::min(input.maxVal, mapped));
                input.value = mapped;
                break;
            }
        }
    }
}

void ShaderSource::setAudioState(float rms, float bass, float mid, float high, GLuint fftTex) {
    m_audioRMS = rms;
    m_audioBass = bass;
    m_audioMid = mid;
    m_audioHigh = high;
    m_audioFFTTex = fftTex;
}

void ShaderSource::setResolution(int w, int h) {
    if (w == m_width && h == m_height) return;
    m_width = w;
    m_height = h;
    if (m_initialized) {
        m_fbo.resize(w, h);
        // Resize pass FBOs (simulation passes at half-res for large canvases)
        for (auto& pass : m_passes) {
            if (pass.ppFBO) {
                int pw = w, ph = h;
                if (w > 4096) {
                    pw = std::max(1, w / 2);
                    ph = std::max(1, h / 2);
                }
                pass.simWidth = pw;
                pass.simHeight = ph;
                pass.ppFBO->a.resize(pw, ph);
                pass.ppFBO->b.resize(pw, ph);
            }
        }
        m_frameIndex = 0; // reset so shader re-seeds
    }
}
