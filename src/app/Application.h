#pragma once
#include "app/OutputZone.h"
#include "app/MappingProfile.h"
#include "app/ProjectorOutput.h"
#include "app/UndoStack.h"
#include "compositing/CompositeEngine.h"
#include "compositing/LayerStack.h"
#include "compositing/MaskRenderer.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include "render/Texture.h"
#include "ui/UIManager.h"
#include "ui/ViewportPanel.h"
#include "ui/LayerPanel.h"
#include "ui/PropertyPanel.h"
#include "ui/WarpEditor.h"
#include "ui/TransitionCatalog.h"
#include "voice/VoiceCommand.h"
#ifdef __APPLE__
#include "voice/MacSpeechRecognizer.h"
#endif
#include <unordered_map>
#include <deque>
#include <future>

#ifdef _WIN32
#include "sources/WindowCaptureSource.h"
#elif defined(__APPLE__)
#include "sources/WindowCaptureSource_mac.h"
#endif
#include "sources/ShaderSource.h"
#include "sources/ShaderClawBridge.h"
#include "sources/ShaderRatings.h"
#include "sources/ShaderPresets.h"
#include "sources/ShaderImprover.h"
#include "sources/ParticleSource.h"
#include "sources/MovingCompanySource.h"
#include "sources/FluidSource.h"
#include "sources/FluidSource3D.h"
#include "sources/HologramModelSource.h"
#ifdef __APPLE__
#include "sources/VisionTracker.h"
#endif

#ifdef HAS_OPENCV
#include "scanning/SceneScanner.h"
#include "scanning/WebcamSource.h"
#include "scanning/ScanPanel.h"
#endif

#ifdef HAS_WHISPER
#include "speech/WhisperSpeech.h"
#endif

#include "speech/EthereaClient.h"
#include "speech/CueClient.h"
#include "app/AudioAnalyzer.h"
#include "app/AudioMixer.h"
#include "app/BPMSync.h"
#include "app/SceneManager.h"
#include "app/OSCManager.h"
#include "timeline/Timeline.h"
#include "app/MIDIManager.h"
#include "app/DataBus.h"
#include "stage/StageView.h"
#include "app/ProDJLink.h"
#include "ui/TimecodePanel.h"

#include "app/NetAdapters.h"

#ifdef HAS_NDI
#include "sources/NDISource.h"
#include "app/NDIOutput.h"
#include "net/NdiNetworkConfig.h"
#endif

#ifdef HAS_SPOUT
#include "app/SpoutIO.h"
#endif

#ifdef HAS_WHEP
#include "sources/WHEPSource.h"
#endif

#ifdef HAS_FFMPEG
#include "app/RTMPOutput.h"
#include "app/VideoRecorder.h"
#endif

#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

class Application {
public:
    bool init();
    void run();
    void shutdown();

private:
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1280, m_windowHeight = 720;

    UIManager m_ui;
    TransitionCatalog m_transitionCatalog;
    std::unordered_map<std::string, float> m_animatedParams; // Phase C lane runtime cache

