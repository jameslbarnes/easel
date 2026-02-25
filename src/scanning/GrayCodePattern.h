#pragma once
#ifdef HAS_OPENCV

#include "render/Texture.h"
#include <vector>
#include <cstdint>

class GrayCodePattern {
public:
    GrayCodePattern() = default;

    void init(int projW, int projH);

    int patternCount() const { return m_totalPatterns; }
    int columnPatternCount() const { return m_colBits; }
    int rowPatternCount() const { return m_rowBits; }
    int projectorWidth() const { return m_projW; }
    int projectorHeight() const { return m_projH; }

    // Returns texture for pattern at index.
    // Uses a single reusable texture — only valid until the next call.
    // Layout: [white, black, col0, col0_inv, col1, col1_inv, ..., row0, row0_inv, ...]
    GLuint patternTexture(int index);

    // Gray code helpers
    static uint32_t toGrayCode(uint32_t n) { return n ^ (n >> 1); }
    static uint32_t fromGrayCode(uint32_t gray);

private:
    int m_projW = 0, m_projH = 0;
    int m_colBits = 0, m_rowBits = 0;
    int m_totalPatterns = 0;

    // Single reusable texture + pixel buffer (avoids 46x texture allocation)
    Texture m_texture;
    std::vector<uint8_t> m_pixels;
    int m_currentIndex = -1;

    void generatePattern(int index);
};

#endif // HAS_OPENCV
