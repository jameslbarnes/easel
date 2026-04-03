#pragma once
#include <string>

enum class EffectType {
    Blur = 0,
    ColorAdjust,   // brightness, contrast, saturation, hue
    Invert,
    Pixelate,
    Feedback,      // trails / echo
    COUNT
};

inline const char* effectTypeName(EffectType t) {
    switch (t) {
        case EffectType::Blur: return "Blur";
        case EffectType::ColorAdjust: return "Color";
        case EffectType::Invert: return "Invert";
        case EffectType::Pixelate: return "Pixelate";
        case EffectType::Feedback: return "Feedback";
        default: return "Unknown";
    }
}

struct LayerEffect {
    EffectType type = EffectType::Blur;
    bool enabled = true;

    // Blur
    float blurRadius = 4.0f;    // 0-20

    // Color adjust
    float brightness = 0.0f;    // -1 to 1
    float contrast = 0.0f;      // -1 to 1
    float saturation = 0.0f;    // -1 to 1
    float hueShift = 0.0f;      // 0-360

    // Pixelate
    float pixelSize = 8.0f;     // 1-64

    // Feedback
    float feedbackMix = 0.8f;   // 0-1
    float feedbackZoom = 1.01f;  // 0.95-1.1
};
