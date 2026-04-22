#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

class LayerStack;

// Per-clip type — drives color coding in the UI and is auto-detected from
// sourcePath when left as Auto. Stored in JSON so explicit user overrides
// (e.g. "treat this as Color") survive a reload.
enum class ClipKind : uint8_t { Auto = 0, Video = 1, Shader = 2, Color = 3 };

// A clip = "this layer is active from startTime to startTime+duration".
// Overlapping clips on the same track trigger a shader transition (if the
// layer has TransitionType::Shader + transitionShaderPath set).
struct TimelineClip {
    uint32_t id = 0;
    double   startTime = 0.0;        // seconds on the timeline
    double   duration  = 5.0;        // seconds on the timeline
    double   sourceIn  = 0.0;        // in-point into the source media (seconds)
    double   sourceOut = -1.0;       // out-point (-1 = use source's full length)
    ClipKind kind      = ClipKind::Auto;
    uint32_t tint      = 0;          // optional RGBA override (0 = auto by kind)
    std::string name;                 // display name (optional)
    std::string sourcePath;           // optional file path — when entering the clip,
                                      // load this into the layer's source
                                      // (empty = keep whatever source is already on the layer)

    // Optional gl-transitions.com transition played on clip-enter. Blends the
    // layer's previous source into this clip's source over transitionInDuration
    // seconds using the named transition (must exist in GLTransitionLibrary).
    // Empty name = instant source swap (existing behavior).
    std::string transitionInName;
    double      transitionInDuration = 0.5; // seconds

    double endTime() const { return startTime + duration; }
    bool contains(double t) const { return t >= startTime && t < endTime(); }
};

// One track per layer. Each track holds clips ordered by startTime.
// Visibility is owned by the Layer itself (Layer panel toggle) — the timeline
// drives visibility on clip enter/exit, but there's no separate per-track
// mute/solo here: the mental model is "layers you can't see in the Layer panel
// are just layers the timeline won't show you."
struct TimelineTrack {
    uint32_t layerId = 0;     // which Layer::id this track drives
    std::string name;          // cached layer name for display
    std::vector<TimelineClip> clips;
};

// A transition = "blend between layer A (below) and layer B (above) while
// both are active, over [startTime, startTime+duration]." Lives in its own
// thin lane drawn between the two layer rows in the timeline UI. Dragging
// the body/edges behaves like a clip.
struct TimelineTransition {
    uint32_t id           = 0;
    uint32_t fromLayerId  = 0;   // lower layer (the one being transitioned FROM)
    uint32_t toLayerId    = 0;   // upper layer (the one transitioned TO)
    double   startTime    = 0.0;
    double   duration     = 1.0;
    std::string name;             // gl-transitions name, e.g. "crossfade"

    double endTime() const { return startTime + duration; }
};

class Timeline {
public:
    // Transport
    void play()  { m_playing = true; }
    void pause() { m_playing = false; }
    void stop()  { m_playing = false; m_playhead = 0.0; m_runtime.clear(); }
    void togglePlay() { m_playing = !m_playing; }
    bool isPlaying() const { return m_playing; }

    double playhead() const { return m_playhead; }
    void   seek(double t);
    double duration() const { return m_duration; }
    void   setDuration(double d) { m_duration = (d < 1.0) ? 1.0 : d; }
    bool   looping() const { return m_loop; }
    void   setLooping(bool l) { m_loop = l; }

    // Work area — the subset of the timeline that Export writes to disk.
    // Defaults to full duration (workAreaEnd < 0 is the "unset" sentinel).
    double workAreaStart() const { return m_workAreaStart < 0.0 ? 0.0 : m_workAreaStart; }
    double workAreaEnd()   const {
        double e = (m_workAreaEnd < 0.0) ? m_duration : m_workAreaEnd;
        if (e > m_duration) e = m_duration;
        if (e < workAreaStart() + 0.1) e = workAreaStart() + 0.1;
        return e;
    }
    void setWorkArea(double s, double e) {
        if (s < 0.0) s = 0.0;
        if (e > m_duration) e = m_duration;
        if (e < s + 0.1) e = s + 0.1;
        m_workAreaStart = s;
        m_workAreaEnd = e;
    }
    void resetWorkArea() { m_workAreaStart = 0.0; m_workAreaEnd = -1.0; }
    bool workAreaExplicit() const { return m_workAreaEnd >= 0.0; }

    // Advance playhead (call once per frame). Does nothing when paused.
    void advance(double dt);

    // Drive layer visibility / source swaps based on clips at current playhead.
    // Call once per frame AFTER advance() and BEFORE CompositeEngine::composite().
    void applyToLayers(LayerStack& layers);

    // Track management
    TimelineTrack* findTrack(uint32_t layerId);
    TimelineTrack& ensureTrack(uint32_t layerId, const std::string& name);
    void removeTrackForLayer(uint32_t layerId);
    std::vector<TimelineTrack>& tracks() { return m_tracks; }
    const std::vector<TimelineTrack>& tracks() const { return m_tracks; }

    // Clip management
    TimelineClip* addClip(uint32_t layerId, double start, double dur,
                          const std::string& name = {}, const std::string& sourcePath = {});
    void removeClip(uint32_t layerId, uint32_t clipId);
    TimelineClip* findClip(uint32_t layerId, uint32_t clipId);

    // Sort a track's clips by startTime. Call after edits that may reorder.
    void sortTrack(uint32_t layerId);

    // Transition management — crossfades between two layers over a time range.
    TimelineTransition* addTransition(uint32_t fromLayerId, uint32_t toLayerId,
                                      double start, double dur,
                                      const std::string& name = "crossfade");
    void removeTransition(uint32_t transitionId);
    TimelineTransition* findTransition(uint32_t transitionId);
    std::vector<TimelineTransition>& transitions() { return m_transitions; }
    const std::vector<TimelineTransition>& transitions() const { return m_transitions; }

    // JSON serialization (keys used by .easel project file).
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
    // Wipe tracks, clips, runtime — used on "New Project" and before fromJson.
    void clear();

private:
    std::vector<TimelineTrack> m_tracks;
    std::vector<TimelineTransition> m_transitions;
    double m_playhead = 0.0;
    double m_duration = 60.0;  // 1 minute — short enough to compose a show quickly
    double m_workAreaStart = 0.0;
    double m_workAreaEnd   = -1.0;  // -1 = "unset"; effective end is m_duration
    bool   m_playing  = false;
    bool   m_loop     = false;
    uint32_t m_nextClipId = 1;
    uint32_t m_nextTransitionId = 1;

    // Tracks previous-frame active-clip per track so we only fire transitions
    // on EDGES (clip-enter / clip-exit), not every frame.
    struct TrackRuntime {
        uint32_t layerId = 0;
        uint32_t activeClipId = 0;
    };
    std::vector<TrackRuntime> m_runtime;
    TrackRuntime& runtimeFor(uint32_t layerId);
};