    // V1 voice-driven timeline. Push-to-talk via UI for now; SFSpeech wrapper
    // arrives in V1.1. handleVoiceIntent() dispatches recognized phrases to
    // the existing timeline / layer / shader APIs so anyone can drive the
    // tool with their voice instead of clicks.
    void handleVoiceIntent(const easel::voice::VoiceIntent& intent);
    void renderVoiceCommandBar();
    void startVoiceRecording();
    void stopVoiceRecording();
    // Per-source rolling transcript state, for the *.recent FIFO + *.words delta.
    struct TranscriptFeed {
        std::string prevSegment;              // baseline for the per-event delta
        std::vector<std::string> finalWords;  // committed words (FIFO history)
        // Caption contract (Phase B): utterance identity + revision tracking.
        std::string utteranceId;              // current utterance (empty for legacy producers)
        int revision = 0;                     // last accepted revision within utteranceId
        std::vector<std::string> committedChunks; // isFinal chunks of the current utterance
        CaptionSegment latest;                // most recent accepted segment for this prefix
    };
    // SCTEXT/1: speaker label ("speaker_N"/producer label) -> small id in
    // first-seen order (cap 254; 255 = unknown in the glyph buffer).
    std::unordered_map<std::string, int> m_speakerIds;
    // Route a live transcript segment to the DataBus channels under `prefix`
    // ("cue" or "etherea"):
    //   <prefix>.latest — full current segment (unchanged contract; the whole
    //                     growing utterance, for typewriter/reveal text shaders).
    //   <prefix>.words  — only the words newly appended since the previous
    //                     segment (per-event delta; mirrors etherea-ai's
    //                     unprocessed-tail concept).
    //   <prefix>.recent — a running FIFO of the last m_recentWordCap words that
    //                     slides as you speak (committed words + current segment,
    //                     windowed). This is the live "ticker" feed.
    //   <prefix>.utterance — utteranceId of the current segment ("" when the
    //                     producer doesn't supply one).
    //   <prefix>.caption — utterance-long text: committed chunks of the current
    //                     utterance + the live tail (utteranceText when the
    //                     producer supplies it). Bind `msg` here for a reveal
    //                     that continues across chunk commits.
    //   <prefix>.speaker — set when the segment carries a speaker.
    // `feed` holds per-source state; `isFinal` commits the current segment.
    // Stale revisions (same utteranceId, lower revision) are dropped.
    void pushTranscript(const std::string& prefix, TranscriptFeed& feed,
                        const CaptionSegment& seg);
    // Thin overload for producers without caption metadata (voice recognizer,
    // OSC /cue/latest): wraps text/isFinal into a CaptionSegment.
    void pushTranscript(const std::string& prefix, TranscriptFeed& feed,
                        const std::string& text, bool isFinal) {
        CaptionSegment seg;
        seg.text = text;
        seg.isFinal = isFinal;
        pushTranscript(prefix, feed, seg);
    }
    // Thin Cue-channel wrapper.
    void pushCueWords(const std::string& text, bool isFinal) {
        pushTranscript("cue", m_cueFeed, text, isFinal);
    }
    char m_voiceTextInput[256] = "";
    std::deque<std::string> m_voiceLog;        // last N transcripts (recent first)
    std::string m_voiceLastEcho;               // formatted intent for the pill
    double m_voiceLastEchoTime = 0.0;
    bool m_voiceBarOpen = true;                // command bar visibility
    std::string m_voicePartial;                // streaming partial transcript while listening
    bool m_voiceListening = false;             // mic gate (push-to-talk)
    bool m_voiceContinuous = true;             // when true, mic stays on; auto-restart after every final
    bool m_voiceRestartPending = false;        // set in onFinal; main loop tears down + restarts
    double m_voiceStartRetryAt = 0.0;          // continuous-mic start backoff (async engine start)
#ifdef __APPLE__
    MacSpeechRecognizer m_voiceRecognizer;
#endif
    ViewportPanel m_viewportPanel;
    LayerPanel m_layerPanel;
    PropertyPanel m_propertyPanel;
    WarpEditor m_warpEditor;

    // Mapping profiles (warp + edge blend, independent of zones)
    std::vector<std::unique_ptr<MappingProfile>> m_mappings;
    MappingProfile* mappingForZone(OutputZone& z);


    // Output zones (replaces singular compositor/warp/FBO)
    std::vector<std::unique_ptr<OutputZone>> m_zones;
    int m_activeZone = 0;
    // Monotonic composite-pass counter — drives idle release of per-zone
    // scratch FBOs (OutputZone::releaseIdleScratch).
    uint64_t m_compositeFrame = 0;
    int m_prevActiveZone = 0;

    // ── Multi-screen output mode ─────────────────────────────────────
    // Independent: each zone drives its own monitor (the existing path).
    // Spanned:     one wide "span" canvas is sliced left-to-right across
    //              several monitors, so a single visual stretches/moves
    //              across screens. The two are alternatives, toggled live.
    enum class OutputMode { Independent, Spanned };
    OutputMode m_outputMode = OutputMode::Independent;

    // The span canvas: its own compositor / warp / layer-visibility at a
    // user-defined resolution (default 3840x1080 = two side-by-side 1080p
    // screens). Lazily created the first time spanned mode is used.
    std::unique_ptr<OutputZone> m_spanZone;
    int m_spanWidth  = 3840;
    int m_spanHeight = 1080;

