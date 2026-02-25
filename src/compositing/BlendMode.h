#pragma once

enum class BlendMode {
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Add,
    Subtract,
    Difference,
    COUNT
};

inline const char* blendModeName(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal:     return "Normal";
        case BlendMode::Multiply:   return "Multiply";
        case BlendMode::Screen:     return "Screen";
        case BlendMode::Overlay:    return "Overlay";
        case BlendMode::Add:        return "Add";
        case BlendMode::Subtract:   return "Subtract";
        case BlendMode::Difference: return "Difference";
        default:                    return "Unknown";
    }
}
