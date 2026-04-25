#include "render/GLTransition.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// GL Transitions shaders target a few GLSL dialects; we normalize to 330 core.
// The GLSL-100 era spelling is texture2D(), which we alias to texture() so
// downloaded shaders compile without modification.
const char* kFragPrelude =
    "#version 330 core\n"
    "in vec2 vUv;\n"
    "out vec4 FragColor;\n"
    "\n"
    "uniform sampler2D from;\n"
    "uniform sampler2D to;\n"
    "uniform float progress;\n"
    "uniform float ratio;\n"
    "\n"
    "// Easel audio state — every transition can react to sound without the\n"
    "// shader author opting in. Unused uniforms are optimized out.\n"
    "uniform float audioRMS;\n"
    "uniform float audioBass;\n"
    "uniform float audioMid;\n"
    "uniform float audioHigh;\n"
    "uniform float audioBeat;\n"
    "\n"
    "#define texture2D(s, uv) texture(s, uv)\n"
    "\n"
    "vec4 getFromColor(vec2 uv) { return texture(from, uv); }\n"
    "vec4 getToColor(vec2 uv)   { return texture(to,   uv); }\n"
    "\n";

const char* kFragEpilogue =
    "\n"
    "void main() {\n"
    "    FragColor = transition(vUv);\n"
    "}\n";

const char* kVertSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vUv = aPos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// GL Transitions often put magic-default comments after uniform decls:
//     uniform vec2 direction; // = vec2(0.0, 1.0)
// They're for the Haxe port; in C++ we just ignore them — defaults are 0.
// But a bare "uniform X name;" with no body still compiles fine, so no-op.

// Strip any embedded `#version` lines from the user shader so our 330 core
// prelude is the only one GLSL sees. Also strip `precision …float;` (GLES).
//
// gl-transitions.com shaders use a convention where a uniform's default
// value is specified in a trailing comment, e.g.
//     uniform float persp; // = 0.7
// Since Easel only drives `progress` / `ratio` / audio uniforms, the rest
// would come through as zeroed, breaking transitions whose math depends on
// non-zero defaults (cube's persp=0.7, unzoom=0.3 are required). Convert
// those declarations into `const float NAME = VALUE;` so the defaults are
// baked in at compile time. Also handles uniform int, vec2/3/4, bool.
std::string sanitize(const std::string& src) {
    std::string s = std::regex_replace(src,
        std::regex(R"(#version\s+\d+[^\n]*)"), "");
    s = std::regex_replace(s,
        std::regex(R"(precision\s+\w+\s+float\s*;)"), "");

    // uniform float NAME; // = 0.7    →  const float NAME = 0.7;
    s = std::regex_replace(s,
        std::regex(R"(uniform\s+float\s+(\w+)\s*;\s*//\s*=\s*([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?))"),
        "const float $1 = $2;");
    // uniform int NAME; // = 3        →  const int NAME = 3;
    s = std::regex_replace(s,
        std::regex(R"(uniform\s+int\s+(\w+)\s*;\s*//\s*=\s*([-+]?[0-9]+))"),
        "const int $1 = $2;");
    // uniform bool NAME; // = true    →  const bool NAME = true;
    s = std::regex_replace(s,
        std::regex(R"(uniform\s+bool\s+(\w+)\s*;\s*//\s*=\s*(true|false))"),
        "const bool $1 = $2;");
    // uniform vec2 NAME; // = vec2(0.5, 0.5)  →  const vec2 NAME = vec2(0.5, 0.5);
    s = std::regex_replace(s,
        std::regex(R"(uniform\s+(vec[234])\s+(\w+)\s*;\s*//\s*=\s*(vec[234]\s*\([^)]*\)))"),
        "const $1 $2 = $3;");
    // uniform vec3/4 NAME; // = ivec3(...) also accepted via loose rule above.

    return s;
}

GLuint compileShader(GLenum type, const std::string& src, std::string& err) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        err = log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

GLTransition::~GLTransition() { release(); }

void GLTransition::release() {
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    if (m_vbo)     { glDeleteBuffers(1, &m_vbo);  m_vbo = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
}

bool GLTransition::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        m_error = "GLTransition: cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return compile(sanitize(ss.str()));
}

bool GLTransition::compile(const std::string& fragBody) {
    release();

    std::string fragSrc = std::string(kFragPrelude) + fragBody + kFragEpilogue;

    std::string err;
    GLuint vs = compileShader(GL_VERTEX_SHADER,   kVertSrc, err);
    if (!vs) { m_error = "vertex: " + err; return false; }
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc,  err);
    if (!fs) { m_error = "fragment: " + err; glDeleteShader(vs); return false; }

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(m_program, len, nullptr, log.data());
        m_error = "link: " + log;
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }

    m_uFrom      = glGetUniformLocation(m_program, "from");
    m_uTo        = glGetUniformLocation(m_program, "to");
    m_uProgress  = glGetUniformLocation(m_program, "progress");
    m_uRatio     = glGetUniformLocation(m_program, "ratio");
    m_uAudioRMS  = glGetUniformLocation(m_program, "audioRMS");
    m_uAudioBass = glGetUniformLocation(m_program, "audioBass");
    m_uAudioMid  = glGetUniformLocation(m_program, "audioMid");
    m_uAudioHigh = glGetUniformLocation(m_program, "audioHigh");
    m_uAudioBeat = glGetUniformLocation(m_program, "audioBeat");

    // Fullscreen triangle (two-tri quad) VAO/VBO.
    const float quad[] = {
        -1.f, -1.f,   1.f, -1.f,   -1.f, 1.f,
         1.f, -1.f,   1.f,  1.f,   -1.f, 1.f,
    };
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    return true;
}

