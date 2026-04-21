#include "timeline/Timeline.h"
#include "compositing/LayerStack.h"
#include "compositing/Layer.h"
#include "sources/VideoSource.h"
#include "sources/ShaderSource.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <unordered_set>

void Timeline::seek(double t) {
    if (t < 0.0) t = 0.0;
    if (t > m_duration) t = m_duration;
    m_playhead = t;
    // Clear per-track runtime so applyToLayers re-evaluates every track (handles
    // backward / out-of-order seeks that would otherwise skip edge firing).
    m_runtime.clear();
}

void Timeline::advance(double dt) {
    if (!m_playing) return;
    m_playhead += dt;
    if (m_playhead >= m_duration) {
        if (m_loop) {
            m_playhead = 0.0;
        } else {
            m_playhead = m_duration;
            m_playing = false;
        }
    }
}

TimelineTrack* Timeline::findTrack(uint32_t layerId) {
    for (auto& t : m_tracks) if (t.layerId == layerId) return &t;
    return nullptr;
}

TimelineTrack& Timeline::ensureTrack(uint32_t layerId, const std::string& name) {
    if (layerId == 0) {
        // Defense against layers whose id hasn't been assigned yet. Return a
        // static dummy — no layer with id 0 exists, so this track never renders.
        static TimelineTrack dummy;
        return dummy;
    }
    if (auto* t = findTrack(layerId)) return *t;
    m_tracks.push_back({layerId, name, {}, false, false});
    return m_tracks.back();
}

void Timeline::removeTrackForLayer(uint32_t layerId) {
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                   [layerId](const TimelineTrack& t){ return t.layerId == layerId; }),
                   m_tracks.end());
    m_runtime.erase(std::remove_if(m_runtime.begin(), m_runtime.end(),
                    [layerId](const TrackRuntime& r){ return r.layerId == layerId; }),
                    m_runtime.end());
}

TimelineClip* Timeline::addClip(uint32_t layerId, double start, double dur,
                                const std::string& name, const std::string& sourcePath) {
    if (layerId == 0) return nullptr;
    auto& track = ensureTrack(layerId, name.empty() ? "Layer" : name);
    TimelineClip c;
    c.id = m_nextClipId++;
    c.startTime = start < 0.0 ? 0.0 : start;
    c.duration  = dur < 0.1 ? 0.1 : dur;
    c.name = name;
    c.sourcePath = sourcePath;
    track.clips.push_back(c);
    sortTrack(layerId);
    return findClip(layerId, c.id);
}

void Timeline::removeClip(uint32_t layerId, uint32_t clipId) {
    auto* track = findTrack(layerId);
    if (!track) return;
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(),
                       [clipId](const TimelineClip& c){ return c.id == clipId; }),
                       track->clips.end());
}

TimelineClip* Timeline::findClip(uint32_t layerId, uint32_t clipId) {
    auto* track = findTrack(layerId);
    if (!track) return nullptr;
    for (auto& c : track->clips) if (c.id == clipId) return &c;
    return nullptr;
}

void Timeline::sortTrack(uint32_t layerId) {
    auto* track = findTrack(layerId);
    if (!track) return;
    std::sort(track->clips.begin(), track->clips.end(),
              [](const TimelineClip& a, const TimelineClip& b){ return a.startTime < b.startTime; });
}

Timeline::TrackRuntime& Timeline::runtimeFor(uint32_t layerId) {
    for (auto& r : m_runtime) if (r.layerId == layerId) return r;
    m_runtime.push_back({layerId, 0});
    return m_runtime.back();
}

void Timeline::clear() {
    m_tracks.clear();
    m_runtime.clear();
    m_playhead = 0.0;
    m_playing = false;
    m_nextClipId = 1;
}

nlohmann::json Timeline::toJson() const {
    nlohmann::json j;
    j["duration"] = m_duration;
    j["playhead"] = m_playhead;
    j["loop"] = m_loop;
    j["nextClipId"] = m_nextClipId;
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& tr : m_tracks) {
        nlohmann::json tj;
        tj["layerId"] = tr.layerId;
        tj["name"] = tr.name;
        tj["muted"] = tr.muted;
        tj["solo"] = tr.solo;
        nlohmann::json clips = nlohmann::json::array();
        for (const auto& c : tr.clips) {
            nlohmann::json cj;
            cj["id"] = c.id;
            cj["startTime"] = c.startTime;
            cj["duration"] = c.duration;
            if (!c.name.empty()) cj["name"] = c.name;
            if (!c.sourcePath.empty()) cj["sourcePath"] = c.sourcePath;
            clips.push_back(cj);
        }
        tj["clips"] = clips;
        tracks.push_back(tj);
    }
    j["tracks"] = tracks;
    return j;
}