    // Horizontal slices of the span canvas, ordered left-to-right. Each slice
    // crops the [u0,u1] sub-rect of the (flat) span canvas, warps it through
    // its OWN mapping profile (independent per-projector corner-pin), then
    // presents fullscreen on one monitor.
    struct SpanSlice {
        int monitor = -1;
        float u0 = 0.0f;
        float u1 = 1.0f;
        int mappingIndex = -1;   // own MappingProfile in m_mappings (-1 until created)
    };
    std::vector<SpanSlice> m_spanSlices;
    // Per-slice GPU targets (parallel to m_spanSlices): cropFBO holds the
    // projector's flat half, warpFBO the corner-pinned result. Kept out of
    // SpanSlice so the struct stays copyable.
    std::vector<Framebuffer> m_spanCropFBO;
    std::vector<Framebuffer> m_spanWarpFBO;
    // Per-projector mapping profiles, kept SEPARATE from m_mappings so the
    // normal Mapping workspace (which enumerates m_mappings) never sees or
    // touches them. SpanSlice.mappingIndex indexes into this vector.
    std::vector<std::unique_ptr<MappingProfile>> m_spanMappings;

    OutputZone& ensureSpanZone();   // create + size the span canvas on demand
    void layoutSpanSlices();        // even left->right split across the slices
    void ensureSpanSliceResources(); // size per-slice FBOs + mapping profiles
    // Shared projector create/retry/backoff helper (used by both the
    // independent per-zone path and the spanned slice path). Returns an
    // active projector for the monitor, or nullptr if it isn't ready yet.
    ProjectorOutput* ensureProjector(int monitorIndex);
    // Target frame-rate cap shown/edited in the Canvas section. 0 = vsync
    // (loop runs at the editor display's refresh). >0 disables vsync and
    // busy-waits after swap to hold the frame to ~1/target sec.
    float m_targetFPS = 0.0f;
    // Swap interval currently applied to the main context (-1 = unset). Driven
    // each frame from m_targetFPS so toggling the Canvas Target flips vsync.
    int m_appliedSwapInterval = -1;
    // Last workspace mode observed in renderUI. Used to detect transitions
    // (e.g. user clicks PLAY tab) so we can auto-open the timeline and
    // reset show-specific UI state without polling every frame.
    UIManager::WorkspaceMode m_prevWorkspaceMode = UIManager::WorkspaceMode::Canvas;
    // glfwGetTime() instant until which the PLAY workspace's PUBLISH
    // button shows the "Published" confirmation caption. 0 = no flash.
    double m_publishFlashUntil = 0.0;
    // M3 — live re-publish. Stable per-process show id (so byte-comparison
    // of consecutive publishes detects actual content changes, not just a
    // bumped timestamp). Cached last-sent JSON for the dirty check, and
    // the next time the dirty check should run.
    std::string m_showIdStr;
    std::string m_lastPublishedJson;
    double      m_nextPublishCheckAt = 0.0;
    // M7-11 — After an explicit /cue/clear, suppress any background writes
    // (mic recognizer, cue WS client) to cue.latest for a brief window so
    // an intentional blank doesn't get clobbered by stray room audio.
    // 0 = no active suppression.
    double      m_cueLatestSuppressUntil = 0.0;
    // Per-source rolling transcript state (delta baseline + FIFO history).
    // Touched only from the transcript write path.
    TranscriptFeed m_cueFeed;
    TranscriptFeed m_ethereaFeed;
    // Max words shown in the *.recent rolling feed; user-adjustable (>=1).
    int m_recentWordCap = 7;
    uint32_t m_nextLayerId = 1;
    OutputZone& activeZone() {
        if (m_activeZone < 0 || m_activeZone >= (int)m_zones.size())
            m_activeZone = 0;
        return *m_zones[m_activeZone];
    }

    LayerStack m_layerStack;
    std::unordered_map<int, std::unique_ptr<ProjectorOutput>> m_projectors;
    MaskRenderer m_maskRenderer;
    UndoStack m_undoStack;

    Mesh m_quad;
    ShaderProgram m_passthroughShader;
    ShaderProgram m_edgeBlendShader;
    // Scratch FBOs for the post chain (edge blend, mask union, bloom, warp
    // supersample) live per-zone on OutputZone — sharing them across
    // mixed-resolution zones caused a destroy/realloc cycle every frame.
    Texture m_testPattern;
    Texture m_maskGrid; // white alignment grid shown while a mask is being added/edited

