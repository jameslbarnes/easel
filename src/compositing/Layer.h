#pragma once
#include "compositing/BlendMode.h"
#include "compositing/LayerEffect.h"
#include "compositing/MaskPath.h"
#include "sources/ContentSource.h"
#include "render/Texture.h"
#ifdef HAS_NDI
#include "app/NDIOutput.h"
#endif
#include <glm/glm.hpp>
#include <string>
#include <memory>

enum class TransitionType { Fade = 0, WipeLeft, WipeRight, WipeUp, WipeDown, Dissolve, Shader, COUNT };

inline const char* transitionTypeName(TransitionType t) {
    switch (t) {
        case TransitionType::Fade: return "Fade";
        case TransitionType::WipeLeft: return "Wipe Left";
        case TransitionType::WipeRight: return "Wipe Right";
        case TransitionType::WipeUp: return "Wipe Up";
        case TransitionType::WipeDown: return "Wipe Down";
        case TransitionType::Dissolve: return "Dissolve";
        case TransitionType::Shader: return "Shader";
        default: return "Fade";
    }
}

enum class MosaicMode { Mirror = 0, Hex, COUNT };

inline const char* mosaicModeName(MosaicMode mode) {
    switch (mode) {
        case MosaicMode::Mirror: return "Mirror";
        case MosaicMode::Hex:    return "Hex";
        default: return "Mirror";
    }
}

class Layer {
public:
    uint32_t id = 0; // stable ID for zone visibility sets
    std::string name = "Layer";
    bool visible = true;
    // User-forced hide from the Layer panel eye toggle. Survives Timeline's
    // per-frame visibility reconciliation (which force-sets visible=true while
    // a clip is under the playhead). When true, the compositor treats the
    // layer as hidden regardless of `visible`.
    bool userHidden = false;

    // Mute / Solo — independent of the eye toggle. Mute hides the layer from
    // the composite. Solo, when set on any layer, hides every non-solo'd
    // layer (the compositor inspects the whole stack to decide).
    bool muted  = false;
    bool soloed = false;
    uint32_t groupId = 0; // 0 = ungrouped
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;

    // 2D transform
    glm::vec2 position = {0.0f, 0.0f};
    glm::vec2 scale = {1.0f, 1.0f};
    float rotation = 0.0f; // degrees
    glm::vec2 anchor = {0.0f, 0.0f}; // pivot point in NDC (-1 to 1), 0,0 = center
    bool flipH = false;
    bool flipV = false;

    // Mosaic mode
    MosaicMode mosaicMode = MosaicMode::Mirror;
    float tileX = 1.0f, tileY = 1.0f;     // Mirror grid (float for audio smoothing)
    float mosaicDensity = 4.0f;            // Hex cell count
    float mosaicSpin = 0.0f;               // Hex rotation
    bool audioReactive = false;
    float audioStrength = 0.15f;

    // Mosaic transition (ephemeral — not serialized)
    MosaicMode mosaicModeFrom = MosaicMode::Mirror;
    float mosaicTransitionStart = -10.0f;
    float mosaicTransitionDuration = 1.5f;

    // Transitions
    TransitionType transitionType = TransitionType::Fade;
    float transitionDuration = 0.5f;  // seconds (0 = instant)
    float transitionProgress = 1.0f;  // 0 = fully out, 1 = fully in
    bool transitionActive = false;
    bool transitionDirection = true;  // true = fading in, false = fading out

    // Shader-based A→B transition (when transitionType == Shader)
    // Queue a next source + trigger: transition shader blends source (A) with nextSource (B)
    // over transitionDuration, then source := nextSource and nextSource is cleared.
    std::string transitionShaderPath; // path to ISF transition shader
    std::shared_ptr<ContentSource> nextSource; // queued "B" source for shader transition
    std::shared_ptr<class ShaderSource> transitionShaderInst; // lazy-loaded shader instance
    bool shaderTransitionActive = false; // distinct from transitionActive (opacity fade)

    // gl-transitions.com transition (parallel to shaderTransitionActive). Uses
    // GLTransitionLibrary — name is the bundled .glsl filename stem.
    std::string glTransitionName;       // e.g. "doorway", "LinearBlur", "fade"
    bool        glTransitionActive = false;

