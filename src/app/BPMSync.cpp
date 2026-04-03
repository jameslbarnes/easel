#include "app/BPMSync.h"
#include <GLFW/glfw3.h>
#include <algorithm>

void BPMSync::update(float dt) {
    if (m_bpm <= 0.0f || dt <= 0.0f) return;

    float beatsPerSecond = m_bpm / 60.0f;
    float phaseInc = beatsPerSecond * dt;

    float oldPhase = m_phase;
    m_phase += phaseInc;

    // Beat boundary crossed
    if (m_phase >= 1.0f) {
        m_phase -= floorf(m_phase);
        m_pulse = 1.0f;
        m_beatCount++;
    }

    // Bar phase (4 beats)
    m_barPhase = fmodf((float)m_beatCount / 4.0f + m_phase / 4.0f, 1.0f);

    // Decay pulse
    m_pulse = std::max(0.0f, m_pulse - m_pulseDecayRate * dt);
}

void BPMSync::setBPM(float bpm) {
    m_bpm = std::max(0.0f, std::min(300.0f, bpm));
}

void BPMSync::tap() {
    double now = glfwGetTime();

    // Reset if too long since last tap
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

    // Need at least 2 taps to compute BPM
    if (m_tapCount >= 2) {
        double totalInterval = m_tapTimes[m_tapCount - 1] - m_tapTimes[0];
        double avgInterval = totalInterval / (double)(m_tapCount - 1);
        if (avgInterval > 0.0) {
            m_bpm = (float)(60.0 / avgInterval);
            m_bpm = std::max(30.0f, std::min(300.0f, m_bpm));
        }
    }

    // Reset phase on tap (align to downbeat)
    m_phase = 0.0f;
    m_pulse = 1.0f;
}
