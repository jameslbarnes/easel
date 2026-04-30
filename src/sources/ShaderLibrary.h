#pragma once

#include "sources/ShaderSource.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Easel-owned shader library.
//
// ShaderClaw is expected to publish ready shaders to a remote Easel endpoint
// or Firestore-style document feed. Easel clients sync that feed into this
// local cache and load shaders from the cache, so runtime use does not depend
// on ShaderClaw or GitHub.
class ShaderLibrary {
public:
    struct ShaderEntry {
        std::string id;
        std::string title;
        std::string description;
        std::string file;     // relative to the local cache shaders dir
        std::string fullPath; // absolute/local load path
        std::string type = "generator";
        std::string updatedAt;
        std::vector<std::string> categories;
        bool remote = false;
    };

    ShaderLibrary() = default;
    ~ShaderLibrary();

    bool open(const std::string& cacheDir, const std::string& endpointUrl = "");
    void shutdown();

    bool isReady() const { return m_ready; }
    bool syncEnabled() const { return !m_endpointUrl.empty(); }
    bool isSyncing() const { return m_syncRunning.load(); }
    const std::string& cacheDir() const { return m_cacheDir; }
    const std::string& endpointUrl() const { return m_endpointUrl; }
    const std::string& statusText() const { return m_statusText; }
    int revision() const { return m_revision; }

    void setEndpointUrl(const std::string& endpointUrl);
    void refreshManifest();
    void requestSync();
    bool importFromDirectory(const std::string& sourceDir);
    const std::vector<ShaderEntry>& shaders() const { return m_shaders; }
    std::string pathForId(const std::string& id) const;
    std::string idForPath(const std::string& path) const;

    void watchSource(const std::string& path, std::shared_ptr<ShaderSource> source);
    void unwatchSource(const std::string& path);
    void update();

    static std::string defaultCacheDir();
    static std::string defaultEndpointUrl();

private:
    struct WatchEntry {
        std::string path;
        std::weak_ptr<ShaderSource> source;
        uint64_t lastModTime = 0;
    };

    struct SyncResult {
        bool ok = false;
        std::string message;
        std::vector<ShaderEntry> shaders;
    };

    std::string m_cacheDir;
    std::string m_shadersDir;
    std::string m_manifestPath;
    std::string m_endpointUrl;
    std::string m_statusText;
    bool m_ready = false;
    int m_revision = 0;

    std::vector<ShaderEntry> m_shaders;
    std::vector<WatchEntry> m_watched;

    std::atomic<bool> m_syncRunning{false};
    std::thread m_syncThread;
    std::mutex m_pendingMutex;
    bool m_hasPendingSync = false;
    SyncResult m_pendingSync;
    std::chrono::steady_clock::time_point m_lastSyncAttempt{};

    SyncResult syncEndpoint(const std::string& endpointUrl);
    bool writeCacheManifest(const std::vector<ShaderEntry>& shaders);
    uint64_t getFileModTime(const std::string& path) const;
};