    // MAPPING-workspace calibration patterns (black & white). In Mapping mode
    // the warp source is replaced by one of these so the user aligns geometry
    // to crisp lines instead of live content / the color test pattern. The
    // active one is chosen via the Mapping panel's Test Pattern dropdown
    // (WarpEditor::testPatternIndex). Order MUST match kPatternNames there:
    // 0 Grid, 1 Checkerboard, 2 Crosshair, 3 Concentric Circles, 4 Dots,
    // 5 Solid White.
    static constexpr int kMapPatternCount = 6;
    Texture m_mapPatterns[kMapPatternCount];

    // Phase Q v4 — bloom pipeline. Half-res FBOs ping-pong for the
    // separable Gaussian; uCompositeFBO holds the screen-blended
    // result before it's copied back to warpFBO.
    ShaderProgram m_bloomBrightShader;
    ShaderProgram m_bloomBlurShader;
    ShaderProgram m_bloomCompositeShader;
    ShaderProgram m_linearCopyShader;     // bloom copy-back, no ACES
    ShaderProgram m_warpDownsampleShader; // 4-tap explicit-offset SS → 1× downsample
    // Bloom FBOs are per-zone (OutputZone::bloom*) — see scratch FBO note above.
    // Global post bloom OFF by default — it was glowing every layer/shader and
    // reading as a soft haze. Crisp output is the default; flip on (or raise
    // strength) only when a deliberately glowy finish is wanted.
    bool          m_bloomEnabled   = false;
    float         m_bloomThreshold = 0.85f;
    float         m_bloomKnee      = 0.30f;
    float         m_bloomStrength  = 0.70f;
    float         m_bloomTint      = 0.40f;
    int           m_bloomBlurPasses = 2;  // 1..6 — more = wider, softer halo
    // Global finish (saturation grade + film grain + dither) applied in the
    // bloom composite pass — lifts every layer toward an engine/post-stack look.
    float         m_finishAmount   = 1.0f;

    int m_selectedLayer = -1;
    AudioAnalyzer m_audioAnalyzer;
    AudioMixer m_audioMixer;
    bool m_mixerEnabled = false;        // false = legacy single-device mode
    int m_mixerOutputDevice = -1;       // -1 = default output
    BPMSync m_bpmSync;
    SceneManager m_sceneManager;
    Timeline m_timeline;
    OSCManager m_oscManager;
    MIDIManager m_midiManager;
    // Latest normalized MIDI CC values, indexed [channel][cc]. Updated each frame from polled events.
    float m_midiCCValues[16][128] = {};
    StageView m_stageView;
    ProDJLink m_prodjlink;
    TimecodePanel m_timecodePanel;
#ifdef __APPLE__
    // MediaPipe-style body/hand/face tracking via Apple Vision. Toggled
    // from the Camera tab; feeds the DataBus vision.* keys each frame.
    VisionTracker m_visionTracker;
#endif
    float m_audioRMS = 0; // backward compat: smoothed audio level
    // Global "Audio -> Shaders" switch. When true, the full AudioFeatures bus is
    // fed to every shader's GLSL uniforms; when false, shaders get a neutral
    // (zeroed) bus — the panic off-switch that the old "feed zeros" decision
    // provided. The explicit per-param audio-binding system is independent.
    bool m_audioToShaders = true;
    // Auto-connect the first MIDI controller so it "just works" without opening
    // it in the MIDI panel. Set true once the user explicitly picks "None",
    // so we stop forcing a reconnect.
    bool m_midiUserDisconnected = false;
    int m_mosaicAudioDevice = -1; // -1 = system loopback, >=0 = index into device list
    bool m_projectorAutoConnect = false;
    int m_lastMonitorCount = 0;
    bool m_maskEditMode = false;
    bool m_showTimecodeWindow = false; // Show-mode: Timecode window visible

    // Show-workspace preview zoom. Multiplier applied to the centered
    // live-output preview's height (then width follows aspect). 1.0 = fit
    // (default). Driven by `+`/`-`/Fit toolbar buttons and Cmd+=, Cmd+-,
    // Cmd+0 keyboard shortcuts (handled in renderUI before the Show panel).
    float m_showZoom = 1.0f;

