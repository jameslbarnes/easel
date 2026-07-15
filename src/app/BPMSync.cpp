#include "app/BPMSync.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

void BPMSync::update(float dt) {
    if (dt <= 0.0f) return;

    // ── Detected-tempo drive (EaselAudio) ───────────────────────────────
    // Only when the user hasn't manually set/tapped a tempo. The detected
    // BPM slews in gently (never snaps mid-performance); the phase itself
    // is pulled on onsets via beatHint() — a light PLL, so audioBeatPhase /
    // audioBPM are real even when nobody touched tap tempo.
    if (m_source != Source::Manual && m_detConf >= 0.4f && m_detBpm > 0.0f) {
        m_source = Source::Detected;
        if (m_bpm <= 0.0f || std::fabs(m_detBpm - m_bpm) > 15.0f) {
            m_bpm = m_detBpm;                      // engage / re-lock
        } else {
            m_bpm += (m_detBpm - m_bpm) * (1.0f - expf(-dt / 1.5f));
        }
    }

    // ── Detection-born tempo expiry ─────────────────────────────────────
    // Free-run through short confidence dips (a breakdown, a quiet bridge)
    // but come to rest when the music actually stopped: sustained low
    // confidence expires a tempo the analyzer locked, so a paused source
    // doesn't keep beat-driven visuals kicking at the last tempo forever.
    // Manual tempos (tap/OSC) are the user's word and never expire; a
    // confident re-detection re-locks instantly via the drive block above.
    if (m_source != Source::Manual && m_bpm > 0.0f) {
        if (m_detConf < 0.15f) {
            m_lowConfTime += dt;
            if (m_lowConfTime >= kDetectedExpirySeconds) {
                m_bpm = 0.0f;
                m_source = Source::Free;
            }
        } else {
            m_lowConfTime = 0.0f;
        }
    }

    if (m_bpm <= 0.0f) {
        // Clock inactive — ease every beat output to rest (the pulse used
        // to freeze at its last value here, leaving a phantom kick).
        m_onBeat = 0.0f;
        m_pulse *= expf(-m_pulseDecayRate * dt);
        return;
    }

    float beatsPerSecond = m_bpm / 60.0f;
    m_phase += beatsPerSecond * dt;

    // Beat boundary crossed
    while (m_phase >= 1.0f) {
        m_phase -= 1.0f;
        m_pulse = 1.0f;
        m_beatCount++;
        m_timeSinceBeat = 0.0f;
        m_toggle = 1 - m_toggle;
    }

    // Bar phase (4 beats)
    m_barPhase = fmodf((float)(m_beatCount % 4) / 4.0f + m_phase / 4.0f, 1.0f);

    // Decay pulse (exponential, frame-rate independent)
    m_pulse *= expf(-m_pulseDecayRate * dt);

    // Logistic-eased one-shot (~120ms) + eased toggle flip
    m_timeSinceBeat += dt;
    float s = m_timeSinceBeat / 0.12f;
    m_onBeat = 1.0f / (1.0f + expf(12.0f * (s - 0.5f)));
    m_toggleEase += ((float)m_toggle - m_toggleEase) * (1.0f - expf(-dt / 0.03f));
}

void BPMSync::setBPM(float bpm) {
    m_bpm = std::max(0.0f, std::min(300.0f, bpm));
    // Explicit user/OSC tempo is a Manual override; clearing (0) frees the
    // clock so the detected tempo may drive it again.
    m_source = (m_bpm > 0.0f) ? Source::Manual : Source::Free;
}

void BPMSync::feedDetected(float bpm, float confidence) {
    m_detBpm = bpm;
    m_detConf = confidence;
    // Sustained loss of confidence while detection-driven: keep the last
    // locked tempo free-running (no snap to silence), but flag it Free so a
    // future confident detection can re-lock cleanly.
    if (m_source == Source::Detected && confidence < 0.15f) {
        m_source = Source::Free;
    }
}

void BPMSync::beatHint(float strength) {
    if (m_source != Source::Detected || m_bpm <= 0.0f) return;
    // Pull phase toward the nearest beat boundary (wrap-aware error)
    float err = (m_phase < 0.5f) ? m_phase : m_phase - 1.0f;
    float gain = 0.20f * std::min(std::max(strength, 0.0f), 1.0f);
    m_phase = fmodf(m_phase - err * gain + 1.0f, 1.0f);
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

    // Tapping is always a manual override of the detected tempo
    m_source = Source::Manual;

    // Reset phase on every tap (sync to downbeat)
    m_phase = 0.0f;
    m_pulse = 1.0f;
    // First tap of a new sequence resets beat count
    if (m_tapCount == 1) {
        m_beatCount = 0;
    }
}