    // Cross-layer (between-row) transition state. Set per frame by
    // Timeline::applyToLayers when a TimelineTransition's window contains the
    // playhead and this layer is the "to" target. The CompositeEngine reads
    // these fields and blends the accumulated stack-so-far with this layer's
    // solo render via the named gl-transition (or shaderPath if set).
    bool        betweenRowActive        = false;
    float       betweenRowProgress      = 0.0f;
    std::string betweenRowGLName;       // gl-transition name; used when shaderPath empty
    std::string betweenRowShaderPath;   // optional ISF shader path

    // Edge feather (0.0 = hard edge, 0.5 = max soft blend)
    float feather = 0.0f;

    // Drop shadow
    bool dropShadowEnabled = false;
    float dropShadowOffsetX = 0.05f;   // NDC offset (positive = right)
    float dropShadowOffsetY = 0.05f;   // NDC offset (positive = down)
    float dropShadowBlur = 8.0f;       // blur radius in pixels
    float dropShadowOpacity = 0.7f;
    float dropShadowColorR = 0.0f;
    float dropShadowColorG = 0.0f;
    float dropShadowColorB = 0.0f;
    float dropShadowSpread = 1.0f;     // alpha multiplier (>1 thickens shadow)

    // Source crop (0.0–0.49): trims edges before tiling
    float cropTop = 0.0f, cropBottom = 0.0f;
    float cropLeft = 0.0f, cropRight = 0.0f;
    bool autoCrop = false;    // auto-detect and remove black borders
    bool autoCropDone = false; // already ran for current source

    // Per-layer masks (applied before blending into composite)
    struct LayerMask {
        std::string name = "Mask";
        MaskPath path;
        std::shared_ptr<Texture> texture; // rendered by MaskRenderer
        float feather = 0.0f;   // 0-1 UV units, softens mask edges
        bool invert = false;    // swap inside/outside
    };
    std::vector<LayerMask> masks;
    int activeMaskIndex = -1; // which mask is being edited (-1 = none)

    // Per-layer effects chain
    std::vector<LayerEffect> effects;

    // Audio-reactive property bindings
    // Each binding modulates a property by an audio signal
    enum class AudioTarget { None=0, Opacity, PositionX, PositionY, Scale, Rotation, COUNT };
    struct AudioBinding {
        AudioTarget target = AudioTarget::None;
        int signal = 0;       // 0=bass, 1=mid, 2=high, 3=beat
        float strength = 0.5f; // how much to modulate
    };
    std::vector<AudioBinding> audioBindings;

    // Shader resolution override (0 = use canvas size)
    int shaderWidth = 0;
    int shaderHeight = 0;

    // Content source
    std::shared_ptr<ContentSource> source;


    // Kick off a shader-based A→B transition: blend current source with nextSrc
    // using the ISF shader at transitionShaderPath over transitionDuration.
    void startShaderTransition(std::shared_ptr<ContentSource> nextSrc) {
        if (!nextSrc || transitionShaderPath.empty()) return;
        nextSource = std::move(nextSrc);
        transitionProgress = 0.0f;
        shaderTransitionActive = true;
    }

    // Kick off a gl-transitions.com A→B transition. Runs over `duration`
    // seconds; compositor routes the layer's render through the named
    // transition in GLTransitionLibrary. If the transition isn't found, the
    // compositor silently falls back to instant swap.
    void startGLTransition(std::shared_ptr<ContentSource> nextSrc,
                           const std::string& name, float duration = 0.5f) {
        if (!nextSrc || name.empty()) return;
        nextSource = std::move(nextSrc);
        glTransitionName = name;
        transitionDuration = duration > 0.01f ? duration : 0.01f;
        transitionProgress = 0.0f;
        glTransitionActive = true;
    }

    // Toggle visibility with transition
    void toggleVisibility() {
        if (transitionDuration <= 0.0f) {
            visible = !visible;
            transitionProgress = visible ? 1.0f : 0.0f;
            return;
        }
        if (visible) {
            // Start fading out
            transitionDirection = false;
            transitionActive = true;
            // When fully faded, mark invisible
        } else {
            // Start fading in
            visible = true;
            transitionDirection = true;
            transitionActive = true;
            transitionProgress = 0.0f;
        }
    }

    glm::mat3 getTransformMatrix() const;

    GLuint textureId() const { return source ? source->textureId() : 0; }
    int width() const { return source ? source->width() : 0; }
    int height() const { return source ? source->height() : 0; }

#ifdef HAS_NDI
    NDIOutput ndiSender;
    std::string ndiName;  // current NDI sender name (for rename detection)
    bool ndiEnabled = false; // per-layer NDI broadcast (off by default for performance)
#endif
};