    // App fullscreen toggle (F11) — borderless, editor UI STAYS visible.
    bool m_editorFullscreen = false;
    // Presentation mode (Shift+F11) — draws the active zone's OUTPUT
    // fullscreen with NO editor UI. Kept independent of m_editorFullscreen so
    // "app fullscreen" (UI visible) and "present output" (UI hidden) are
    // separate states. Esc steps out of present first, then out of fullscreen.
    bool m_presentMode = false;
    // Set to true when Esc is pressed in fullscreen — actual exit runs
    // at the TOP of the next frame so AppKit has time to settle the
    // window-style transition without racing the GL/ImGui dock setup.
    bool m_pendingExitFullscreen = false;
    // Async close — blocking cleanup runs on a background thread while
    // the main thread renders a spinner so the window stays responsive.
    std::atomic<bool> m_closing{false};
    std::atomic<bool> m_closingDone{false};
    std::atomic<int>  m_closingStep{0};
    std::thread       m_closingThread;
    int m_savedWindowX = 0, m_savedWindowY = 0;
    int m_savedWindowW = 1280, m_savedWindowH = 720;

    // Splash screen shown immediately on launch
    bool   m_showSplash = true;
    double m_splashStartTime = 0.0;
    void   renderSplash();

    // Landing page shown after splash dismisses — skipped when the default
    // project auto-loaded at startup (live/agent workflow restores the show
    // across restarts; the landing page must not cover it).
    bool m_showLanding = false;
    bool m_autoLoadedProject = false;
    int  m_landingSubView = 0; // 0=main 1=templates 2=recent
    std::vector<std::string> m_recentProjects;
    void renderLandingPage();
    void loadRecentProjectsList();
    void addRecentProject(const std::string& path);

    void updateSources();
    void compositeAndWarp();
    void compositeZone(OutputZone& zone);
    void presentOutputs();
    void renderReadbackFBO(OutputZone& zone);
    void renderReadbackFBO(OutputZone& zone, Framebuffer& target, int width, int height);
    void renderUI();
    void renderMenuBar();
    // Inline brand mark + overflow menu drawn at the start of the workspace
    // nav row. Replaces the standalone main menu bar so we have ONE chrome
    // row instead of two stacked ones.
    void renderNavBarPrefix();
    void toggleEditorFullscreen();
#ifdef HAS_FFMPEG
    void renderGoLiveButton();
#endif

    // Inline (no Begin/End) section renderers — called from inside their host
    // tab so each control lives with the workflow step it belongs to.
    void renderStageInlineSetup(OutputZone& zone);   // Composition + Output, rendered in Stage tab
    void renderEdgeBlendInline(OutputZone& zone);    // Edge blend, rendered in Masks tab
    void addZone();
    void removeZone(int index);
    void duplicateZone(int index);
    void setupMultiGPUProjection(const std::vector<std::string>& ndiSourceNames);
    void loadImage(const std::string& path);
    void loadVideo(const std::string& path);
#if defined(_WIN32) || defined(__APPLE__)
    void addScreenCapture(int monitorIndex);
#endif
    void addParticles();
    void addFluid();
    void addFluid3D();
    void addHologramModel(const std::string& path = ""); // upload 3D model → glitchy hologram; empty path opens the picker
    void addMovingCompany();   // F-117 flying through space (mesh source)
#ifdef HAS_OPENCV
    void addWebcam(int cameraIndex);
#endif
#ifdef _WIN32
    void addWindowCapture(HWND hwnd, const std::string& title);
#elif defined(__APPLE__)
    void addWindowCapture(uint32_t windowID, const std::string& title);
#endif
    void loadShader(const std::string& path);
    void registerLayerWithZones(uint32_t layerId);

#if defined(_WIN32) || defined(__APPLE__)
    std::vector<WindowInfo> m_windowList;
#endif
    ShaderClawBridge m_shaderClaw;
    ShaderRatings    m_shaderRatings;
    ShaderPresets    m_shaderPresets;
    ShaderImprover   m_shaderImprover;

    // Push Further (AI shader improve) — styled top-level panel state.
    bool        m_pushOpen = false;
    std::string m_pushPath, m_pushTitle, m_pushFile;
    char        m_pushInstr[512] = {};
    int         m_pushCombine = 0;   // 0 = none; else index+1 into shaders()
    void        renderPushFurtherPanel();

