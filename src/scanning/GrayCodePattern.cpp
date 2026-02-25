#ifdef HAS_OPENCV
#include "scanning/GrayCodePattern.h"
#include <cmath>
#include <algorithm>

uint32_t GrayCodePattern::fromGrayCode(uint32_t gray) {
    uint32_t binary = gray;
    for (uint32_t mask = gray >> 1; mask != 0; mask >>= 1) {
        binary ^= mask;
    }
    return binary;
}

void GrayCodePattern::init(int projW, int projH) {
    m_projW = projW;
    m_projH = projH;

    m_colBits = (int)std::ceil(std::log2((double)projW));
    m_rowBits = (int)std::ceil(std::log2((double)projH));

    // white + black + (colBits * 2 for normal+inverted) + (rowBits * 2)
    m_totalPatterns = 2 + m_colBits * 2 + m_rowBits * 2;

    m_currentIndex = -1;
    m_pixels.resize(m_projW * m_projH * 4);
}

GLuint GrayCodePattern::patternTexture(int index) {
    if (index < 0 || index >= m_totalPatterns) return 0;

    // Only regenerate if the requested pattern changed
    if (index != m_currentIndex) {
        generatePattern(index);
        m_currentIndex = index;
    }
    return m_texture.id();
}

void GrayCodePattern::generatePattern(int index) {
    if (index == 0) {
        // White reference
        std::fill(m_pixels.begin(), m_pixels.end(), 255);
    } else if (index == 1) {
        // Black reference
        for (int i = 0; i < m_projW * m_projH; i++) {
            m_pixels[i * 4 + 0] = 0;
            m_pixels[i * 4 + 1] = 0;
            m_pixels[i * 4 + 2] = 0;
            m_pixels[i * 4 + 3] = 255;
        }
    } else {
        int patIdx = index - 2;
        bool isColumn = patIdx < m_colBits * 2;

        int bitIndex;
        bool inverted;

        if (isColumn) {
            bitIndex = patIdx / 2;
            inverted = (patIdx % 2) == 1;
        } else {
            int rowPatIdx = patIdx - m_colBits * 2;
            bitIndex = rowPatIdx / 2;
            inverted = (rowPatIdx % 2) == 1;
        }

        // Bit position: most significant bit first for better spatial resolution
        int msBit = isColumn ? (m_colBits - 1 - bitIndex) : (m_rowBits - 1 - bitIndex);

        for (int y = 0; y < m_projH; y++) {
            for (int x = 0; x < m_projW; x++) {
                uint32_t coord = isColumn ? (uint32_t)x : (uint32_t)y;
                uint32_t gray = toGrayCode(coord);
                bool bitOn = ((gray >> msBit) & 1) != 0;

                if (inverted) bitOn = !bitOn;

                uint8_t val = bitOn ? 255 : 0;
                int idx = (y * m_projW + x) * 4;
                m_pixels[idx + 0] = val;
                m_pixels[idx + 1] = val;
                m_pixels[idx + 2] = val;
                m_pixels[idx + 3] = 255;
            }
        }
    }

    if (m_texture.id() == 0) {
        m_texture.createEmpty(m_projW, m_projH);
    }
    m_texture.updateData(m_pixels.data(), m_projW, m_projH, GL_RGBA, GL_UNSIGNED_BYTE);
}

#endif // HAS_OPENCV