void GLTransition::render(GLuint fromTex, GLuint toTex, float progress,
                          int width, int height,
                          float audioRMS, float audioBass,
                          float audioMid, float audioHigh, float audioBeat) {
    if (!m_program) return;
    glUseProgram(m_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fromTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, toTex);

    if (m_uFrom      >= 0) glUniform1i(m_uFrom, 0);
    if (m_uTo        >= 0) glUniform1i(m_uTo,   1);
    if (m_uProgress  >= 0) glUniform1f(m_uProgress,  progress);
    if (m_uRatio     >= 0) glUniform1f(m_uRatio,     height > 0 ? (float)width / (float)height : 1.0f);
    if (m_uAudioRMS  >= 0) glUniform1f(m_uAudioRMS,  audioRMS);
    if (m_uAudioBass >= 0) glUniform1f(m_uAudioBass, audioBass);
    if (m_uAudioMid  >= 0) glUniform1f(m_uAudioMid,  audioMid);
    if (m_uAudioHigh >= 0) glUniform1f(m_uAudioHigh, audioHigh);
    if (m_uAudioBeat >= 0) glUniform1f(m_uAudioBeat, audioBeat);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ── Library ────────────────────────────────────────────────────────────────

GLTransitionLibrary& GLTransitionLibrary::instance() {
    static GLTransitionLibrary lib;
    return lib;
}

void GLTransitionLibrary::scan(const std::string& dir) {
    m_entries.clear();
    m_scannedDirs.clear();
    scanAdditional(dir);
}

void GLTransitionLibrary::scanAdditional(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        std::cerr << "[GLTransitionLibrary] scanAdditional: directory missing: "
                  << dir << "\n";
        return;
    }
    m_scannedDirs.push_back(dir);
    int added = 0;
    for (auto& ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file()) continue;
        auto p = ent.path();
        if (p.extension() != ".glsl") continue;
        auto name = p.stem().string();
        Entry e;
        e.path = p.string();
        auto t = fs::last_write_time(p, ec);
        if (!ec) e.mtimeRaw = (int64_t)t.time_since_epoch().count();
        // Later dirs override earlier ones (user shadows bundled).
        m_entries[name] = std::move(e);
        added++;
    }
    std::cerr << "[GLTransitionLibrary] scanAdditional: +" << added
              << " from " << dir << "  (total " << m_entries.size() << ")\n";
}

void GLTransitionLibrary::checkAndReload() {
    std::error_code ec;

    // 1. Hot-reload: any entry whose source file's mtime changed gets its
    //    compiled runner discarded so get() recompiles on next use.
    for (auto& kv : m_entries) {
        Entry& e = kv.second;
        auto t = fs::last_write_time(e.path, ec);
        if (ec) continue;
        int64_t cur = (int64_t)t.time_since_epoch().count();
        if (cur != e.mtimeRaw) {
            e.mtimeRaw = cur;
            e.runner.reset();
            e.triedCompile = false;
            std::cerr << "[GLTransitionLibrary] hot-reload: " << kv.first << "\n";
        }
    }

    // 2. Discover new files dropped into any of the scanned dirs.
    for (auto& dir : m_scannedDirs) {
        for (auto& ent : fs::directory_iterator(dir, ec)) {
            if (!ent.is_regular_file()) continue;
            auto p = ent.path();
            if (p.extension() != ".glsl") continue;
            auto name = p.stem().string();
            if (m_entries.count(name)) continue;
            Entry e;
            e.path = p.string();
            auto t = fs::last_write_time(p, ec);
            if (!ec) e.mtimeRaw = (int64_t)t.time_since_epoch().count();
            m_entries.emplace(std::move(name), std::move(e));
            std::cerr << "[GLTransitionLibrary] picked up new transition: "
                      << p.string() << "\n";
        }
    }
}

std::vector<std::string> GLTransitionLibrary::names() const {
    std::vector<std::string> out;
    out.reserve(m_entries.size());
    for (auto& kv : m_entries) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool GLTransitionLibrary::has(const std::string& name) const {
    return m_entries.count(name) > 0;
}

GLTransition* GLTransitionLibrary::get(const std::string& name) {
    auto it = m_entries.find(name);
    if (it == m_entries.end()) return nullptr;
    auto& e = it->second;
    if (!e.runner && !e.triedCompile) {
        e.triedCompile = true;
        e.runner = std::make_unique<GLTransition>();
        if (!e.runner->loadFromFile(e.path)) {
            std::cerr << "[GLTransition] " << name << " compile failed: "
                      << e.runner->errorLog() << "\n";
            e.runner.reset();
        }
    }
    return e.runner.get();
}