    // ShaderClaw thumbnail preview (animated on hover)
    std::shared_ptr<ShaderSource> m_scPreview;
    std::string m_scPreviewPath;
    int m_scPreviewFrame = 0;

    // ShaderClaw static thumbnail cache (one per shader)
    struct SCThumbEntry {
        std::shared_ptr<Texture> texture;
        bool ready = false;
    };
    std::unordered_map<std::string, SCThumbEntry> m_scThumbnails;
    std::shared_ptr<ShaderSource> m_scThumbRenderer; // temp shader for generating thumbnails
    std::string m_scThumbRenderPath; // currently rendering thumbnail for this shader
    int m_scThumbRenderFrame = 0;

#ifdef HAS_OPENCV
    SceneScanner m_scanner;
    WebcamSource m_webcam;
    ScanPanel m_scanPanel;
#endif

    SpeechState m_speechState;
    EthereaClient m_ethereaClient;
    CueClient m_cueClient;
    DataBus m_dataBus;
    std::string m_prevTranscript;       // Last full_transcript for diffing

    // Voice decay: fade text layers after speech stops (matches Shader-Claw 3)
    double m_voiceLastInputTime = 0.0;  // glfwGetTime() of last transcript update
    float m_voiceDecayHold = 2.0f;      // seconds at full opacity after last input
    float m_voiceDecayDuration = 3.0f;  // seconds to fade out after hold
    bool m_voiceDecayEnabled = true;
#ifdef HAS_WHISPER
    WhisperSpeech m_whisperSpeech;
#endif

    // Remove all agent-managed layers (those with a non-empty Layer::managedKey).
    // Backs /easel/layer/clear-managed; not NDI-specific so it links in builds
    // compiled without HAS_NDI.
    void clearManagedLayers();
    // Idempotent managed shader overlay layer keyed by slot (composited above a
    // base source). Backs /easel/layer/ensure/shader.
    void ensureManagedShaderLayer(const std::string& slot, const std::string& shaderPath);
    // Idempotent managed Fluid Simulation layer keyed by slot. The built-in fluid
    // generator (addFluid) as an agent-managed, zone-assignable layer. Backs
    // /easel/layer/ensure/fluid.
    void ensureManagedFluidLayer(const std::string& slot, bool threeD = false);
    // Remove one managed layer by its exact key (drops an overlay/base layer).
    // Backs /easel/layer/remove-managed.
    void removeManagedLayer(const std::string& slot);
    // Set an ISF param on a managed shader layer (by key). Backs /easel/layer/param.
    void setManagedLayerParam(const std::string& key, const std::string& name, const OSCMessage& msg);
    // Drive the master audio-reactivity recipe on a managed layer (by key) —
    // the remote face of the PropertyPanel Reactivity/Character/Shuffle/Off
    // row, via AudioPresetEngine. Backs /easel/layer/audiopreset.
    void setManagedLayerAudioPreset(const std::string& key, const std::string& command, const OSCMessage& msg);
    // Bind one param of a managed shader layer to an audio signal (or "off").
    // Backs /easel/layer/audiobind — the remote face of the per-slider bolt.
    void setManagedLayerAudioBind(const std::string& key, const std::string& param,
                                  const std::string& signalName, const OSCMessage& msg);
    // Bind another layer as a shader image input's texture source — the
    // local face of the PropertyPanel TEXTURE dropdown. (The OSC verb rides
    // upstream's bindManagedLayerImage below.)
    void setManagedLayerBindImage(const std::string& key, const std::string& input, const std::string& sourceName);
    void bindManagedLayerImage(const std::string& key, const std::string& name, const std::string& sourceRef);
    // Agent composite/bus: find-or-create an Easel output zone, publish it as a
    // named NDI feed, and assign managed layers to it. Backs /easel/zone/ensure
    // and /easel/zone/layer.
    OutputZone* ensureZoneByName(const std::string& name);
    void ensureZoneNdi(const std::string& zoneName, const std::string& feedName);
    // Set a zone's physical output at runtime: None (preview) / Fullscreen+monitor /
    // NDI[+feed]. Backs /easel/zone/output; the render loop applies it next frame.
    void setZoneOutput(const std::string& zoneName, const std::string& dest,
                       int monitor, const std::string& ndiFeedName = std::string());
    void addZoneLayerByKey(const std::string& zoneName, const std::string& managedKey);
    // Tear down a composite zone (stop its NDI feed). Backs /easel/zone/remove.
    void removeZoneByName(const std::string& zoneName);

#ifdef HAS_NDI
    NDIOutput m_ndiOutput;
    Framebuffer m_ndiFluxInputFBO;
    NDIFinder m_ndiFinder;
    std::vector<NDISenderInfo> m_ndiSources;
    bool m_ndiOutputEnabled = true;
    // Wire-rate cap applied to ALL NDI senders (global + per-zone) each
    // frame; <= 0 = uncapped. Live-settable via OSC /easel/ndi/fps.
    float m_ndiTargetFps = 60.0f;
    void addNDISource(const std::string& senderName, const std::string& senderUrl = "");
    // Idempotent agent-driven NDI layer keyed by a stable "slot" (managedKey).
    // Re-ensuring the same slot reuses the layer and only reconnects when the
    // sender name changes, so /easel/layer/ensure/ndi never stacks duplicates.
    void ensureManagedNDILayer(const std::string& slot, const std::string& senderName);

