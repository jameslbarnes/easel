#include "sources/ShaderLibrary.h"
#include "net/HttpClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeTextIfChanged(const fs::path& path, const std::string& text) {
    std::string existing = readText(path);
    if (existing == text) return true;
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << text;
    return true;
}

bool hasShaderExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".fs" || ext == ".frag" || ext == ".glsl";
}

std::string stemFromFile(const std::string& file) {
    fs::path p(file);
    return p.stem().string();
}

std::string sanitizeStem(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char ch : input) {
        if (std::isalnum(ch)) out.push_back((char)std::tolower(ch));
        else if (ch == '_' || ch == '-') out.push_back((char)ch);
        else if (ch == ' ' || ch == '.' || ch == '/') {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "shader" : out;
}

bool startsWithHttp(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string endpointBase(const std::string& endpointUrl) {
    size_t query = endpointUrl.find('?');
    std::string clean = endpointUrl.substr(0, query);
    size_t slash = clean.find_last_of('/');
    return (slash == std::string::npos) ? clean : clean.substr(0, slash + 1);
}

std::string endpointOrigin(const std::string& endpointUrl) {
    size_t schemeEnd = endpointUrl.find("://");
    if (schemeEnd == std::string::npos) return "";
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = endpointUrl.find('/', hostStart);
    return (pathStart == std::string::npos) ? endpointUrl : endpointUrl.substr(0, pathStart);
}

std::string resolveUrl(const std::string& endpointUrl, const std::string& value) {
    if (value.empty() || startsWithHttp(value)) return value;
    if (value[0] == '/') return endpointOrigin(endpointUrl) + value;
    return endpointBase(endpointUrl) + value;
}

void sortEntries(std::vector<ShaderLibrary::ShaderEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return a.title < b.title;
              });
}

