#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

class LayerStack;

// A clip = "this layer is active from startTime to startTime+duration".
// Overlapping clips on the same track trigger a shader transition (if the
// layer has TransitionType::Shader + transitionShaderPath set).
struct TimelineClip {
    uint32_t id = 0;
    double   startTime = 0.0;        // seconds
    double   duration  = 5.0;        // seconds
    std::string name;                 // display name (optional)
    std::string sourcePath;           // optional file path — when entering the clip,
                                      // load this into the layer's source
                                      // (empty = keep whatever source is already on the layer)

    double endTime() const { return startTime + duration; }
    bool contains(double t) const { return t >= startTime && t < endTime(); }
};

// One track per layer. Each track holds clips ordered by startTime.
struct TimelineTrack {
    uint32_t layerId = 0;     // which Layer::id this track drives
    std::string name;          // cached layer name for display
    std::vector<TimelineClip> clips;
    bool muted = false;
    bool solo  = false;
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

    // JSON serialization (keys used by .easel project file).
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
    // Wipe tracks, clips, runtime — used on "New Project" and before fromJson.
    void clear();

private:
    std::vector<TimelineTrack> m_tracks;
    double m_playhead = 0.0;
    double m_duration = 300.0; // 5 minutes default
    bool   m_playing  = false;
    bool   m_loop     = false;
    uint32_t m_nextClipId = 1;

    // Tracks previous-frame active-clip per track so we only fire transitions
    // on EDGES (clip-enter / clip-exit), not every frame.
    struct TrackRuntime {
        uint32_t layerId = 0;
        uint32_t activeClipId = 0;
    };
    std::vector<TrackRuntime> m_runtime;
    TrackRuntime& runtimeFor(uint32_t layerId);
};