    // Wi-Fi / Ethernet NIC pin + cross-device reachability (see NdiNetworkConfig).
    NdiNetworkSettings m_ndiNetwork;             // persisted machine-wide selection
    std::vector<NetAdapterInfo> m_netAdapters;   // cached; refreshed on panel open / Refresh
    std::vector<NdiPeerStatus> m_ndiPeerStatus;  // cached peer reachability
    double m_ndiPeerStatusLastRefresh = 0.0;
    bool m_ndiServerUp = false;                  // cached discovery-server :5959 probe
    double m_ndiServerUpLastRefresh = 0.0;
    void applyNdiNetworkSettings(bool reinit);   // write config (+ optional NDI re-init)
    void refreshNdiPeerStatus();                 // re-enumerate adapters + classify peers
#endif

#ifdef HAS_SPOUT
    EaselSpoutSender m_spoutOutput;
    bool m_spoutOutputEnabled = false;
#endif

#ifdef HAS_WHEP
    char m_whepUrlBuf[512] = {};
    std::shared_ptr<WHEPSource> m_whepConnecting; // tracks in-progress connection
    std::string m_whepStatus;
    void addWHEPSource(const std::string& whepUrl);
    void addScopeRTMP();
#endif

    int m_selectedAudioDevice = -1; // -1 = default loopback

    // Top-edge screen pos of the transport-pill mic button, captured each
    // frame so ##fp_sound_popup can anchor itself ABOVE the pill (the pill
    // is flush to the viewport bottom; a downward popup would be clipped).
    float m_fpSoundBtnTopX = 0.0f;
    float m_fpSoundBtnTopY = 0.0f;

#ifdef HAS_FFMPEG
    RTMPOutput m_rtmpOutput;
    char m_streamKeyBuf[128] = {};
    int m_streamAspect = 0; // 0=16:9, 1=4:3, 2=16:10, 3=Source
    VideoRecorder m_recorder;
    std::vector<RecAudioDevice> m_audioDevices;
    std::vector<RecAudioDevice> m_outputDevices; // render devices for mixer output
    void renderTransportBar();
    void renderTimelinePanel();
    // Phase 5 — floating transport pill at viewport bottom-center. Draws
    // play/stop/loop + timecode in a single rounded pill that overlays the
    // canvas, matching reference B's minimal control surface. Independent
    // of the docked timeline panel which still hosts tracks/audio lane.
    void renderFloatingTransportPill();
    // Floating REC + LIVE pills at viewport bottom-right — two separate
    // rounded surfaces matching the reference's split record/broadcast
    // affordances. Independent of the docked timeline.
    void renderFloatingActionPills();

