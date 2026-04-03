#include "app/BPMSync.h"
#include <GLFW/glfw3.h>
#include <algorithm>

void BPMSync::update(float dt) {
    if (m_bpm <= 0.0f || dt <= 0.0f) return;

    float beatsPerSecond = m_bpm / 60.0f;
    m_phase += beatsPerSecond * dt;

    // Beat boundary crossed
    while (m_phase >= 1.0f) {
        m_phase -= 1.0f;
        m_pulse = 1.0f;
        m_beatCount++;
    }

    // Bar phase (4 beats)
    m_barPhase = fmodf((float)(m_beatCount % 4) / 4.0f + m_phase / 4.0f, 1.0f);

    // Decay pulse (exponential, frame-rate independent)
    m_pulse *= expf(-m_pulseDecayRate * dt);
}

void BPMSync::setBPM(float bpm) {
    m_bpm = std::max(0.0f, std::min(300.0f, bpm));
}

void BPMSync::tap() {
    double now = glfwGetTime();

    // Reset if too long since last tap (Resolume uses ~2s timeout)
    if (m_tapCount > 0 && (now - m_lastTapTime) > kTapTimeout) {
        m_tapCount = 0;
    }

    // Shift taps if buffer full
    if (m_tapCount >= kMaxTaps) {
        for (int i = 0; i < kMaxTaps - 1; i++) {
            m_tapTimes[i] = m_tapTimes[i + 1];
        }
        m_tapCount = kMaxTaps - 1;
    }

    m_tapTimes[m_tapCount++] = now;
    m_lastTapTime = now;

    // Resolume-style: use median of recent intervals for stability
    if (m_tapCount >= 2) {
        int numIntervals = m_tapCount - 1;
        double intervals[kMaxTaps];
        for (int i = 0; i < numIntervals; i++) {
            intervals[i] = m_tapTimes[i + 1] - m_tapTimes[i];
        }
        // Sort and take median (rejects outliers better than average)
        std::sort(intervals, intervals + numIntervals);
        double median = intervals[numIntervals / 2];
        if (median > 0.0) {
            m_bpm = (float)(60.0 / median);
            m_bpm = std::max(30.0f, std::min(300.0f, m_bpm));
        }
    }

    // Reset phase on every tap (sync to downbeat)
    m_phase = 0.0f;
    m_pulse = 1.0f;
    // First tap of a new sequence resets beat count
    if (m_tapCount == 1) {
        m_beatCount = 0;
    }
}
