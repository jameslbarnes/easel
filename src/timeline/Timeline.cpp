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
    m_tracks.push_back({layerId, name, {}});
    return m_tracks.back();
}

void Timeline::removeTrackForLayer(uint32_t layerId) {
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                   [layerId](const TimelineTrack& t){ return t.layerId == layerId; }),
                   m_tracks.end());
    m_runtime.erase(std::remove_if(m_runtime.begin(), m_runtime.end(),
                    [layerId](const TrackRuntime& r){ return r.layerId == layerId; }),
                    m_runtime.end());
    // Also scrub any between-row transitions that referenced this layer —
    // otherwise the UI keeps rendering orphan bars pointing at a dead layer
    // and applyToLayers() repeatedly fails to find the from/to layers.
    m_transitions.erase(std::remove_if(m_transitions.begin(), m_transitions.end(),
                    [layerId](const TimelineTransition& tr){
                        return tr.fromLayerId == layerId || tr.toLayerId == layerId;
                    }),
                    m_transitions.end());
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
    m_transitions.clear();
    m_markers.clear();
    m_sections.clear();
    m_lanes.clear();
    m_runtime.clear();
    m_playhead = 0.0;
    m_playing = false;
    m_nextClipId = 1;
    m_nextTransitionId = 1;
    m_nextMarkerId = 1;
    m_nextSectionId = 1;
    m_nextLaneId = 1;
}

TimelineTransition* Timeline::addTransition(uint32_t fromLayerId, uint32_t toLayerId,
                                            double start, double dur,
                                            const std::string& name) {
    if (fromLayerId == 0 || toLayerId == 0) return nullptr;
    TimelineTransition tr;
    tr.id = m_nextTransitionId++;
    tr.fromLayerId = fromLayerId;
    tr.toLayerId   = toLayerId;
    tr.startTime   = start < 0.0 ? 0.0 : start;
    tr.duration    = dur  < 0.1 ? 0.1 : dur;
    tr.name        = name.empty() ? "crossfade" : name;
    m_transitions.push_back(tr);
    return &m_transitions.back();
}

void Timeline::removeTransition(uint32_t transitionId) {
    m_transitions.erase(std::remove_if(m_transitions.begin(), m_transitions.end(),
                        [transitionId](const TimelineTransition& t){ return t.id == transitionId; }),
                        m_transitions.end());
}

TimelineTransition* Timeline::findTransition(uint32_t transitionId) {
    for (auto& t : m_transitions) if (t.id == transitionId) return &t;
    return nullptr;
}

// ─── Markers ─────────────────────────────────────────────────────────
uint32_t Timeline::addMarker(double time, const std::string& name,
                             const std::string& sceneName) {
    TimelineMarker m;
    m.id = m_nextMarkerId++;
    m.time = time < 0 ? 0 : time;
    m.name = name;
    m.sceneName = sceneName;
    m_markers.push_back(m);
    return m.id;
}
void Timeline::removeMarker(uint32_t id) {
    m_markers.erase(std::remove_if(m_markers.begin(), m_markers.end(),
                    [id](const TimelineMarker& mk){ return mk.id == id; }),
                    m_markers.end());
}
TimelineMarker* Timeline::findMarker(uint32_t id) {
    for (auto& mk : m_markers) if (mk.id == id) return &mk;
    return nullptr;
}

// ─── Lanes ───────────────────────────────────────────────────────────
uint32_t Timeline::addLane(uint32_t layerId, TimelineLaneKind kind,
                           const std::string& paramName) {
    TimelineLane l;
    l.id        = m_nextLaneId++;
    l.layerId   = layerId;
    l.kind      = kind;
    l.paramName = paramName.empty() ? std::string("opacity") : paramName;
    m_lanes.push_back(l);
    return l.id;
}
void Timeline::removeLane(uint32_t id) {
    m_lanes.erase(std::remove_if(m_lanes.begin(), m_lanes.end(),
                  [id](const TimelineLane& l){ return l.id == id; }),
                  m_lanes.end());
}
TimelineLane* Timeline::findLane(uint32_t id) {
    for (auto& l : m_lanes) if (l.id == id) return &l;
    return nullptr;
}

