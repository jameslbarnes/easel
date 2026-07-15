#pragma once
#include <cmath>
#include <vector>

class BPMSync {
public:
    // Where the current tempo came from. Manual (tap tempo, UI, OSC) always
    // overrides Detected (EaselAudio autocorrelation tempo); clearing the BPM
    // (setBPM(0)) returns to Free so detection may re-engage.
    enum class Source { Free = 0, Detected, Manual };

    // Call once per frame with delta time
    void update(float dt);

    // BPM control (user/OSC — marks the clock Manual; 0 clears back to Free)
    void setBPM(float bpm);
    float bpm() const { return m_bpm; }

    // Tap tempo: call on each tap (marks the clock Manual)
    void tap();

    // ── EaselAudio detected-tempo phase lock ────────────────────────────
    // Feed the analyzer's autocorrelation tempo each frame. When no manual
    // override is active and confidence >= 0.4, the detected tempo drives
    // (and phase-locks) this clock. Tap/OSC stay overrides.
    void feedDetected(float bpm, float confidence);
    // Nudge the phase toward the nearest beat boundary on a detected onset
    // (classic PLL pull). Only acts while the clock is detection-driven.
    void beatHint(float strength);
    Source source() const { return m_source; }
    float detectedConfidence() const { return m_detConf; }

    // Beat phase: 0.0 at beat start, 1.0 at next beat (sawtooth)
    float beatPhase() const { return m_phase; }

    // Beat pulse: 1.0 on beat, decays to 0.0 (for visual kicks)
    float beatPulse() const { return m_pulse; }

    // Beat number (increments each beat)
    int beatCount() const { return m_beatCount; }

    // Bar phase: 0.0-1.0 over 4 beats
    float barPhase() const { return m_barPhase; }

    // Phase ramp over n beats (n = 2/4/8/16 — MadMapper bpmOffset pattern)
    float phaseN(int n) const {
        if (n <= 1 || m_bpm <= 0.0f) return m_phase;
        return (float)(m_beatCount % n) / (float)n + m_phase / (float)n;
    }

    // Logistic-eased one-shot on each beat (~120ms) and eased 0/1 flip
    float onBeat() const { return m_onBeat; }
    float toggleOnBeat() const { return m_toggleEase; }

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

    // Detected-tempo sync state
    Source m_source = Source::Free;
    float m_detBpm = 0.0f;
    float m_detConf = 0.0f;
    // Detection-born tempo expiry: a clock the analyzer locked keeps
    // free-running through short confidence dips (breakdowns), but expires
    // to rest after this long without confidence — otherwise a paused
    // radio keeps every audioKick()/audioBeatPulse shader kicking at the
    // last (often junk) tempo forever. Manual tempos never expire.
    float m_lowConfTime = 0.0f;
    static constexpr float kDetectedExpirySeconds = 3.0f;

    // Beat-eased outputs
    float m_timeSinceBeat = 1e9f;
    float m_onBeat = 0.0f;
    int   m_toggle = 0;
    float m_toggleEase = 0.0f;

    // Tap tempo
    static constexpr int kMaxTaps = 8;
    double m_tapTimes[kMaxTaps] = {};
    int m_tapCount = 0;
    double m_lastTapTime = 0.0;
    static constexpr double kTapTimeout = 2.0; // reset if no tap for 2s

    float m_pulseDecayRate = 12.0f; // pulse decay speed
};
