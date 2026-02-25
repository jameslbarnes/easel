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

class Layer {
public:
    std::string name = "Layer";
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;

    // 2D transform
    glm::vec2 position = {0.0f, 0.0f};
    glm::vec2 scale = {1.0f, 1.0f};
    float rotation = 0.0f; // degrees
    bool flipH = false;
    bool flipV = false;

    // Tile/mirror: repeats with alternating mirror (1 = normal, 2+ = mirrored tiles)
    int tileX = 1, tileY = 1;

    // Source crop (0.0–0.49): trims edges before tiling
    float cropTop = 0.0f, cropBottom = 0.0f;
    float cropLeft = 0.0f, cropRight = 0.0f;

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
#endif
};
