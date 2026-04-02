#pragma once
#include "compositing/BlendMode.h"
#include "compositing/MaskPath.h"
#include "sources/ContentSource.h"
#include "render/Texture.h"
#ifdef HAS_NDI
#include "app/NDIOutput.h"
#endif
#include <glm/glm.hpp>
#include <string>
#include <memory>

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
    uint32_t groupId = 0; // 0 = ungrouped
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;

    // 2D transform
    glm::vec2 position = {0.0f, 0.0f};
    glm::vec2 scale = {1.0f, 1.0f};
    float rotation = 0.0f; // degrees
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

    // Edge feather (0.0 = hard edge, 0.5 = max soft blend)
    float feather = 0.0f;

    // Source crop (0.0–0.49): trims edges before tiling
    float cropTop = 0.0f, cropBottom = 0.0f;
    float cropLeft = 0.0f, cropRight = 0.0f;
    bool autoCrop = true;     // auto-detect and remove black borders
    bool autoCropDone = false; // already ran for current source

    // Content source
    std::shared_ptr<ContentSource> source;

    // Optional mask (rendered from maskPath)
    std::shared_ptr<Texture> mask;
    MaskPath maskPath;
    bool maskEnabled = false;

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
