#pragma once
#include "sources/ShaderSource.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

// Monitors a Shader-Claw shaders directory for changes and provides
// a shader browser for loading ISF shaders into Easel layers.
class ShaderClawBridge {
public:
    struct ShaderEntry {
        std::string title;
        std::string description;
        std::string file; // relative to shaders dir
        std::string fullPath;
        std::string type;
        std::vector<std::string> categories;
    };

    ShaderClawBridge() = default;
    ~ShaderClawBridge();

    // Connect to a Shader-Claw shaders directory
    bool connect(const std::string& shadersDir);
    void disconnect();
    bool isConnected() const { return m_connected; }
    const std::string& shadersDir() const { return m_shadersDir; }

    // Reload manifest.json from the shaders directory
    void refreshManifest();
    const std::vector<ShaderEntry>& shaders() const { return m_shaders; }

    // Check for file changes (call once per frame)
    // Returns the path of the changed file, or empty string if no change
    std::string pollFileChanges();

    // Register a shader source for live reload when its file changes
    void watchSource(const std::string& path, std::shared_ptr<ShaderSource> source);
    void unwatchSource(const std::string& path);

    // Process file changes and hot-reload watched shaders
    void update();

private:
    std::string m_shadersDir;
    bool m_connected = false;
    std::vector<ShaderEntry> m_shaders;

    // File watching
#ifdef _WIN32
    HANDLE m_watchHandle = INVALID_HANDLE_VALUE;
#endif

    // Watched shader sources (path -> weak_ptr to ShaderSource)
    struct WatchEntry {
        std::string path;
        std::weak_ptr<ShaderSource> source;
        uint64_t lastModTime = 0;
    };
    std::vector<WatchEntry> m_watched;

    uint64_t getFileModTime(const std::string& path);
};
