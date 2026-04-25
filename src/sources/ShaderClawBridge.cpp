#include "sources/ShaderClawBridge.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

ShaderClawBridge::~ShaderClawBridge() {
    disconnect();
}

bool ShaderClawBridge::connect(const std::string& shadersDir) {
    // Verify the directory exists
    if (!fs::exists(shadersDir) || !fs::is_directory(shadersDir)) {
        std::cerr << "ShaderClaw: directory not found: " << shadersDir << std::endl;
        return false;
    }

    m_shadersDir = shadersDir;
    m_connected = true;

    // Load the manifest
    refreshManifest();

    // Set up file change notification
#ifdef _WIN32
    m_watchHandle = FindFirstChangeNotificationA(
        shadersDir.c_str(),
        FALSE, // don't watch subtree
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME
    );
    if (m_watchHandle == INVALID_HANDLE_VALUE) {
        std::cerr << "ShaderClaw: failed to watch directory" << std::endl;
    }
#endif

    std::cout << "ShaderClaw: connected to " << shadersDir
              << " (" << m_shaders.size() << " shaders)" << std::endl;
    return true;
}

void ShaderClawBridge::disconnect() {
#ifdef _WIN32
    if (m_watchHandle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(m_watchHandle);
        m_watchHandle = INVALID_HANDLE_VALUE;
    }
#endif
    m_connected = false;
    m_shaders.clear();
    m_watched.clear();
}

void ShaderClawBridge::refreshManifest() {
    m_shaders.clear();

    std::string manifestPath = m_shadersDir + "/manifest.json";
    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        // No manifest — scan for .fs files directly
        for (const auto& entry : fs::directory_iterator(m_shadersDir)) {
            if (entry.path().extension() == ".fs") {
                ShaderEntry se;
                se.file = entry.path().filename().string();
                se.fullPath = entry.path().string();
                se.title = entry.path().stem().string();
                se.type = "generator";
                m_shaders.push_back(std::move(se));
            }
        }
        std::sort(m_shaders.begin(), m_shaders.end(),
                  [](const ShaderEntry& a, const ShaderEntry& b) {
                      return a.title < b.title;
                  });
        return;
    }

    try {
        json manifest;
        file >> manifest;

        if (!manifest.is_array()) return;

        for (const auto& item : manifest) {
            ShaderEntry se;
            se.title = item.value("title", "Untitled");
            se.description = item.value("description", "");
            se.file = item.value("file", "");
            se.type = item.value("type", "generator");

            if (item.contains("categories") && item["categories"].is_array()) {
                for (const auto& cat : item["categories"]) {
                    se.categories.push_back(cat.get<std::string>());
                }
            }

            // Skip Three.js scene entries — they're JavaScript (.scene.js),
            // and Easel's ISF pipeline only understands GLSL fragment shaders.
            // Listing them would surface broken tiles (e.g. "Cube Explosion").
            if (se.type == "scene") continue;

            if (!se.file.empty()) {
                // Manifest sometimes groups scenes under a "scenes" subfolder;
                // for regular shaders the file path is relative to shadersDir.
                std::string folder;
                if (item.contains("folder") && item["folder"].is_string()) {
                    folder = item["folder"].get<std::string>();
                }
                se.fullPath = folder.empty()
                    ? (m_shadersDir + "/" + se.file)
                    : (m_shadersDir + "/" + folder + "/" + se.file);
                m_shaders.push_back(std::move(se));
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "ShaderClaw: manifest parse error: " << e.what() << std::endl;
    }

    // Sort shaders alphabetically by title
    std::sort(m_shaders.begin(), m_shaders.end(),
              [](const ShaderEntry& a, const ShaderEntry& b) {
                  return a.title < b.title;
              });
}

std::string ShaderClawBridge::pollFileChanges() {
#ifdef _WIN32
    if (m_watchHandle == INVALID_HANDLE_VALUE) return "";

    DWORD result = WaitForSingleObject(m_watchHandle, 0);
    if (result == WAIT_OBJECT_0) {
        // Directory changed — re-register for next notification
        FindNextChangeNotification(m_watchHandle);
        return "changed"; // caller should check specific files
    }
#endif
    return "";
}

void ShaderClawBridge::watchSource(const std::string& path, std::shared_ptr<ShaderSource> source) {
    // Check if already watching this path
    for (auto& w : m_watched) {
        if (w.path == path) {
            w.source = source;
            w.lastModTime = getFileModTime(path);
            return;
        }
    }

    WatchEntry entry;
    entry.path = path;
    entry.source = source;
    entry.lastModTime = getFileModTime(path);
    m_watched.push_back(std::move(entry));
}

void ShaderClawBridge::unwatchSource(const std::string& path) {
    m_watched.erase(
        std::remove_if(m_watched.begin(), m_watched.end(),
            [&](const WatchEntry& w) { return w.path == path; }),
        m_watched.end()
    );
}

void ShaderClawBridge::update() {
    if (!m_connected) return;

    // Check if directory has changed
    std::string change = pollFileChanges();
    if (change.empty()) return;

    // Check each watched file for modification
    for (auto& w : m_watched) {
        uint64_t modTime = getFileModTime(w.path);
        if (modTime != w.lastModTime && modTime != 0) {
            w.lastModTime = modTime;

            auto source = w.source.lock();
            if (source) {
                // Read new source and hot-reload
                std::ifstream file(w.path);
                if (file.is_open()) {
                    std::stringstream ss;
                    ss << file.rdbuf();
                    std::string newCode = ss.str();

                    if (source->reload(newCode)) {
                        std::cout << "ShaderClaw: hot-reloaded " << w.path << std::endl;
                    }
                }
            }
        }
    }
}

uint64_t ShaderClawBridge::getFileModTime(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER uli;
        uli.LowPart = data.ftLastWriteTime.dwLowDateTime;
        uli.HighPart = data.ftLastWriteTime.dwHighDateTime;
        return uli.QuadPart;
    }
#else
    try {
        auto ftime = fs::last_write_time(path);
        return (uint64_t)ftime.time_since_epoch().count();
    } catch (...) {}
#endif
    return 0;
}
