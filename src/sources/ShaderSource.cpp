#include "sources/ShaderSource.h"
#include "app/MIDIManager.h"
#include "render/FontAtlas.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <cmath>
#include <algorithm>
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
                param.name  = input.value("NAME", "");
                param.type  = input.value("TYPE", "float");
                param.label = input.value("LABEL", "");
                param.group = input.value("GROUP", "");

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
                        for (const auto& v : input["VALUES"]) {
                            if (v.is_number()) param.longValues.push_back(v.get<int>());
                        }
                        if (!param.longValues.empty())
                            param.maxVal = (float)(param.longValues.size() - 1);
                    }
                    param.defaultFloat = input.contains("DEFAULT") ? jsonToFloat(input["DEFAULT"], 0.0f) : 0.0f;
                    param.value = param.defaultFloat;
                    if (input.contains("LABELS") && input["LABELS"].is_array()) {
                        for (const auto& l : input["LABELS"]) {
                            param.longLabels.push_back(l.get<std::string>());
                        }
                    }
                } else if (param.type == "event") {
                    // Event trigger — behaves like a momentary bool
                    param.defaultBool = false;
                    param.value = false;
                    // Optional ISF fields used by PropertyPanel's hit-button:
                    //   MOMENTARY: press-and-hold mode (uniform mirrors held).
                    //   TARGET:    on tap, randomize this sibling float param.
                    param.momentary   = input.value("MOMENTARY", false);
                    param.eventTarget = input.value("TARGET",    std::string(""));
                } else if (param.type == "text") {
                    // Text input — Shader-Claw compiles to NAME_0..NAME_N + NAME_len uniforms
                    // Store default text and max length
                    if (input.contains("MAX_LENGTH") && input["MAX_LENGTH"].is_number()) {
                        param.maxVal = input["MAX_LENGTH"].get<float>();
                    } else {
                        param.maxVal = 12.0f;
                    }
                    param.defaultText = input.value("DEFAULT", "");
                    // SCTEXT/1: case is the shader's decision (CASE key, default
                    // preserve) — the DEFAULT is no longer uppercased here.
                    param.value = param.defaultText;
                    param.textRun = TextRun{param.defaultText};
                    if (input.contains("CASE") && input["CASE"].is_string()) {
                        std::string cs = input["CASE"].get<std::string>();
                        for (auto& c : cs) c = (char)tolower((unsigned char)c);
                        if (cs == "upper" || cs == "lower" || cs == "preserve") param.textCase = cs;
                    }
                    if (input.contains("OVERFLOW") && input["OVERFLOW"].is_string()) {
                        std::string ov = input["OVERFLOW"].get<std::string>();
                        for (auto& c : ov) c = (char)tolower((unsigned char)c);
                        param.textOverflowHead = (ov == "head");
                    }
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

    // SCTEXT/1: decide per text input whether the GLSL BODY (not the JSON)
    // still uses the legacy per-char uniforms (`<n>_<digits>` => shim) and
    // whether it samples the glyph buffer (`textBuf_<n>`); cache the global
    // glyph-atlas / helper-block references for translateFragment +
    // uploadUniforms.
    {
        std::string body = source.substr(commentEnd + 2);
        m_bodyHasGlyphAtlas = body.find("glyphAtlasTex") != std::string::npos;
        m_bodyHasScText = body.find("scText") != std::string::npos ||
                          body.find("scGlyph") != std::string::npos;
        // SCFONT/1 MSDF face references (Phase D)
        m_bodyHasMSDFTex    = body.find("glyphMSDFTex")    != std::string::npos;
        m_bodyHasMetricsTex = body.find("glyphMetricsTex") != std::string::npos;
        m_bodyHasAtlasInfo  = body.find("glyphAtlasInfo")  != std::string::npos;
        m_bodyHasFaceInfo   = body.find("glyphFaceInfo")   != std::string::npos;
        for (auto& in : m_inputs) {
            if (in.type != "text") continue;
            in.textShim = std::regex_search(body, std::regex("\\b" + in.name + "_[0-9]+\\b"));
            in.textBufRef = body.find("textBuf_" + in.name) != std::string::npos;
        }
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

    // Route every sampling of a top-down image input (NDI, Video, etc.) through
    // a per-input wrapper that flips v when _flip_<name> is true. Without this
    // the _flip_<name> uniform is set but never consumed and the source ends up
    // upside down inside the shader.
    for (const auto& input : m_inputs) {
        if (input.type != "image") continue;
        const std::string& n = input.name;
        body = std::regex_replace(body,
            std::regex("\\btexture\\s*\\(\\s*" + n + "\\s*,"),
            "_flippedTex_" + n + "(");
        body = std::regex_replace(body,
            std::regex("\\bIMG_NORM_PIXEL\\s*\\(\\s*" + n + "\\s*,"),
            "_flippedTex_" + n + "(");
        body = std::regex_replace(body,
            std::regex("\\bIMG_PIXEL\\s*\\(\\s*" + n + "\\s*,"),
            "_flippedTexPixel_" + n + "(");
        body = std::regex_replace(body,
            std::regex("\\bIMG_THIS_PIXEL\\s*\\(\\s*" + n + "\\s*\\)"),
            "_flippedTexThis_" + n + "()");
        body = std::regex_replace(body,
            std::regex("\\bIMG_THIS_NORM_PIXEL\\s*\\(\\s*" + n + "\\s*\\)"),
            "_flippedTexThisNorm_" + n + "()");
    }

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
    out << "uniform float msgAge;\n";
    out << "\n";

    // Mouse interaction (for painting shaders)
    out << "uniform float mouseDown;\n";
    out << "\n";

    // Phase V scaffolding — MediaPipe-compatible landmark samplers so
    // shaders authored against ShaderClaw3 web (which feeds these via
    // browser MediaPipe) compile in Easel. VisionSource will populate
    // these in V1.1; until then they sample whatever texture is bound
    // to unit 0 (typically zero — shaders check the alpha channel as
    // confidence before drawing pose-driven elements).
    out << "uniform sampler2D mpPoseLandmarks;\n";
    out << "uniform sampler2D mpFaceLandmarks;\n";
    out << "uniform sampler2D mpHandLandmarks;\n";
    out << "uniform sampler2D mpSegmentation;\n";
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
    // SCTEXT/1 glyph atlas (16x6 printable-ASCII grid, unit 12)
    if (body.find("glyphAtlasTex") != std::string::npos) {
        out << "uniform sampler2D glyphAtlasTex;\n";
        hasImageInputs = true;
    }
    // SCFONT/1 MSDF glyph atlas + metrics (Phase D, phase-D-design.md 1.4):
    // units 10 (atlas, LINEAR) / 11 (metrics, NEAREST), each declared only
    // when the body references the name.
    if (body.find("glyphMSDFTex") != std::string::npos) {
        out << "uniform sampler2D glyphMSDFTex;\n";
        hasImageInputs = true;
    }
    if (body.find("glyphMetricsTex") != std::string::npos) {
        out << "uniform sampler2D glyphMetricsTex;\n";
        hasImageInputs = true;
    }
    if (body.find("glyphAtlasInfo") != std::string::npos) {
        out << "uniform vec4 glyphAtlasInfo;\n";
    }
    if (body.find("glyphFaceInfo") != std::string::npos) {
        out << "uniform vec4 glyphFaceInfo;\n";
    }

    // Shader-Claw audio FFT sampler (always declared — bound by setAudioState)
    out << "uniform sampler2D audioFFT;\n";
    hasImageInputs = true;

    // Shader-Claw audio reactivity builtins (always declared)
    out << "uniform float audioLevel;\n";
    out << "uniform float audioBass;\n";
    out << "uniform float audioMid;\n";
    out << "uniform float audioHigh;\n";

    // --- Audio Feature Bus (always declared; see AudioFeatures.h) ----------
    // Tier 1 — core energy & bands
    out << "uniform float audioSub, audioLowMid, audioHighMid, audioTreble, audioPunch;\n";
    out << "uniform float audioBeat, audioBeatPhase, audioBeatPulse, audioBarPhase, audioBPM, audioTempo01;\n";
    // Tier 2 — spectral character
    out << "uniform float audioBrightness, audioSpread, audioRolloff, audioFlatness, audioTexture;\n";
    out << "uniform float audioFlux, audioOnset, audioOnsetRate, audioTilt, audioZCR;\n";
    // Tier 3 — affect / mood
    out << "uniform float audioValence, audioArousal, audioTension, audioWarmth, audioSoftness, audioRoughness, audioCharm;\n";
    out << "uniform vec2  audioMood;\n";
    // Tier 4 — structure / build-up
    out << "uniform float audioEnergy, audioEnergyVel, audioEnergyAcc, audioBuildup, audioBuildupRate, audioDrop;\n";
    out << "uniform float audioNovelty, audioSectionPhase, audioSectionAge, audioLayers, audioDensity;\n";
    out << "uniform vec4  audioPresence;\n";
    out << "uniform vec2  audioFlow;\n";
    // Tier 5 — palette anchors (linear RGB)
    out << "uniform vec3  audioPalShadow, audioPalMid, audioPalHigh, audioPalAccent;\n";
    out << "uniform float audioPalTemp, audioPalSat;\n";
    // Harmony scalars (chroma[] + chroma/occupancy textures land with the palette engine)
    out << "uniform float audioDominantPitch, audioMajorMinor, audioHCDF;\n";
    // ── EaselAudio v1 (spec/easel_audio_bus.json) ─────────────────────
    // Temperament matrix — Hit / Presence / Time per band + mix.
    // (audioLevelPresence: the schema's mix "audioPresence" name is already
    // taken by the vec4 per-band presence uniform above — nothing renamed.)
    out << "uniform float audioBassHit, audioMidHit, audioHighHit;\n";
    out << "uniform float audioBassPresence, audioMidPresence, audioHighPresence, audioLevelPresence;\n";
    out << "uniform float audioBassTime, audioMidTime, audioHighTime, audioTime;\n";
    // Rhythm bus — detected tempo confidence + multi-beat phase ramps + eased one-shots
    out << "uniform float audioBPMConfidence, audioPhase2, audioPhase4, audioPhase8, audioPhase16;\n";
    out << "uniform float audioOnBeat, audioToggleOnBeat;\n";
    // Tier-1 pseudo-stems (band split + causal median HPSS) + temperaments
    out << "uniform float stemBass, stemDrums, stemMelody, stemAir, stemVocal;\n";
    out << "uniform float stemBassHit, stemDrumsHit, stemMelodyHit, stemAirHit, stemVocalHit;\n";
    out << "uniform float stemBassPresence, stemDrumsPresence, stemMelodyPresence, stemAirPresence, stemVocalPresence;\n";

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
            // Flip-aware wrappers — the regex pass on the user body redirects
            // texture/IMG_*_PIXEL calls on this input to these helpers so
            // top-down NDI/Video sources read right-side-up inside the shader.
            const std::string& n = input.name;
            out << "vec4 _flippedTex_" << n << "(vec2 uv) { if (_flip_" << n
                << ") uv.y = 1.0 - uv.y; return texture(" << n << ", uv); }\n";
            out << "vec4 _flippedTexPixel_" << n << "(vec2 px) { return _flippedTex_"
                << n << "(px / IMG_SIZE_" << n << "); }\n";
            out << "vec4 _flippedTexThis_" << n << "() { return _flippedTex_"
                << n << "(gl_FragCoord.xy / RENDERSIZE); }\n";
            out << "vec4 _flippedTexThisNorm_" << n << "() { return _flippedTex_"
                << n << "(isf_FragNormCoord); }\n";
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
            // SCTEXT/1 (phase-C-design.md 1.2/1.4): `<n>_len` always; the
            // legacy per-char ints `<n>_0..` only while the body still uses
            // them (shim, cap min(MAX_LENGTH,64)); the glyph buffer sampler +
            // `<n>_cap` only when the body samples `textBuf_<n>`.
            int maxLen = (int)input.maxVal;
            if (maxLen <= 0) maxLen = 12;
            out << "uniform int " << input.name << "_len;\n";
            if (input.textShim) {
                int shimCap = std::min(maxLen, 64);
                for (int i = 0; i < shimCap; i++) {
                    out << "uniform int " << input.name << "_" << i << ";\n";
                }
            }
            if (input.textBufRef) {
                out << "uniform sampler2D textBuf_" << input.name << ";\n";
                out << "uniform float " << input.name << "_cap;\n";
            }
        }
        // image type already declared above
    }
    out << "\n";

    // Redirect gl_FragColor -> FragColor
    out << "#define gl_FragColor FragColor\n";
    out << "\n";

    // --- Audio Feature Bus helpers (auto-injected; see audio-reactive-system.md).
    // Shared idioms so shaders adopt the bus consistently. Names are audio-
    // prefixed to avoid colliding with the audioFFT sampler / user symbols.
    out << "vec3 audioPalette(float t){\n"
           "  t = clamp(t,0.0,1.0);\n"
           "  vec3 c = (t<0.5)? mix(audioPalShadow,audioPalMid,t*2.0)\n"
           "                  : mix(audioPalMid,audioPalHigh,t*2.0-1.0);\n"
           "  return mix(c, audioPalAccent, audioBeat*smoothstep(0.6,1.0,t)*0.6);\n"
           "}\n";
    out << "float audioSpectrum(float f){ return texture(audioFFT, vec2(clamp(f,0.0,1.0),0.5)).r; }\n";
    out << "float audioKick(){ return audioBeatPulse; }\n";      // bass transient
    out << "float audioHit(){ return audioOnset; }\n";           // broadband transient
    out << "float audioBreath(){ return pow(max(audioLevel,0.0),0.6); }\n"; // perceptual loudness
    out << "float audioAlive(float rest,float drive,float amount){ return rest+drive*amount; }\n"; // living baseline
    out << "\n";

    // SCTEXT/1 + SCFONT/1 helper block "sc_text v2" (phase-C-design.md 1.6 +
    // phase-D-design.md 1.6) — verbatim across easel / claw3 / etherea except
    // `texture(` here because the preamble is emitted after the
    // texture2D->texture regex on the body. v1 function text is byte-identical.
    if (body.find("scText") != std::string::npos || body.find("scGlyph") != std::string::npos ||
        body.find("scMSDF") != std::string::npos || body.find("sampleGlyphMSDF") != std::string::npos) {
        out << "// ---- sc_text v2 : SCTEXT/1 glyph buffer + SCFONT/1 MSDF glyph atlas + metrics helpers (keep identical across easel / claw3 / etherea) ----\n"
               "vec4 scTextTexel(sampler2D buf, float cap, int i, float sub) {\n"
               "    return texture(buf, vec2((float(i) * 2.0 + sub + 0.5) / (cap * 2.0), 0.5));\n"
               "}\n"
               "int scTextByte(float v) { return int(floor(v * 255.0 + 0.5)); }\n"
               "int scTextCode(sampler2D buf, float cap, int i) {         // Unicode code point; 0 = empty slot\n"
               "    vec4 t = scTextTexel(buf, cap, i, 0.0);\n"
               "    return scTextByte(t.r) + 256 * scTextByte(t.g);\n"
               "}\n"
               "int scTextWord(sampler2D buf, float cap, int i) {         // word index; -1 = none (whitespace)\n"
               "    int w = scTextByte(scTextTexel(buf, cap, i, 0.0).b);\n"
               "    return (w == 255) ? -1 : w;\n"
               "}\n"
               "float scTextRevealMs(sampler2D buf, float cap, int i) {   // ms since utterance start; -1.0 = unknown\n"
               "    vec4 t = scTextTexel(buf, cap, i, 1.0);\n"
               "    int ms = scTextByte(t.r) + 256 * scTextByte(t.g);\n"
               "    return (ms == 65535) ? -1.0 : float(ms);\n"
               "}\n"
               "int scTextSpeaker(sampler2D buf, float cap, int i) {      // speaker id; -1 = unknown\n"
               "    int s = scTextByte(scTextTexel(buf, cap, i, 1.0).b);\n"
               "    return (s == 255) ? -1 : s;\n"
               "}\n"
               "int scGlyphCell(int cp) { return (cp >= 32 && cp <= 126) ? cp - 32 : 95; }\n"
               "vec2 scGlyphUV(int cp, vec2 uv) {                         // uv glyph-local, y = 1 at glyph top\n"
               "    float idx = float(scGlyphCell(cp));\n"
               "    float col = mod(idx, 16.0);\n"
               "    float row = floor(idx / 16.0);\n"
               "    return vec2((col + uv.x) / 16.0, 1.0 - (row + 1.0 - uv.y) / 6.0);\n"
               "}\n"
               "float scGlyphSample(sampler2D atlas, int cp, vec2 uv) {   // coverage 0..1; 0 outside the cell / for space\n"
               "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;\n"
               "    if (cp <= 32) return 0.0;\n"
               "    return texture(atlas, scGlyphUV(cp, uv)).r;\n"
               "}\n"
               "float scU16(vec2 v) { return floor(v.x * 255.0 + 0.5) + 256.0 * floor(v.y * 255.0 + 0.5); }\n"
               "float scS16em(vec2 v) { return (scU16(v) - 32768.0) / 4096.0; }          // em, fixed-point 1/4096\n"
               "vec4 scGlyphMetricTexel(sampler2D mtx, int cp, int k) {                  // 8 texels per code point, page = cp/256\n"
               "    int page = cp / 256; int lo = cp - page * 256;\n"
               "    return texture(mtx, vec2((float(lo * 8 + k) + 0.5) / 2048.0, (float(page) + 0.5) / 256.0));\n"
               "}\n"
               "bool  scGlyphPresent(sampler2D mtx, int cp) { return scGlyphMetricTexel(mtx, cp, 4).b > 0.5; }\n"
               "int   scGlyphResolve(sampler2D mtx, int cp) { return scGlyphPresent(mtx, cp) ? cp : 65533; }   // U+FFFD fallback\n"
               "vec4  scGlyphAtlasRect(sampler2D mtx, int cp) {                           // atlas px (left, bottom, right, top), y up\n"
               "    vec4 a = scGlyphMetricTexel(mtx, cp, 0), b = scGlyphMetricTexel(mtx, cp, 1);\n"
               "    return vec4(scU16(a.rg), scU16(a.ba), scU16(b.rg), scU16(b.ba));\n"
               "}\n"
               "vec4  scGlyphPlane(sampler2D mtx, int cp) {                               // em box rel. to pen origin (left, bottom, right, top)\n"
               "    vec4 a = scGlyphMetricTexel(mtx, cp, 2), b = scGlyphMetricTexel(mtx, cp, 3);\n"
               "    return vec4(scS16em(a.rg), scS16em(a.ba), scS16em(b.rg), scS16em(b.ba));\n"
               "}\n"
               "float scGlyphAdvance(sampler2D mtx, int cp) { return scS16em(scGlyphMetricTexel(mtx, cp, 4).rg); }   // em\n"
               "float scMSDFMedian(vec3 c) { return max(min(c.r, c.g), min(max(c.r, c.g), c.b)); }\n"
               "// Analytic (no derivatives): screenPxRange = distanceRangePx * (glyph px on screen / glyph px in the atlas).\n"
               "float scMSDFCoverage(sampler2D atlas, vec2 atlasUV, float screenPxRange) {\n"
               "    float sd = scMSDFMedian(texture(atlas, atlasUV).rgb) - 0.5;\n"
               "    return clamp(sd * max(screenPxRange, 1.0) + 0.5, 0.0, 1.0);\n"
               "}\n"
               "float scMSDFScreenPxRange(vec4 atlasInfo, float glyphScreenPx, float glyphAtlasPx) {\n"
               "    return atlasInfo.z * glyphScreenPx / max(glyphAtlasPx, 1.0);\n"
               "}\n"
               "// Glyph-local uv over the plane box (x 0..1 left->right, y 0..1 bottom->top) -> coverage; 0 outside / absent / space.\n"
               "float scGlyphMSDF(sampler2D atlas, sampler2D mtx, vec4 atlasInfo, int cp, vec2 uv, float screenPxRange) {\n"
               "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;\n"
               "    vec4 r = scGlyphAtlasRect(mtx, cp);\n"
               "    if (r.z <= r.x || r.w <= r.y) return 0.0;\n"
               "    vec2 auv = (r.xy + uv * (r.zw - r.xy)) / atlasInfo.xy;\n"
               "    return scMSDFCoverage(atlas, auv, screenPxRange);\n"
               "}\n"
               "// ---- end sc_text v2 ----\n\n";
    }

    // sc_text v2d: derivative-based MSDF sampler, only when referenced
    // (GLSL 330 core has fwidth without any extension).
    if (body.find("sampleGlyphMSDF") != std::string::npos) {
        out << "// ---- sc_text v2d : derivative-based variant ----\n"
               "float sampleGlyphMSDF(sampler2D atlas, vec2 atlasUV, float distanceRange, vec2 atlasSizePx) {\n"
               "    vec2 unitRange = vec2(distanceRange) / atlasSizePx;\n"
               "    vec2 screenTexSize = vec2(1.0) / fwidth(atlasUV);\n"
               "    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);\n"
               "    float sd = scMSDFMedian(texture(atlas, atlasUV).rgb) - 0.5;\n"
               "    return clamp(sd * screenPxRange + 0.5, 0.0, 1.0);\n"
               "}\n"
               "// ---- end sc_text v2d ----\n\n";
    }

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
        m_lastError = m_shader.lastError();
        std::cerr << "Failed to compile ISF shader" << std::endl;
        {
            FILE* f = fopen("/tmp/etherea_debug.log", "a");
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

    pruneTextBufTextures(false);
    return true;
}

