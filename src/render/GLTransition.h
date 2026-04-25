#pragma once
#include <glad/glad.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// Runs a single gl-transitions.com shader between two textures.
//
// GL Transitions shaders follow a simple contract:
//   vec4 transition(vec2 uv);   // returns the blended color
//   uniform float progress;     // 0..1
//   uniform float ratio;        // width / height
//   // plus vec4 getFromColor(vec2), getToColor(vec2) helpers
//
// We inject a GLSL 330 wrapper that wires up the helpers, the standard
// uniforms, and Easel-specific audio uniforms (audioRMS/audioBass/audioMid
// /audioHigh/audioBeat) so any transition can react to sound without the
// shader author having to know.
class GLTransition {
public:
    GLTransition() = default;
    ~GLTransition();
    GLTransition(const GLTransition&)            = delete;
    GLTransition& operator=(const GLTransition&) = delete;

    // Load + compile from disk. Returns false + fills errorLog() on failure.
    bool loadFromFile(const std::string& path);

    // Blend fromTex → toTex at the given progress. Renders a fullscreen quad
    // to whatever FBO/viewport is currently bound by the caller.
    void render(GLuint fromTex, GLuint toTex, float progress,
                int width, int height,
                float audioRMS  = 0.0f,
                float audioBass = 0.0f,
                float audioMid  = 0.0f,
                float audioHigh = 0.0f,
                float audioBeat = 0.0f);

    bool isValid() const { return m_program != 0; }
    const std::string& errorLog() const { return m_error; }
    void release();

private:
    bool compile(const std::string& fragBody);

    GLuint m_program = 0;
    GLuint m_vao = 0, m_vbo = 0;
    std::string m_error;

    GLint m_uFrom = -1, m_uTo = -1, m_uProgress = -1, m_uRatio = -1;
    GLint m_uAudioRMS = -1, m_uAudioBass = -1, m_uAudioMid = -1,
          m_uAudioHigh = -1, m_uAudioBeat = -1;
};

// Project-wide registry. Scans assets/transitions/gl/*.glsl on startup; each
// transition is lazy-compiled on first use (compile is ~1ms, but no need to
// pay it for transitions the user never picks). Call instance() from the
// render thread only — GL objects live on one context.
//
// Supports scanning multiple directories (bundled + user shaders) and cheap
// hot-reload via file-mtime checks.
class GLTransitionLibrary {
public:
    static GLTransitionLibrary& instance();

    // One-time scan of a directory. Clears any previously scanned entries.
    void scan(const std::string& dir);

    // Adds another directory to the scan set without clearing. Same-named
    // entries in the new dir override those from earlier scans (so a user's
    // `dissolve.glsl` can shadow the bundled one).
    void scanAdditional(const std::string& dir);

    // Per-frame low-cost maintenance: re-stat files in every scanned dir,
    // invalidate cached runners whose source file changed, and pick up newly
    // added files. Caller should throttle (e.g., once per second).
    void checkAndReload();

    // Sorted list of names (for UI dropdowns).
    std::vector<std::string> names() const;
    bool has(const std::string& name) const;

    // Get (lazy-compile) a transition by name. Returns nullptr if unknown or
    // compile failed; caller can check runner->errorLog().
    GLTransition* get(const std::string& name);

private:
    struct Entry {
        std::string path;
        std::unique_ptr<GLTransition> runner; // nullptr until first use
        bool triedCompile = false;
        int64_t mtimeRaw = 0; // raw rep of fs::last_write_time for hot-reload
    };
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<std::string> m_scannedDirs;   // remembered for checkAndReload
};
