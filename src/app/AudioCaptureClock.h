#pragma once
#include <cstdint>

// Device sample positions preserve waveform continuity. Wall-clock timestamps
// establish the initial offset only: rounding/jitter in per-packet QPC readings
// must not insert silence or delete samples from otherwise contiguous audio.
class AudioCaptureClock {
public:
    void reset() { m_initialized = false; }

    int64_t sampleAt(uint64_t position, int64_t wallSample, bool timestampValid) {
        if (!timestampValid) return m_initialized ? m_nextSample : wallSample;
        if (!m_initialized || position < m_lastPosition) {
            m_originPosition = position;
            m_originSample = wallSample;
            m_initialized = true;
        }
        m_lastPosition = position;
        return m_originSample + static_cast<int64_t>(position - m_originPosition);
    }

    void advance(int64_t start, uint32_t frames) { m_nextSample = start + frames; }

private:
    bool m_initialized = false;
    uint64_t m_originPosition = 0, m_lastPosition = 0;
    int64_t m_originSample = 0, m_nextSample = 0;
};