    // Recording is indefinite/live: the REC button records the active zone's
    // output continuously until the user stops it (no Work-Area auto-stop).
    bool   m_timelineExporting = false;   // retained (always false) for legacy UI checks
    double m_timelineExportEnd = 0.0;
    std::string m_timelineExportPath;
    // overridePath: empty = timestamped recordings/ default (UI button path);
    // non-empty = explicit output file (the /easel/record/start OSC trigger).
    // zoneIndex: -1 = follow the active zone (REC button behavior); >=0 = pin
    // the recording to that zone regardless of later editor tab clicks.
    void   startRecording(const std::string& overridePath = "", int zoneIndex = -1);

    // Session auto-record (EASEL_AUTO_RECORD, default on): pinned to the Main
    // zone, started a few frames into the run so zone FBOs exist.
    bool   m_autoRecordPending = false;
    int    m_recordZoneIndex = -1;

    // Timeline panel visibility. Toggled via the T key or the transport-bar
    // show/hide button. Defaults to visible so the panel is discoverable.
    bool   m_timelineOpen = true;

    // When true, the timeline renders only its transport row and hides the
    // ruler / tracks / audio lane / clip inspector. The panel itself remains
    // docked — drag the dock splitter to shrink to just the transport.
    // Default: minimized. The transport row is enough for a quick show of
    // hands during setup, and the tracks pop up on demand (click the "+" on
    // the timeline tab or press T). Keeps the canvas area maximized on launch.
    bool   m_timelineMinimized = true;

    // ── Single source of truth for the animated timeline geometry ──────────
    // The timeline + bottom transport bar slide up/down as one unit. These
    // are recomputed once per frame in updateTimelineAnim() (called at the
    // top of renderUI, before any panel renders) so Fixes 1/2/3 all read the
    // SAME numbers in the same frame:
    //   m_timelineAnimT  : eased open factor [0..1] (0 = hidden, 1 = open)
    //   m_timelineCurH   : current animated timeline height in px (0 when
    //                      minimized). Bottom nav slides up by this much.
    //   m_timelineTopY   : screen-space Y of the timeline's top edge — the
    //                      hard bottom limit for the params panel + the
    //                      left-rail layer thumbnails.
    float  m_timelineAnimT = 0.0f;
    float  m_timelineCurH  = 0.0f;
    float  m_timelineTopY  = 0.0f;
    // Target (fully-open) timeline content height, measured by
    // renderTimelinePanel() and consumed next frame by updateTimelineAnim().
    float  m_timelineTargetH = 220.0f;
    void   updateTimelineAnim();

    // Audio level meter (WASAPI IAudioMeterInformation)
    void* m_audioMeterInfo = nullptr;
    void* m_audioMeterDevice = nullptr;
    int m_meterDeviceIdx = -2; // which device the meter is tracking (-2 = uninitialized)
    float m_audioLevelPeak = 0.0f;
    float m_audioLevelL = 0.0f, m_audioLevelR = 0.0f;
    float m_audioLevelSmooth = 0.0f, m_audioLevelSmoothL = 0.0f, m_audioLevelSmoothR = 0.0f;
    void updateAudioMeter();
    void cleanupAudioMeter();

    // Old mosaic meter removed — replaced by AudioAnalyzer
#endif

    // File drop handling
    std::vector<std::string> m_pendingDrops;
    void handleDroppedFiles();
    static void dropCallback(GLFWwindow* window, int count, const char** paths);

    // JSON save/load
    void saveProject(const std::string& path);
    void loadProject(const std::string& path);

    // Publish the current Show (layers + timeline + markers + BPM) to the
    // etherea-agent over OSC at /agent/play/publish. The agent caches it and
    // includes it in every state.snapshot, so paired iPhones mirror the
    // timeline. Triggered by the File > Publish to Mobile menu item and by
    // an inbound /easel/play/publish OSC message.
    void publishPlayToAgent();

    // M3 — Internal helpers for the live re-publish loop. buildPlayJson()
    // produces the wire payload as a string (no side effects); the per-frame
    // dirty check sends it only when it differs from the last send.
    std::string buildPlayJson();
    void        publishPlayIfChanged();

    // Screenshot. The flip + PNG encode runs on a worker thread (one job at
    // a time) so an externally-triggered capture doesn't freeze the frame.
    void captureScreenshot(const std::string& path);
    void captureWindow(const std::string& path);
    void writeScreenshotAsync(const std::string& path,
                              std::vector<uint8_t> pixels, int w, int h);
    void pollScreenshotTrigger();
    std::future<void> m_screenshotJob;
};