bool ShaderSource::reload(const std::string& isfSource) {
    // Try to compile new source; keep old shader if it fails
    ShaderProgram newShader;
    std::string oldRawFrag = m_rawFragment;
    auto oldInputs = m_inputs;
    auto oldPassBuffers = m_passBuffers;
    bool oldHasGlyphAtlas = m_bodyHasGlyphAtlas, oldHasScText = m_bodyHasScText;
    bool oldHasMSDFTex = m_bodyHasMSDFTex, oldHasMetricsTex = m_bodyHasMetricsTex;
    bool oldHasAtlasInfo = m_bodyHasAtlasInfo, oldHasFaceInfo = m_bodyHasFaceInfo;

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
        // Restore old state. m_passBuffers must roll back too — the next
        // successful reload compares against it to decide whether the pass
        // FBOs need rebuilding, and the failed candidate's layout would
        // make that comparison lie.
        m_rawFragment = oldRawFrag;
        m_inputs = oldInputs;
        m_passBuffers = oldPassBuffers;
        m_bodyHasGlyphAtlas = oldHasGlyphAtlas;
        m_bodyHasScText = oldHasScText;
        m_bodyHasMSDFTex = oldHasMSDFTex;
        m_bodyHasMetricsTex = oldHasMetricsTex;
        m_bodyHasAtlasInfo = oldHasAtlasInfo;
        m_bodyHasFaceInfo = oldHasFaceInfo;
        std::cerr << "Shader reload failed, keeping previous version" << std::endl;
        return false;
    }

    // Transfer matching parameter values from old inputs
    for (auto& newInput : m_inputs) {
        for (const auto& oldInput : oldInputs) {
            if (newInput.name == oldInput.name && newInput.type == oldInput.type) {
                newInput.value = oldInput.value;
                if (newInput.type == "text") {
                    newInput.textRun = oldInput.textRun;   // keep word timings; textDirty stays true
                }
                break;
            }
        }
    }

    // Swap in the new shader (old one gets destroyed)
    m_shader.loadFromSource(vertSrc, fragSrc);

    // Rebuild the multi-pass ping-pong FBOs when the PASSES layout changed.
    // Without this, a hot-reload that dropped passes left their half-float
    // buffers resident AND still dispatched every frame, while added passes
    // had no storage at all. When the layout is unchanged (the common case —
    // ShaderImprover mandates pass-union preservation) the buffers are kept
    // so persistent-pass state survives the reload.
    if (m_passBuffers != oldPassBuffers) {
        createPassFBOs();
    }
    pruneTextBufTextures(false);
    return true;
}