// ─── Sections ────────────────────────────────────────────────────────
uint32_t Timeline::addSection(double start, double end, const std::string& name) {
    if (end < start + 0.1) end = start + 0.1;
    TimelineSection s;
    s.id = m_nextSectionId++;
    s.startTime = start < 0 ? 0 : start;
    s.endTime = end;
    s.name = name;
    m_sections.push_back(s);
    return s.id;
}
void Timeline::removeSection(uint32_t id) {
    m_sections.erase(std::remove_if(m_sections.begin(), m_sections.end(),
                     [id](const TimelineSection& s){ return s.id == id; }),
                     m_sections.end());
}
TimelineSection* Timeline::findSection(uint32_t id) {
    for (auto& s : m_sections) if (s.id == id) return &s;
    return nullptr;
}

nlohmann::json Timeline::toJson() const {
    nlohmann::json j;
    j["duration"] = m_duration;
    j["playhead"] = m_playhead;
    j["loop"] = m_loop;
    j["nextClipId"] = m_nextClipId;
    j["workAreaStart"] = m_workAreaStart;
    j["workAreaEnd"]   = m_workAreaEnd;
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& tr : m_tracks) {
        nlohmann::json tj;
        tj["layerId"] = tr.layerId;
        tj["name"] = tr.name;
        nlohmann::json clips = nlohmann::json::array();
        for (const auto& c : tr.clips) {
            nlohmann::json cj;
            cj["id"] = c.id;
            cj["startTime"] = c.startTime;
            cj["duration"] = c.duration;
            if (c.sourceIn  != 0.0)  cj["sourceIn"]  = c.sourceIn;
            if (c.sourceOut >= 0.0)  cj["sourceOut"] = c.sourceOut;
            if (c.kind != ClipKind::Auto) cj["kind"] = (int)c.kind;
            if (c.tint != 0)         cj["tint"]     = c.tint;
            if (!c.name.empty()) cj["name"] = c.name;
            if (!c.sourcePath.empty()) cj["sourcePath"] = c.sourcePath;
            if (!c.transitionInName.empty()) {
                cj["transitionInName"]     = c.transitionInName;
                cj["transitionInDuration"] = c.transitionInDuration;
            }
            if (!c.transitionInShaderPath.empty()) {
                cj["transitionInShaderPath"] = c.transitionInShaderPath;
                cj["transitionInDuration"]   = c.transitionInDuration;
            }
            if (c.playbackMode != ClipPlaybackMode::Forward) {
                cj["playbackMode"] = (int)c.playbackMode;
            }
            clips.push_back(cj);
        }
        tj["clips"] = clips;
        tracks.push_back(tj);
    }
    j["tracks"] = tracks;

    nlohmann::json trs = nlohmann::json::array();
    for (const auto& tr : m_transitions) {
        nlohmann::json rj;
        rj["id"]          = tr.id;
        rj["fromLayerId"] = tr.fromLayerId;
        rj["toLayerId"]   = tr.toLayerId;
        rj["startTime"]   = tr.startTime;
        rj["duration"]    = tr.duration;
        rj["name"]        = tr.name;
        if (!tr.shaderPath.empty()) rj["shaderPath"] = tr.shaderPath;
        trs.push_back(rj);
    }
    j["transitions"]      = trs;
    j["nextTransitionId"] = m_nextTransitionId;

    nlohmann::json ls = nlohmann::json::array();
    for (const auto& l : m_lanes) {
        nlohmann::json lj;
        lj["id"]           = l.id;
        lj["layerId"]      = l.layerId;
        lj["kind"]         = (int)l.kind;
        lj["paramName"]    = l.paramName;
        lj["midiChannel"]  = l.midiChannel;
        lj["audioSignal"]  = l.audioSignal;
        lj["audioStrength"]= l.audioStrength;
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : l.points) {
            nlohmann::json pj;
            pj["time"]  = p.time;
            pj["value"] = p.value;
            pj["n"]     = p.noteOrCC;
            pts.push_back(pj);
        }
        lj["points"] = pts;
        ls.push_back(lj);
    }
    j["lanes"]      = ls;
    j["nextLaneId"] = m_nextLaneId;

    nlohmann::json ss = nlohmann::json::array();
    for (const auto& s : m_sections) {
        nlohmann::json sj;
        sj["id"]        = s.id;
        sj["startTime"] = s.startTime;
        sj["endTime"]   = s.endTime;
        sj["name"]      = s.name;
        if (s.tint != 0) sj["tint"] = s.tint;
        ss.push_back(sj);
    }
    j["sections"]      = ss;
    j["nextSectionId"] = m_nextSectionId;
    return j;
}