std::vector<std::string> stringArrayFromJson(const json& item, const char* key) {
    std::vector<std::string> out;
    if (!item.contains(key) || !item[key].is_array()) return out;
    for (const auto& v : item[key]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string jsonString(const json& item, const char* key, const std::string& fallback = "") {
    if (!item.contains(key)) return fallback;
    const auto& v = item[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return fallback;
}

bool jsonBool(const json& item, const char* key, bool fallback = true) {
    if (!item.contains(key)) return fallback;
    const auto& v = item[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s == "true" || s == "1" || s == "yes";
    }
    return fallback;
}

const json* fieldValue(const json& fields, const char* key) {
    if (!fields.is_object()) return nullptr;
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &(*it);
}

std::string firestoreString(const json& fields, const char* key, const std::string& fallback = "") {
    const json* v = fieldValue(fields, key);
    if (!v || !v->is_object()) return fallback;
    if (v->contains("stringValue")) return (*v)["stringValue"].get<std::string>();
    if (v->contains("integerValue")) return (*v)["integerValue"].get<std::string>();
    if (v->contains("doubleValue")) return std::to_string((*v)["doubleValue"].get<double>());
    if (v->contains("timestampValue")) return (*v)["timestampValue"].get<std::string>();
    if (v->contains("booleanValue")) return (*v)["booleanValue"].get<bool>() ? "true" : "false";
    return fallback;
}

bool firestoreBool(const json& fields, const char* key, bool fallback = true) {
    const json* v = fieldValue(fields, key);
    if (!v || !v->is_object()) return fallback;
    if (v->contains("booleanValue")) return (*v)["booleanValue"].get<bool>();
    std::string s = firestoreString(fields, key, fallback ? "true" : "false");
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s == "true" || s == "1" || s == "yes";
}

std::vector<std::string> firestoreStringArray(const json& fields, const char* key) {
    std::vector<std::string> out;
    const json* v = fieldValue(fields, key);
    if (!v || !v->is_object() || !v->contains("arrayValue")) return out;
    const auto& arr = (*v)["arrayValue"];
    if (!arr.contains("values") || !arr["values"].is_array()) return out;
    for (const auto& entry : arr["values"]) {
        if (entry.contains("stringValue")) out.push_back(entry["stringValue"].get<std::string>());
    }
    return out;
}

std::string firestoreDocumentId(const json& doc) {
    std::string name = jsonString(doc, "name");
    size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

bool statusIsPublished(const std::string& status) {
    if (status.empty()) return true;
    std::string s = status;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s == "published" || s == "ready" || s == "active";
}

ShaderLibrary::ShaderEntry entryFromEaselJson(const json& item, const fs::path& shadersDir) {
    ShaderLibrary::ShaderEntry entry;
    entry.id = jsonString(item, "id");
    entry.title = jsonString(item, "title", "Untitled");
    entry.description = jsonString(item, "description");
    entry.file = jsonString(item, "file");
    entry.type = jsonString(item, "type", "generator");
    entry.updatedAt = jsonString(item, "updatedAt");
    entry.categories = stringArrayFromJson(item, "categories");

    if (entry.id.empty()) entry.id = sanitizeStem(!entry.file.empty() ? stemFromFile(entry.file) : entry.title);
    if (entry.file.empty()) entry.file = sanitizeStem(entry.id) + ".fs";
    entry.fullPath = (shadersDir / entry.file).string();
    return entry;
}

ShaderLibrary::ShaderEntry entryFromFirestoreJson(const json& doc, const fs::path& shadersDir) {
    const auto& fields = doc.contains("fields") ? doc["fields"] : json::object();
    ShaderLibrary::ShaderEntry entry;
    entry.id = firestoreString(fields, "id", firestoreDocumentId(doc));
    entry.title = firestoreString(fields, "title", entry.id.empty() ? "Untitled" : entry.id);
    entry.description = firestoreString(fields, "description");
    entry.file = firestoreString(fields, "file");
    entry.type = firestoreString(fields, "type", "generator");
    entry.updatedAt = firestoreString(fields, "updatedAt", firestoreString(fields, "updated_at"));
    entry.categories = firestoreStringArray(fields, "categories");

    if (entry.id.empty()) entry.id = sanitizeStem(!entry.file.empty() ? stemFromFile(entry.file) : entry.title);
    if (entry.file.empty()) entry.file = sanitizeStem(entry.id) + ".fs";
    entry.fullPath = (shadersDir / entry.file).string();
    return entry;
}

std::string sourceFromEaselJson(const json& item) {
    for (const char* key : {"fragment", "source", "code", "fs"}) {
        std::string value = jsonString(item, key);
        if (!value.empty()) return value;
    }
    return "";
}

std::string vertexFromEaselJson(const json& item) {
    for (const char* key : {"vertex", "vs"}) {
        std::string value = jsonString(item, key);
        if (!value.empty()) return value;
    }
    return "";
}

std::string sourceFromFirestoreJson(const json& fields) {
    for (const char* key : {"fragment", "source", "code", "fs"}) {
        std::string value = firestoreString(fields, key);
        if (!value.empty()) return value;
    }
    return "";
}

std::string vertexFromFirestoreJson(const json& fields) {
    for (const char* key : {"vertex", "vs"}) {
        std::string value = firestoreString(fields, key);
        if (!value.empty()) return value;
    }
    return "";
}

std::string fetchMaybeRelative(const std::string& endpointUrl, const std::string& urlValue) {
    std::string url = resolveUrl(endpointUrl, urlValue);
    if (url.empty()) return "";
    HttpResponse response = HttpClient::get(url, {}, 8);
    if (response.status >= 200 && response.status < 300) return response.body;
    return "";
}

} // namespace

ShaderLibrary::~ShaderLibrary() {
    shutdown();
}

bool ShaderLibrary::open(const std::string& cacheDir, const std::string& endpointUrl) {
    shutdown();

    m_cacheDir = cacheDir;
    m_shadersDir = (fs::path(cacheDir) / "shaders").string();
    m_manifestPath = (fs::path(cacheDir) / "manifest.json").string();
    m_endpointUrl = endpointUrl;
    fs::create_directories(m_shadersDir);

    m_ready = true;
    refreshManifest();
    if (m_shaders.empty()) {
        m_statusText = syncEnabled()
            ? "Waiting for shader library sync"
            : "Cloud sync disabled. Set EASEL_SHADER_LIBRARY_URL.";
    }
    if (syncEnabled()) requestSync();
    return true;
}

void ShaderLibrary::shutdown() {
    if (m_syncThread.joinable()) {
        m_syncThread.join();
    }
    m_syncRunning.store(false);
    m_hasPendingSync = false;
    m_ready = false;
    m_shaders.clear();
    m_watched.clear();
}

void ShaderLibrary::setEndpointUrl(const std::string& endpointUrl) {
    m_endpointUrl = endpointUrl;
    if (syncEnabled()) requestSync();
}

void ShaderLibrary::refreshManifest() {
    m_shaders.clear();

    std::ifstream file(m_manifestPath);
    if (file.is_open()) {
        try {
            json manifest;
            file >> manifest;
            const json* list = nullptr;
            if (manifest.is_array()) {
                list = &manifest;
            } else if (manifest.contains("shaders") && manifest["shaders"].is_array()) {
                list = &manifest["shaders"];
            }

            if (list) {
                fs::path shadersDir(m_shadersDir);
                for (const auto& item : *list) {
                    ShaderEntry entry = entryFromEaselJson(item, shadersDir);
                    if (entry.type == "scene") continue;
                    if (!fs::exists(entry.fullPath)) continue;
                    entry.remote = jsonBool(item, "remote", false);
                    m_shaders.push_back(std::move(entry));
                }
            }
        } catch (const json::exception& e) {
            std::cerr << "[ShaderLibrary] manifest parse error: " << e.what() << std::endl;
        }
    }

    if (m_shaders.empty() && fs::exists(m_shadersDir)) {
        for (const auto& entry : fs::directory_iterator(m_shadersDir)) {
            if (!entry.is_regular_file() || !hasShaderExtension(entry.path())) continue;
            ShaderEntry shader;
            shader.id = sanitizeStem(entry.path().stem().string());
            shader.title = entry.path().stem().string();
            shader.file = entry.path().filename().string();
            shader.fullPath = entry.path().string();
            shader.type = "generator";
            m_shaders.push_back(std::move(shader));
        }
    }

    sortEntries(m_shaders);
    m_statusText = "Loaded " + std::to_string(m_shaders.size()) + " cached shaders";
    m_revision++;
}

void ShaderLibrary::requestSync() {
    if (!syncEnabled() || !m_ready) return;
    if (m_syncRunning.exchange(true)) return;
    if (m_syncThread.joinable()) m_syncThread.join();

    m_lastSyncAttempt = std::chrono::steady_clock::now();
    m_statusText = "Syncing shader library...";
    std::string endpoint = m_endpointUrl;
    m_syncThread = std::thread([this, endpoint]() {
        SyncResult result = syncEndpoint(endpoint);
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingSync = std::move(result);
            m_hasPendingSync = true;
        }
        m_syncRunning.store(false);
    });
}

bool ShaderLibrary::importFromDirectory(const std::string& sourceDir) {
    fs::path srcDir(sourceDir);
    if (!fs::exists(srcDir) || !fs::is_directory(srcDir)) {
        m_statusText = "Import failed: directory not found";
        return false;
    }

    std::vector<ShaderEntry> imported;
    fs::path manifestPath = srcDir / "manifest.json";
    if (fs::exists(manifestPath)) {
        try {
            json manifest = json::parse(readText(manifestPath));
            if (manifest.is_array()) {
                for (const auto& item : manifest) {
                    if (jsonString(item, "type") == "scene") continue;
                    std::string file = jsonString(item, "file");
                    if (file.empty()) continue;

                    std::string folder = jsonString(item, "folder");
                    fs::path sourcePath = folder.empty()
                        ? (srcDir / file)
                        : (srcDir / folder / file);
                    if (!fs::exists(sourcePath)) continue;

                    ShaderEntry entry = entryFromEaselJson(item, m_shadersDir);
                    entry.file = sourcePath.filename().string();
                    entry.fullPath = (fs::path(m_shadersDir) / entry.file).string();
                    fs::copy_file(sourcePath, entry.fullPath, fs::copy_options::overwrite_existing);

                    fs::path sourceVs = sourcePath;
                    sourceVs.replace_extension(".vs");
                    if (fs::exists(sourceVs)) {
                        fs::copy_file(sourceVs,
                                      fs::path(m_shadersDir) / sourceVs.filename(),
                                      fs::copy_options::overwrite_existing);
                    }
                    imported.push_back(std::move(entry));
                }
            }
        } catch (const std::exception& e) {
            m_statusText = std::string("Import failed: ") + e.what();
            return false;
        }
    } else {
        for (const auto& entryPath : fs::directory_iterator(srcDir)) {
            if (!entryPath.is_regular_file() || !hasShaderExtension(entryPath.path())) continue;
            ShaderEntry entry;
            entry.id = sanitizeStem(entryPath.path().stem().string());
            entry.title = entryPath.path().stem().string();
            entry.file = entryPath.path().filename().string();
            entry.fullPath = (fs::path(m_shadersDir) / entry.file).string();
            fs::copy_file(entryPath.path(), entry.fullPath, fs::copy_options::overwrite_existing);

            fs::path sourceVs = entryPath.path();
            sourceVs.replace_extension(".vs");
            if (fs::exists(sourceVs)) {
                fs::copy_file(sourceVs,
                              fs::path(m_shadersDir) / sourceVs.filename(),
                              fs::copy_options::overwrite_existing);
            }
            imported.push_back(std::move(entry));
        }
    }

    if (imported.empty()) {
        m_statusText = "Import found no shader files";
        return false;
    }

    std::unordered_map<std::string, ShaderEntry> byFile;
    for (auto& entry : m_shaders) byFile[entry.file] = std::move(entry);
    for (auto& entry : imported) byFile[entry.file] = std::move(entry);

    m_shaders.clear();
    for (auto& [_, entry] : byFile) m_shaders.push_back(std::move(entry));
    sortEntries(m_shaders);
    writeCacheManifest(m_shaders);
    m_statusText = "Imported " + std::to_string(imported.size()) + " shaders";
    m_revision++;
    return true;
}

void ShaderLibrary::watchSource(const std::string& path, std::shared_ptr<ShaderSource> source) {
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

void ShaderLibrary::unwatchSource(const std::string& path) {
    m_watched.erase(
        std::remove_if(m_watched.begin(), m_watched.end(),
                       [&](const WatchEntry& w) { return w.path == path; }),
        m_watched.end());
}

std::string ShaderLibrary::pathForId(const std::string& id) const {
    if (id.empty()) return "";
    for (const auto& shader : m_shaders) {
        if (shader.id == id) return shader.fullPath;
    }
    return "";
}

std::string ShaderLibrary::idForPath(const std::string& path) const {
    if (path.empty()) return "";
    fs::path input;
    try {
        input = fs::weakly_canonical(path);
    } catch (...) {
        input = fs::path(path);
    }

    for (const auto& shader : m_shaders) {
        try {
            if (fs::weakly_canonical(shader.fullPath) == input) return shader.id;
        } catch (...) {
            if (shader.fullPath == path) return shader.id;
        }
    }
    return "";
}

void ShaderLibrary::update() {
    if (!m_ready) return;

    if (!m_syncRunning.load() && m_syncThread.joinable()) {
        m_syncThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_hasPendingSync) {
            if (m_pendingSync.ok) {
                m_shaders = std::move(m_pendingSync.shaders);
                sortEntries(m_shaders);
                m_statusText = m_pendingSync.message;
                m_revision++;
            } else {
                m_statusText = m_pendingSync.message;
            }
            m_pendingSync = {};
            m_hasPendingSync = false;
        }
    }

    if (syncEnabled() && !m_syncRunning.load()) {
        auto now = std::chrono::steady_clock::now();
        if (m_lastSyncAttempt.time_since_epoch().count() == 0 ||
            now - m_lastSyncAttempt > std::chrono::seconds(15)) {
            requestSync();
        }
    }

    for (auto& w : m_watched) {
        uint64_t modTime = getFileModTime(w.path);
        if (modTime != w.lastModTime && modTime != 0) {
            w.lastModTime = modTime;
            auto source = w.source.lock();
            if (!source) continue;

            std::ifstream file(w.path);
            if (!file.is_open()) continue;
            std::stringstream ss;
            ss << file.rdbuf();
            if (source->reload(ss.str())) {
                std::cout << "[ShaderLibrary] hot-reloaded " << w.path << std::endl;
            }
        }
    }
}

std::string ShaderLibrary::defaultCacheDir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home) return (fs::path(home) / ".easel" / "shader-library").string();
    return ".easel\\shader-library";
#else
    const char* home = std::getenv("HOME");
    if (home) return (fs::path(home) / ".easel" / "shader-library").string();
    return ".easel/shader-library";
#endif
}