ShaderSource::~ShaderSource() {
    pruneTextBufTextures(true);
}

// Delete glyph-buffer textures that no current text input samples (or all
// of them when `all` — destructor path).
void ShaderSource::pruneTextBufTextures(bool all) {
    for (auto it = m_textBufTex.begin(); it != m_textBufTex.end();) {
        bool keep = false;
        if (!all) {
            for (const auto& in : m_inputs) {
                if (in.type == "text" && in.textBufRef && in.name == it->first) { keep = true; break; }
            }
        }
        if (keep) { ++it; continue; }
        if (it->second) glDeleteTextures(1, &it->second);
        it = m_textBufTex.erase(it);
    }
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

            // Swap ping-pong after ANY targeted pass so subsequent passes (and
            // the final pass) read what this pass just wrote. Previously only
            // persistent passes swapped, so a plain intermediate TARGET buffer
            // stayed on the stale read side → later passes sampled an empty
            // buffer (the black-output bug that blocked multi-pass shaders).
            if (pass.ppFBO) {
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
    m_shader.setFloat("msgAge", m_msgAge);

    // Audio state (Shader-Claw naming convention)
    m_shader.setFloat("audioLevel", m_audioRMS);
    m_shader.setFloat("audioBass", m_audioBass);
    m_shader.setFloat("audioMid", m_audioMid);
    m_shader.setFloat("audioHigh", m_audioHigh);
    m_shader.setFloat("_voiceLevel", m_audioRMS);
    m_shader.setInt("audioFFT", 2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_audioFFTTex ? m_audioFFTTex : 0);

    // --- Audio Feature Bus uploads (legacy audioLevel/Bass/Mid/High above are
    //     the frozen pipeline; these are the new perceptual features) ---------
    {
        const AudioFeatures& f = m_af;
        // Tier 1
        m_shader.setFloat("audioSub", f.sub);
        m_shader.setFloat("audioLowMid", f.lowMid);
        m_shader.setFloat("audioHighMid", f.highMid);
        m_shader.setFloat("audioTreble", f.treble);
        m_shader.setFloat("audioPunch", f.punch);
        m_shader.setFloat("audioBeat", f.beat);
        m_shader.setFloat("audioBeatPhase", f.beatPhase);
        m_shader.setFloat("audioBeatPulse", f.beatPulse);
        m_shader.setFloat("audioBarPhase", f.barPhase);
        m_shader.setFloat("audioBPM", f.bpm);
        m_shader.setFloat("audioTempo01", f.tempo01);
        // Tier 2
        m_shader.setFloat("audioBrightness", f.brightness);
        m_shader.setFloat("audioSpread", f.spread);
        m_shader.setFloat("audioRolloff", f.rolloff);
        m_shader.setFloat("audioFlatness", f.flatness);
        m_shader.setFloat("audioTexture", f.texture);
        m_shader.setFloat("audioFlux", f.flux);
        m_shader.setFloat("audioOnset", f.onset);
        m_shader.setFloat("audioOnsetRate", f.onsetRate);
        m_shader.setFloat("audioTilt", f.tilt);
        m_shader.setFloat("audioZCR", f.zcr);
        // Tier 3
        m_shader.setFloat("audioValence", f.valence);
        m_shader.setFloat("audioArousal", f.arousal);
        m_shader.setFloat("audioTension", f.tension);
        m_shader.setFloat("audioWarmth", f.warmth);
        m_shader.setFloat("audioSoftness", f.softness);
        m_shader.setFloat("audioRoughness", f.roughness);
        m_shader.setFloat("audioCharm", f.charm);
        m_shader.setVec2("audioMood", glm::vec2(f.valence, f.arousal));
        // Tier 4
        m_shader.setFloat("audioEnergy", f.energy);
        m_shader.setFloat("audioEnergyVel", f.energyVel);
        m_shader.setFloat("audioEnergyAcc", f.energyAcc);
        m_shader.setFloat("audioBuildup", f.buildup);
        m_shader.setFloat("audioBuildupRate", f.buildupRate);
        m_shader.setFloat("audioDrop", f.drop);
        m_shader.setFloat("audioNovelty", f.novelty);
        m_shader.setFloat("audioSectionPhase", f.sectionPhase);
        m_shader.setFloat("audioSectionAge", f.sectionAge);
        m_shader.setFloat("audioLayers", f.layers);
        m_shader.setFloat("audioDensity", f.density);
        m_shader.setVec4("audioPresence", glm::vec4(f.presence[0], f.presence[1], f.presence[2], f.presence[3]));
        m_shader.setVec2("audioFlow", glm::vec2(f.flow[0], f.flow[1]));
        // Tier 5
        m_shader.setVec3("audioPalShadow", glm::vec3(f.palShadow[0], f.palShadow[1], f.palShadow[2]));
        m_shader.setVec3("audioPalMid",    glm::vec3(f.palMid[0], f.palMid[1], f.palMid[2]));
        m_shader.setVec3("audioPalHigh",   glm::vec3(f.palHigh[0], f.palHigh[1], f.palHigh[2]));
        m_shader.setVec3("audioPalAccent", glm::vec3(f.palAccent[0], f.palAccent[1], f.palAccent[2]));
        m_shader.setFloat("audioPalTemp", f.palTemp);
        m_shader.setFloat("audioPalSat", f.palSat);
        // Harmony scalars
        m_shader.setFloat("audioDominantPitch", f.dominantPitch);
        m_shader.setFloat("audioMajorMinor", f.majorMinor);
        m_shader.setFloat("audioHCDF", f.hcdf);
        // ── EaselAudio v1 — temperament matrix ─────────────────────────
        m_shader.setFloat("audioBassHit", f.bassHit);
        m_shader.setFloat("audioMidHit", f.midHit);
        m_shader.setFloat("audioHighHit", f.highHit);
        m_shader.setFloat("audioBassPresence", f.bassPresence);
        m_shader.setFloat("audioMidPresence", f.midPresence);
        m_shader.setFloat("audioHighPresence", f.highPresence);
        m_shader.setFloat("audioLevelPresence", f.levelPresence);
        m_shader.setFloat("audioBassTime", f.bassTime);
        m_shader.setFloat("audioMidTime", f.midTime);
        m_shader.setFloat("audioHighTime", f.highTime);
        m_shader.setFloat("audioTime", f.levelTime);
        // Rhythm bus
        m_shader.setFloat("audioBPMConfidence", f.bpmConfidence);
        m_shader.setFloat("audioPhase2", f.phase2);
        m_shader.setFloat("audioPhase4", f.phase4);
        m_shader.setFloat("audioPhase8", f.phase8);
        m_shader.setFloat("audioPhase16", f.phase16);
        m_shader.setFloat("audioOnBeat", f.onBeat);
        m_shader.setFloat("audioToggleOnBeat", f.toggleOnBeat);
        // Pseudo-stems + temperaments
        m_shader.setFloat("stemBass", f.stemBass);
        m_shader.setFloat("stemDrums", f.stemDrums);
        m_shader.setFloat("stemMelody", f.stemMelody);
        m_shader.setFloat("stemAir", f.stemAir);
        m_shader.setFloat("stemVocal", f.stemVocal);
        m_shader.setFloat("stemBassHit", f.stemBassHit);
        m_shader.setFloat("stemDrumsHit", f.stemDrumsHit);
        m_shader.setFloat("stemMelodyHit", f.stemMelodyHit);
        m_shader.setFloat("stemAirHit", f.stemAirHit);
        m_shader.setFloat("stemVocalHit", f.stemVocalHit);
        m_shader.setFloat("stemBassPresence", f.stemBassPresence);
        m_shader.setFloat("stemDrumsPresence", f.stemDrumsPresence);
        m_shader.setFloat("stemMelodyPresence", f.stemMelodyPresence);
        m_shader.setFloat("stemAirPresence", f.stemAirPresence);
        m_shader.setFloat("stemVocalPresence", f.stemVocalPresence);
    }

    // Font atlas for text shaders
    GLuint fontAtlas = FontAtlas::texture();
    m_shader.setInt("fontAtlasTex", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, fontAtlas);

    // SCTEXT/1 glyph atlas (unit 12) when the body references it
    if (m_bodyHasGlyphAtlas) {
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, FontAtlas::glyphTexture());
        m_shader.setInt("glyphAtlasTex", 12);
    }

    // SCFONT/1 MSDF face (Phase D): atlas on unit 10, metrics on unit 11,
    // resolved from the shader's fontFamily/fontWeight inputs (default 0/400).
    if (m_bodyHasMSDFTex || m_bodyHasMetricsTex || m_bodyHasAtlasInfo || m_bodyHasFaceInfo) {
        int msdfFam = 0, msdfWeight = 400;
        int imageInputCount = 0;
        for (const auto& in : m_inputs) {
            if (in.type == "image") imageInputCount++;
            if ((in.type == "long" || in.type == "float") &&
                std::holds_alternative<float>(in.value)) {
                if (in.name == "fontFamily")
                    msdfFam = (int)std::lround(std::get<float>(in.value));
                else if (in.name == "fontWeight")
                    msdfWeight = (int)std::lround(std::get<float>(in.value));
            }
        }
        const MSDFFace& msdf = FontAtlas::msdfFace(msdfFam, msdfWeight);
        if (m_bodyHasMSDFTex) {
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D, msdf.atlasTex);
            m_shader.setInt("glyphMSDFTex", 10);
        }
        if (m_bodyHasMetricsTex) {
            glActiveTexture(GL_TEXTURE11);
            glBindTexture(GL_TEXTURE_2D, msdf.metricsTex);
            m_shader.setInt("glyphMetricsTex", 11);
        }
        if (m_bodyHasAtlasInfo)
            m_shader.setVec4("glyphAtlasInfo", glm::vec4(msdf.atlasInfo[0], msdf.atlasInfo[1],
                                                         msdf.atlasInfo[2], msdf.atlasInfo[3]));
        if (m_bodyHasFaceInfo)
            m_shader.setVec4("glyphFaceInfo", glm::vec4(msdf.faceInfo[0], msdf.faceInfo[1],
                                                        msdf.faceInfo[2], msdf.faceInfo[3]));
        // Image inputs bind at units 8+; a 3rd/4th image collides with 10/11
        // (design-accepted like Phase C's 12-15 note) — log once.
        if (!m_loggedMSDFCollision && imageInputCount > 2) {
            std::cerr << "ShaderSource: SCFONT/1 uses texture units 10/11; "
                      << imageInputCount << " image inputs (units 8+) may collide"
                      << std::endl;
            m_loggedMSDFCollision = true;
        }
    }

    // User inputs
    int textBufIndex = 0; // k-th text input sampling textBuf_<n> -> unit 13+k (max 3)
    for (auto& input : m_inputs) {
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
            // SCTEXT/1: encode lazily (text/options changed), upload the
            // glyph-buffer texture when the body samples it, and keep the
            // legacy per-char int uniforms alive for shim shaders.
            int maxLen = (int)input.maxVal;
            if (maxLen <= 0) maxLen = 12;
            const int cap = std::max(1, std::min(maxLen, 1024));
            const int shimCap = std::min(maxLen, 64);

            // Direct writes to input.value (PropertyPanel, OSC) bypass
            // setTextRun — resync the run from the value string.
            const std::string& cur = std::get<std::string>(input.value);
            if (cur != input.textRun.text) {
                input.textRun = TextRun{cur};
                input.textDirty = true;
            }
            if (input.textDirty) {
                GlyphEncodeOptions o;
                o.cap = cap;
                o.shimCap = shimCap;
                o.caseMode = input.textCase == "upper" ? GlyphEncodeOptions::Upper
                           : input.textCase == "lower" ? GlyphEncodeOptions::Lower
                                                        : GlyphEncodeOptions::Preserve;
                o.overflowHead = input.textOverflowHead;
                input.textEncoded = GlyphBuffer::encodeTextRun(input.textRun, o);
                if (input.textBufRef) {
                    GLuint& tex = m_textBufTex[input.name];
                    if (!tex) glGenTextures(1, &tex);
                    glActiveTexture(GL_TEXTURE0 + 13 + std::min(textBufIndex, 2));
                    glBindTexture(GL_TEXTURE_2D, tex);
                    GLint prevAlign = 4;
                    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cap * 2, 1, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, input.textEncoded.bytes.data());
                    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                input.textDirty = false;
            }
            const GlyphEncoded& enc = input.textEncoded;
            // `<n>_len`: glyph slots after OVERFLOW (shim-only shaders: the shim window)
            int len = (input.textShim && !input.textBufRef) ? enc.shimLen : enc.len;
            m_shader.setInt(input.name + "_len", len);
            if (input.textShim) {
                for (int i = 0; i < shimCap && i < (int)enc.shimCodes.size(); i++) {
                    m_shader.setInt(input.name + "_" + std::to_string(i), enc.shimCodes[i]);
                }
            }
            if (input.textBufRef) {
                if (textBufIndex < 3) {
                    int unit = 13 + textBufIndex;
                    auto it = m_textBufTex.find(input.name);
                    glActiveTexture(GL_TEXTURE0 + unit);
                    glBindTexture(GL_TEXTURE_2D, it != m_textBufTex.end() ? it->second : 0);
                    m_shader.setInt("textBuf_" + input.name, unit);
                    m_shader.setFloat(input.name + "_cap", (float)cap);
                } else if (!m_loggedTextBufOverflow) {
                    m_loggedTextBufOverflow = true;
                    std::cerr << "ShaderSource: more than 3 glyph-buffer text inputs; '"
                              << input.name << "' not bound (" << m_path << ")" << std::endl;
                }
                textBufIndex++;
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
    setTextRun(name, TextRun{text});
}

void ShaderSource::setTextRun(const std::string& name, const TextRun& run) {
    for (auto& input : m_inputs) {
        if (input.name == name && input.type == "text") {
            const std::string& cur = std::get<std::string>(input.value);
            bool same = (cur == run.text) && (input.textRun.text == run.text) &&
                        (input.textRun.speakerId == run.speakerId) &&
                        (input.textRun.words.size() == run.words.size());
            if (same) {
                for (size_t i = 0; i < run.words.size(); ++i) {
                    const auto& a = input.textRun.words[i]; const auto& b = run.words[i];
                    if (a.start != b.start || a.end != b.end || a.revealMs != b.revealMs ||
                        a.speakerId != b.speakerId) { same = false; break; }
                }
            }
            if (same) return;   // unchanged: no re-encode / re-upload
            input.value = run.text;
            input.textRun = run;
            input.textDirty = true;
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
                                      float dt, MIDIManager* midi,
                                      float energy, float build, float drop,
                                      float silence, float momentum) {
    // Clamp dt so a stall / first frame can't blow the exponential up. The
    // smoother is time-constant based (frame-rate independent): alpha is
    // derived from a per-second rate and the real frame dt, identical in
    // spirit to AudioAnalyzer::expSmooth.
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
    if (dt > 0.1f)    dt = 0.1f;

    for (auto& [paramName, binding] : m_audioBindings) {
        if (binding.signal == AudioSignal::None) continue;
        if (!m_audioEnabled && binding.signal != AudioSignal::MidiCC) {
            // Hold the current parameter value without destroying its recipe.
            // Re-enable from a fresh sample rather than a stale follower tail.
            binding.hasSmoothed = false;
            continue;
        }

        // Get raw signal value (0-1)
        float raw = 0.0f;
        bool haveRaw = true;
        switch (binding.signal) {
            case AudioSignal::Level: raw = level; break;
            case AudioSignal::Bass:  raw = bass; break;
            case AudioSignal::Mid:   raw = mid; break;
            case AudioSignal::High:  raw = high; break;
            case AudioSignal::Beat:  raw = beat; break;
            case AudioSignal::Energy:   raw = energy;   break;
            case AudioSignal::Build:    raw = build;    break;
            case AudioSignal::Drop:     raw = drop;     break;
            case AudioSignal::Silence:  raw = silence;  break;
            case AudioSignal::Momentum: raw = momentum; break;
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

        // Frame-rate-independent asymmetric follower + range mapping, now
        // shared with FluidSource via AudioBinding::follow(). [rangeMin,
        // rangeMax] IS the effective depth/strength (default rangeMin=paramMin,
        // rangeMax=paramMax preserves the original 1:1 mapping).
        float mapped = binding.follow(raw, dt);

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

void ShaderSource::setAudioState(float rms, float bass, float mid, float high, GLuint fftTex) {
    m_audioRMS = m_audioEnabled ? rms : 0.0f;
    m_audioBass = m_audioEnabled ? bass : 0.0f;
    m_audioMid = m_audioEnabled ? mid : 0.0f;
    m_audioHigh = m_audioEnabled ? high : 0.0f;
    m_audioFFTTex = m_audioEnabled ? fftTex : 0;
}

void ShaderSource::setAudioEnabled(bool enabled) {
    m_audioEnabled = enabled;
    if (!enabled) setAudioFeatures(AudioFeatures{});
}

void ShaderSource::setAudioFeatures(const AudioFeatures& features) {
    m_af = m_audioEnabled ? features : AudioFeatures{};
    const auto& f = m_af;
    // Mirror the legacy fields so old shaders' audioLevel/Bass/Mid/High/FFT
    // keep their exact prior meaning (audioMid = merged lowMid+highMid).
    m_audioRMS  = f.level;
    m_audioBass = f.bass;
    m_audioMid  = (f.lowMid + f.highMid) * 0.5f;
    m_audioHigh = f.treble;
    m_audioFFTTex = f.fftTex;
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
    } else if (w > 0 && h > 0 && m_shader.id() != 0) {
        // Shader compiled but FBO creation failed at load time (0×0 dimensions).
        // Attempt late initialization now that we have valid dimensions.
        if (m_fbo.create(w, h)) {
            m_quad.createQuad();
            m_initialized = true;
            if (!m_passBuffers.empty()) {
                createPassFBOs();
            }
        }
    }
}
