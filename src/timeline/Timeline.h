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

// Per-clip playback semantics — how the clip's source behaves as the
// playhead moves through it.
enum class ClipPlaybackMode : uint8_t {
    Forward = 0,   // standard
    Loop,          // restart source when reaching clip end (wired)
    Hold,          // play once, hold last frame (wired: pause source at clip end)
    Reverse,       // play in reverse (requires FFmpeg reverse-seek — UI-only for now)
    PingPong       // forward then reverse then forward (UI-only for now)
};

inline const char* clipPlaybackModeName(ClipPlaybackMode m) {
    switch (m) {
        case ClipPlaybackMode::Forward:  return "Forward";
        case ClipPlaybackMode::Loop:     return "Loop";
        case ClipPlaybackMode::Hold:     return "Hold";
        case ClipPlaybackMode::Reverse:  return "Reverse";
        case ClipPlaybackMode::PingPong: return "Ping-Pong";
        default: return "Forward";
    }
}

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

    // Optional ISF shader path. When non-empty it takes precedence over
    // transitionInName and drives the A→B blend on clip-enter through
    // Layer::startShaderTransition + the per-layer transitionShaderPath.
    std::string transitionInShaderPath;

    // How the clip's source plays as the playhead moves through it.
    ClipPlaybackMode playbackMode = ClipPlaybackMode::Forward;

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

// Named cue point on the timeline — one dot in the Markers lane at a time.
// During playback, the first marker whose window contains the playhead
// fires its associated sceneName via SceneManager::recall*. Useful for
// scripted shows: pre-author "drop" at 2:14, single key press during the
// show (or playhead arrival) recalls the full layer-stack snapshot.
struct TimelineMarker {
    uint32_t    id   = 0;
    double      time = 0.0;
    std::string name;
    std::string sceneName;  // blank = informational only, no scene recall
};

// Contiguous section of the timeline — renders as a colored band on the
// ruler. Used for "verse / chorus / drop" labels so performers can jump
// between acts without scrubbing.
struct TimelineSection {
    uint32_t    id        = 0;
    double      startTime = 0.0;
    double      endTime   = 1.0;
    std::string name;
    uint32_t    tint = 0;  // 0 = auto-pick from a palette by id
    double duration() const { return endTime - startTime; }
};

// Additional per-layer sublanes below the main clip row. Data-model stubs —
// these hold keyframes/events but no runtime yet. UI lets the user add lanes,
// shows placeholder tracks, and serialises round-trip.
enum class TimelineLaneKind : uint8_t {
    Automation = 0,   // float keyframes driving a named layer parameter
    MIDI,             // MIDI note/CC events (id + value, quantised)
    AudioReactive,    // audio-signal binding (bass/mid/high/beat → param)
    COUNT
};
inline const char* timelineLaneKindName(TimelineLaneKind k) {
    switch (k) {
        case TimelineLaneKind::Automation:    return "Automation";
        case TimelineLaneKind::MIDI:          return "MIDI";
        case TimelineLaneKind::AudioReactive: return "Audio-Reactive";
        default: return "Lane";
    }
}
struct TimelineLanePoint {
    double time  = 0.0;
    float  value = 0.0f;      // 0..1 for automation / audio-reactive depth
    int    noteOrCC = 0;       // MIDI note number (or CC #)
};
struct TimelineLane {
    uint32_t id      = 0;
    uint32_t layerId = 0;
    TimelineLaneKind kind = TimelineLaneKind::Automation;
    std::string paramName;                 // e.g. "opacity", "position.x", "CC1"
    int         midiChannel  = 0;
    int         audioSignal  = 0;          // 0=bass 1=mid 2=high 3=beat
    float       audioStrength = 0.5f;
    std::vector<TimelineLanePoint> points; // keyframes / events
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
    // Optional — path to an ISF fragment shader with `from`, `to`, `progress`
    // inputs. When set, takes precedence over `name` and lets the user drive
    // the blend with any custom ISF shader (e.g. one of their Soph-Orb style
    // creations) rather than a gl-transitions.com preset.
    std::string shaderPath;

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

    // Markers — named cue points. addMarker returns the new id.
    uint32_t addMarker(double time, const std::string& name = "Cue",
                       const std::string& sceneName = "");
    void     removeMarker(uint32_t id);
    TimelineMarker* findMarker(uint32_t id);
    std::vector<TimelineMarker>& markers()             { return m_markers; }
    const std::vector<TimelineMarker>& markers() const { return m_markers; }

    // Sections — ruler bands spanning a time range.
    uint32_t addSection(double start, double end, const std::string& name = "Section");
    void     removeSection(uint32_t id);
    TimelineSection* findSection(uint32_t id);
    std::vector<TimelineSection>& sections()             { return m_sections; }
    const std::vector<TimelineSection>& sections() const { return m_sections; }

    // Lanes — per-layer automation / MIDI / audio-reactive sublanes.
    uint32_t addLane(uint32_t layerId, TimelineLaneKind kind,
                     const std::string& paramName = "opacity");
    void     removeLane(uint32_t id);
    TimelineLane* findLane(uint32_t id);
    std::vector<TimelineLane>& lanes()             { return m_lanes; }
    const std::vector<TimelineLane>& lanes() const { return m_lanes; }

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
    uint32_t m_nextMarkerId = 1;
    uint32_t m_nextSectionId = 1;
    uint32_t m_nextLaneId = 1;
    std::vector<TimelineMarker>  m_markers;
    std::vector<TimelineSection> m_sections;
    std::vector<TimelineLane>    m_lanes;

    // Tracks previous-frame active-clip per track so we only fire transitions
    // on EDGES (clip-enter / clip-exit), not every frame.
    struct TrackRuntime {
        uint32_t layerId = 0;
        uint32_t activeClipId = 0;
    };
    std::vector<TrackRuntime> m_runtime;
    TrackRuntime& runtimeFor(uint32_t layerId);
};