std::string ShaderLibrary::defaultEndpointUrl() {
    const char* endpoint = std::getenv("EASEL_SHADER_LIBRARY_URL");
    if (endpoint && endpoint[0]) return endpoint;
    endpoint = std::getenv("EASEL_SHADER_FIRESTORE_URL");
    if (endpoint && endpoint[0]) return endpoint;
    return "https://firestore.googleapis.com/v1/projects/etherea-aa67d/databases/(default)/documents/easel_shaders?key=AIzaSyCzr0uLyLkQXoTSMFaabxu5uQoj1SgfaKc&pageSize=500";
}

ShaderLibrary::SyncResult ShaderLibrary::syncEndpoint(const std::string& endpointUrl) {
    SyncResult result;
    HttpResponse response = HttpClient::get(endpointUrl, {{"Accept", "application/json"}}, 8);
    if (response.status < 200 || response.status >= 300) {
        result.message = "Shader sync failed";
        if (!response.error.empty()) result.message += ": " + response.error;
        else if (response.status != 0) result.message += ": HTTP " + std::to_string(response.status);
        return result;
    }

    json root;
    try {
        root = json::parse(response.body);
    } catch (const json::exception& e) {
        result.message = std::string("Shader sync failed: invalid JSON: ") + e.what();
        return result;
    }

    fs::path shadersDir(m_shadersDir);
    std::vector<ShaderEntry> entries;
    try {
        if (root.contains("documents") && root["documents"].is_array()) {
            for (const auto& doc : root["documents"]) {
                const auto& fields = doc.contains("fields") ? doc["fields"] : json::object();
                if (!firestoreBool(fields, "enabled", true)) continue;
                if (!statusIsPublished(firestoreString(fields, "status"))) continue;

                ShaderEntry entry = entryFromFirestoreJson(doc, shadersDir);
                if (entry.type == "scene") continue;

                std::string fragment = sourceFromFirestoreJson(fields);
                if (fragment.empty()) {
                    fragment = fetchMaybeRelative(endpointUrl, firestoreString(fields, "fragmentUrl"));
                }
                if (fragment.empty()) {
                    fragment = fetchMaybeRelative(endpointUrl, firestoreString(fields, "sourceUrl"));
                }
                if (fragment.empty()) continue;

                entry.remote = true;
                fs::path shaderPath = shadersDir / entry.file;
                writeTextIfChanged(shaderPath, fragment);

                std::string vertex = vertexFromFirestoreJson(fields);
                if (vertex.empty()) {
                    vertex = fetchMaybeRelative(endpointUrl, firestoreString(fields, "vertexUrl"));
                }
                if (!vertex.empty()) {
                    fs::path vertexPath = shaderPath;
                    vertexPath.replace_extension(".vs");
                    writeTextIfChanged(vertexPath, vertex);
                }
                entry.fullPath = shaderPath.string();
                entries.push_back(std::move(entry));
            }
        } else {
            const json* list = nullptr;
            if (root.is_array()) list = &root;
            else if (root.contains("shaders") && root["shaders"].is_array()) list = &root["shaders"];

            if (!list) {
                result.message = "Shader sync failed: no shaders array";
                return result;
            }

            for (const auto& item : *list) {
                if (!jsonBool(item, "enabled", true)) continue;
                if (!statusIsPublished(jsonString(item, "status"))) continue;

                ShaderEntry entry = entryFromEaselJson(item, shadersDir);
                if (entry.type == "scene") continue;

                std::string fragment = sourceFromEaselJson(item);
                if (fragment.empty()) fragment = fetchMaybeRelative(endpointUrl, jsonString(item, "fragmentUrl"));
                if (fragment.empty()) fragment = fetchMaybeRelative(endpointUrl, jsonString(item, "sourceUrl"));
                if (fragment.empty()) fragment = fetchMaybeRelative(endpointUrl, jsonString(item, "url"));
                if (fragment.empty()) continue;

                entry.remote = true;
                fs::path shaderPath = shadersDir / entry.file;
                writeTextIfChanged(shaderPath, fragment);

                std::string vertex = vertexFromEaselJson(item);
                if (vertex.empty()) vertex = fetchMaybeRelative(endpointUrl, jsonString(item, "vertexUrl"));
                if (!vertex.empty()) {
                    fs::path vertexPath = shaderPath;
                    vertexPath.replace_extension(".vs");
                    writeTextIfChanged(vertexPath, vertex);
                }
                entry.fullPath = shaderPath.string();
                entries.push_back(std::move(entry));
            }
        }
    } catch (const std::exception& e) {
        result.message = std::string("Shader sync failed: ") + e.what();
        return result;
    }

    sortEntries(entries);
    writeCacheManifest(entries);
    result.ok = true;
    result.message = "Synced " + std::to_string(entries.size()) + " shaders";
    result.shaders = std::move(entries);
    return result;
}

bool ShaderLibrary::writeCacheManifest(const std::vector<ShaderEntry>& shaders) {
    json root;
    root["schema"] = "easel.shaderLibrary.v1";
    root["shaders"] = json::array();
    for (const auto& entry : shaders) {
        json item;
        item["id"] = entry.id;
        item["title"] = entry.title;
        item["description"] = entry.description;
        item["file"] = entry.file;
        item["type"] = entry.type;
        item["updatedAt"] = entry.updatedAt;
        item["remote"] = entry.remote;
        item["categories"] = entry.categories;
        root["shaders"].push_back(std::move(item));
    }
    return writeTextIfChanged(m_manifestPath, root.dump(2));
}

uint64_t ShaderLibrary::getFileModTime(const std::string& path) const {
    try {
        auto ftime = fs::last_write_time(path);
        return (uint64_t)ftime.time_since_epoch().count();
    } catch (...) {
        return 0;
    }
}