void Timeline::fromJson(const nlohmann::json& j) {
    clear();
    if (j.is_null() || !j.is_object()) return;
    m_duration = j.value("duration", 300.0);
    m_playhead = j.value("playhead", 0.0);
    m_loop = j.value("loop", false);
    m_nextClipId = j.value("nextClipId", (uint32_t)1);
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const auto& tj : j["tracks"]) {
            TimelineTrack tr;
            tr.layerId = tj.value("layerId", (uint32_t)0);
            tr.name = tj.value("name", std::string{});
            tr.muted = tj.value("muted", false);
            tr.solo = tj.value("solo", false);
            if (tj.contains("clips") && tj["clips"].is_array()) {
                for (const auto& cj : tj["clips"]) {
                    TimelineClip c;
                    c.id = cj.value("id", m_nextClipId++);
                    c.startTime = cj.value("startTime", 0.0);
                    c.duration = cj.value("duration", 5.0);
                    c.name = cj.value("name", std::string{});
                    c.sourcePath = cj.value("sourcePath", std::string{});
                    if (c.id >= m_nextClipId) m_nextClipId = c.id + 1;
                    tr.clips.push_back(c);
                }
            }
            if (tr.layerId != 0) m_tracks.push_back(std::move(tr));
        }
    }
}

// Build a content source from a file path. Heuristic: .fs / .frag / .isf → ShaderSource,
// anything else → VideoSource (ffmpeg decode). Returns nullptr on failure / unsupported.
static std::shared_ptr<ContentSource> loadSourceForPath(const std::string& path) {
    if (path.empty()) return nullptr;
    // Extension sniff
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    if (ext == ".fs" || ext == ".frag" || ext == ".isf") {
        auto s = std::make_shared<ShaderSource>();
        if (!s->loadFromFile(path)) return nullptr;
        return s;
    }
#ifdef HAS_FFMPEG
    auto v = std::make_shared<VideoSource>();
    if (!v->load(path)) return nullptr;
    return v;
#else
    return nullptr;
#endif
}

void Timeline::applyToLayers(LayerStack& layers) {
    double t = m_playhead;

    // Build a solo set (if any track is soloed, only soloed tracks play).
    bool anySolo = false;
    for (auto& tr : m_tracks) if (tr.solo) { anySolo = true; break; }

    for (auto& track : m_tracks) {
        // Find current-frame active clip (first clip whose [start, end) contains t).
        const TimelineClip* active = nullptr;
        for (const auto& c : track.clips) {
            if (c.contains(t)) { active = &c; break; }
        }

        // Find the layer
        Layer* layer = nullptr;
        for (int i = 0; i < layers.count(); i++) {
            auto l = layers[i];
            if (l && l->id == track.layerId) { layer = l.get(); break; }
        }
        if (!layer) continue;

        // Mute / solo gating — silent tracks still advance their runtime so
        // un-muting mid-show doesn't replay every edge.
        bool gated = track.muted || (anySolo && !track.solo);

        auto& rt = runtimeFor(track.layerId);
        uint32_t prevActiveId = rt.activeClipId;
        uint32_t curActiveId  = active ? active->id : 0;

        if (!gated) {
            // Edge: entering a new clip (or starting fresh).
            if (curActiveId != prevActiveId && curActiveId != 0) {
                // Show the layer.
                layer->visible = true;
                if (!layer->transitionActive && !layer->shaderTransitionActive) {
                    layer->transitionProgress = 1.0f;
                }
                // If clip has a sourcePath different from current, hot-load + optionally transition.
                if (!active->sourcePath.empty()) {
                    auto src = loadSourceForPath(active->sourcePath);
                    if (src) {
                        if (layer->source && layer->transitionType == TransitionType::Shader
                            && !layer->transitionShaderPath.empty() && prevActiveId != 0)
                        {
                            layer->startShaderTransition(src);
                        } else {
                            // Warn once per layer if Shader transition is selected but no
                            // shader path is set — user likely forgot to pick one.
                            if (layer->transitionType == TransitionType::Shader
                                && layer->transitionShaderPath.empty() && prevActiveId != 0)
                            {
                                static std::unordered_set<uint32_t> s_warned;
                                if (s_warned.insert(layer->id).second) {
                                    std::cerr << "[Timeline] Layer " << layer->id
                                              << " (\"" << layer->name
                                              << "\") has transitionType=Shader but no "
                                                 "transitionShaderPath — falling back to instant swap."
                                              << std::endl;
                                }
                            }
                            layer->source = src;
                        }
                    }
                }
            }
            // Edge: exiting the last clip (no overlap successor).
            if (curActiveId == 0 && prevActiveId != 0) {
                // Fade out at clip boundary (no transitionActive fade-out already in flight)
                if (!layer->transitionActive && layer->transitionDuration > 0.0f) {
                    layer->transitionDirection = false;
                    layer->transitionActive = true;
                } else if (layer->transitionDuration <= 0.0f) {
                    layer->visible = false;
                }
            }
        }

        rt.activeClipId = curActiveId;
    }
}
