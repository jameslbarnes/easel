#pragma once
#include <cmath>
#include <vector>

class BPMSync {
public:
    // Call once per frame with delta time
    void update(float dt);

    // BPM control
    void setBPM(float bpm);
    float bpm() const { return m_bpm; }

    // Tap tempo: call on each tap
    void tap();

    // Beat phase: 0.0 at beat start, 1.0 at next beat (sawtooth)
    float beatPhase() const { return m_phase; }

    // Beat pulse: 1.0 on beat, decays to 0.0 (for visual kicks)
    float beatPulse() const { return m_pulse; }

    // Beat number (increments each beat)
    int beatCount() const { return m_beatCount; }

    // Bar phase: 0.0-1.0 over 4 beats
    float barPhase() const { return m_barPhase; }

    // Is BPM sync active?
    bool active() const { return m_bpm > 0.0f; }

    // Nudge phase forward/back for manual alignment
    void nudge(float amount) { m_phase = fmodf(m_phase + amount + 1.0f, 1.0f); }

    // Reset phase to downbeat
    void resetPhase() { m_phase = 0.0f; m_barPhase = 0.0f; m_beatCount = 0; }

private:
    float m_bpm = 0.0f;          // 0 = disabled
    float m_phase = 0.0f;        // 0-1 sawtooth per beat
    float m_barPhase = 0.0f;     // 0-1 sawtooth per 4 beats
    float m_pulse = 0.0f;        // 1.0 on beat, decays
    int m_beatCount = 0;

    // Tap tempo
    static constexpr int kMaxTaps = 8;
    double m_tapTimes[kMaxTaps] = {};
    int m_tapCount = 0;
    double m_lastTapTime = 0.0;
    static constexpr double kTapTimeout = 2.0; // reset if no tap for 2s

    float m_pulseDecayRate = 12.0f; // pulse decay speed
};