void Timeline::fromJson(const nlohmann::json& j) {
    clear();
    if (j.is_null() || !j.is_object()) return;
    m_duration = j.value("duration", 60.0);
    m_playhead = j.value("playhead", 0.0);
    m_loop = j.value("loop", false);
    m_nextClipId = j.value("nextClipId", (uint32_t)1);
    m_workAreaStart = j.value("workAreaStart", 0.0);
    m_workAreaEnd   = j.value("workAreaEnd",  -1.0);
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const auto& tj : j["tracks"]) {
            TimelineTrack tr;
            tr.layerId = tj.value("layerId", (uint32_t)0);
            tr.name = tj.value("name", std::string{});
            // NOTE: old files may carry "muted"/"solo" — they're ignored now.
            if (tj.contains("clips") && tj["clips"].is_array()) {
                for (const auto& cj : tj["clips"]) {
                    TimelineClip c;
                    c.id = cj.value("id", m_nextClipId++);
                    c.startTime = cj.value("startTime", 0.0);
                    c.duration = cj.value("duration", 5.0);
                    c.sourceIn  = cj.value("sourceIn",  0.0);
                    c.sourceOut = cj.value("sourceOut", -1.0);
                    c.kind = (ClipKind)cj.value("kind", (int)ClipKind::Auto);
                    c.tint = cj.value("tint", (uint32_t)0);
                    c.name = cj.value("name", std::string{});
                    c.sourcePath = cj.value("sourcePath", std::string{});
                    c.transitionInName       = cj.value("transitionInName",       std::string{});
                    c.transitionInShaderPath = cj.value("transitionInShaderPath", std::string{});
                    c.transitionInDuration   = cj.value("transitionInDuration",   0.5);
                    c.playbackMode           = (ClipPlaybackMode)cj.value("playbackMode",
                                                    (int)ClipPlaybackMode::Forward);
                    if (c.id >= m_nextClipId) m_nextClipId = c.id + 1;
                    tr.clips.push_back(c);
                }
            }
            if (tr.layerId != 0) m_tracks.push_back(std::move(tr));
        }
    }

    if (j.contains("transitions") && j["transitions"].is_array()) {
        for (const auto& rj : j["transitions"]) {
            TimelineTransition tr;
            tr.id          = rj.value("id",          m_nextTransitionId++);
            tr.fromLayerId = rj.value("fromLayerId", (uint32_t)0);
            tr.toLayerId   = rj.value("toLayerId",   (uint32_t)0);
            tr.startTime   = rj.value("startTime",   0.0);
            tr.duration    = rj.value("duration",    1.0);
            tr.name        = rj.value("name",        std::string{"crossfade"});
            tr.shaderPath  = rj.value("shaderPath",  std::string{});
            if (tr.id >= m_nextTransitionId) m_nextTransitionId = tr.id + 1;
            if (tr.fromLayerId && tr.toLayerId) m_transitions.push_back(tr);
        }
    }
    m_nextTransitionId = j.value("nextTransitionId", m_nextTransitionId);

    if (j.contains("lanes") && j["lanes"].is_array()) {
        for (const auto& lj : j["lanes"]) {
            TimelineLane l;
            l.id            = lj.value("id",            m_nextLaneId++);
            l.layerId       = lj.value("layerId",       (uint32_t)0);
            l.kind          = (TimelineLaneKind)lj.value("kind", (int)TimelineLaneKind::Automation);
            l.paramName     = lj.value("paramName",     std::string{"opacity"});
            l.midiChannel   = lj.value("midiChannel",   0);
            l.audioSignal   = lj.value("audioSignal",   0);
            l.audioStrength = lj.value("audioStrength", 0.5f);
            if (lj.contains("points") && lj["points"].is_array()) {
                for (const auto& pj : lj["points"]) {
                    TimelineLanePoint p;
                    p.time     = pj.value("time",  0.0);
                    p.value    = pj.value("value", 0.0f);
                    p.noteOrCC = pj.value("n",     0);
                    l.points.push_back(p);
                }
            }
            if (l.id >= m_nextLaneId) m_nextLaneId = l.id + 1;
            if (l.layerId != 0) m_lanes.push_back(l);
        }
    }
    m_nextLaneId = j.value("nextLaneId", m_nextLaneId);

    if (j.contains("sections") && j["sections"].is_array()) {
        for (const auto& sj : j["sections"]) {
            TimelineSection s;
            s.id        = sj.value("id",        m_nextSectionId++);
            s.startTime = sj.value("startTime", 0.0);
            s.endTime   = sj.value("endTime",   s.startTime + 1.0);
            s.name      = sj.value("name",      std::string{"Section"});
            s.tint      = sj.value("tint",      (uint32_t)0);
            if (s.id >= m_nextSectionId) m_nextSectionId = s.id + 1;
            m_sections.push_back(s);
        }
    }
    m_nextSectionId = j.value("nextSectionId", m_nextSectionId);
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

    // Reset transitionProgress to 1.0 on any layer that isn't currently mid
    // fade-in/out of its own Layer transition. Also clear any previous-frame
    // between-row transition annotations — we re-set them below if a
    // TimelineTransition is currently active for this layer.
    for (int i = 0; i < layers.count(); i++) {
        auto l = layers[i];
        if (!l) continue;
        if (!l->transitionActive && !l->shaderTransitionActive) {
            l->transitionProgress = 1.0f;
        }
        l->betweenRowActive        = false;
        l->betweenRowProgress      = 0.0f;
        l->betweenRowGLName.clear();
        l->betweenRowShaderPath.clear();
    }

    for (auto& track : m_tracks) {
        // Tracks with no clips don't drive visibility — otherwise a layer
        // that just got added (and whose auto-clip hasn't been synced yet)
        // would be force-hidden for a frame. Users can also use the layer
        // stack without the timeline by simply not defining clips.
        if (track.clips.empty()) continue;

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

        auto& rt = runtimeFor(track.layerId);
        uint32_t prevActiveId = rt.activeClipId;
        uint32_t curActiveId  = active ? active->id : 0;

        {
            // Edge: entering a new clip (or starting fresh).
            if (curActiveId != prevActiveId && curActiveId != 0) {
                // Show the layer unless the user manually hid it.
                if (!layer->userHidden) layer->visible = true;
                if (!layer->transitionActive && !layer->shaderTransitionActive) {
                    layer->transitionProgress = 1.0f;
                }
                // If clip has a sourcePath different from current, hot-load + optionally transition.
                if (!active->sourcePath.empty()) {
                    auto src = loadSourceForPath(active->sourcePath);
                    if (src) {
                        // Respect sourceIn — video clips play from the clip's in-point.
#ifdef HAS_FFMPEG
                        if (active->sourceIn > 0.0) {
                            if (auto* vs = dynamic_cast<VideoSource*>(src.get())) {
                                vs->seek(active->sourceIn);
                            }
                        }
#endif
                        // Priority: per-clip ISF shader > per-clip gl-transition >
                        // legacy ISF shader transition > instant. Only transitions
                        // from an existing previous clip; first-clip entry is still
                        // instant.
                        if (layer->source && !active->transitionInShaderPath.empty() && prevActiveId != 0) {
                            // Route the per-clip ISF shader through the Layer's
                            // shader-transition slot so CompositeEngine picks it
                            // up via layer->transitionShaderPath + shaderTransitionActive.
                            layer->transitionShaderPath = active->transitionInShaderPath;
                            layer->transitionShaderInst.reset();
                            layer->transitionType = TransitionType::Shader;
                            layer->transitionDuration = (float)active->transitionInDuration;
                            layer->startShaderTransition(src);
                        } else if (layer->source && !active->transitionInName.empty() && prevActiveId != 0) {
                            layer->startGLTransition(src, active->transitionInName,
                                                     (float)active->transitionInDuration);
                        } else if (layer->source && layer->transitionType == TransitionType::Shader
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

        // Per-clip playback-mode runtime: Loop wraps the video source back to
        // sourceIn when it passes sourceIn + clip.duration; Hold pauses the
        // source once it reaches that point. Only meaningful for video
        // sources; shader sources ignore.
#ifdef HAS_FFMPEG
        if (active && layer->source) {
            if (auto* vs = dynamic_cast<VideoSource*>(layer->source.get())) {
                double clipElapsed = t - active->startTime;
                double srcPos      = active->sourceIn + clipElapsed;
                double srcEnd      = (active->sourceOut > 0.0)
                                     ? active->sourceOut
                                     : active->sourceIn + active->duration;
                if (active->playbackMode == ClipPlaybackMode::Loop) {
                    double loopLen = std::max(0.1, srcEnd - active->sourceIn);
                    if (loopLen > 0.0) {
                        double want = active->sourceIn
                                      + std::fmod(clipElapsed, loopLen);
                        if (std::abs(vs->currentTime() - want) > 1.0) {
                            vs->seek(want);
                        }
                    }
                    if (!vs->isPlaying()) vs->play();
                } else if (active->playbackMode == ClipPlaybackMode::Hold) {
                    if (srcPos >= srcEnd && vs->isPlaying()) {
                        vs->pause();
                    }
                }
                (void)srcPos;
            }
        }
#endif

        // Per-frame visibility reconciliation. Without this, a seek clears
        // m_runtime (so prevActiveId resets to 0) and neither edge fires —
        // leaving the layer stuck in whatever visible state it had before.
        // With this, visibility always tracks "is the playhead inside a clip
        // on this track?" regardless of how the playhead got here.
        if (curActiveId != 0) {
            if (!layer->userHidden) layer->visible = true;
            // If a fade-out was in flight from a prior edge, cancel it —
            // the user has re-entered the clip's window (via drag or seek).
            if (layer->transitionActive && !layer->transitionDirection) {
                layer->transitionActive   = false;
                layer->transitionProgress = 1.0f;
            }
        } else if (!layer->transitionActive && !layer->shaderTransitionActive) {
            // No clip under the playhead and nothing fading — hide the layer.
            layer->visible = false;
        }

        rt.activeClipId = curActiveId;
    }

    // Cross-layer transitions: annotate the "to" layer with the shader blend
    // parameters so CompositeEngine can run the gl-transition (or custom ISF)
    // shader during composite. This is a real shader-driven crossfade — not
    // just an opacity ramp — which is why the from/to layers both render
    // fully and the shader blends the result.
    for (const auto& tr : m_transitions) {
        if (t < tr.startTime || t >= tr.endTime()) continue;
        double p = (t - tr.startTime) / std::max(tr.duration, 1e-3);
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        for (int i = 0; i < layers.count(); i++) {
            auto l = layers[i];
            if (!l) continue;
            if (l->id == tr.toLayerId) {
                l->betweenRowActive       = true;
                l->betweenRowProgress     = (float)p;
                l->betweenRowGLName       = tr.name;
                l->betweenRowShaderPath   = tr.shaderPath;
            }
        }
    }
}
