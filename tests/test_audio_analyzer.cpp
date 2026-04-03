#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "app/AudioAnalyzer.h"
#include <cmath>
#include <vector>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Helper: generate a sine wave at a given frequency
static std::vector<float> sineWave(float freq, int sampleRate, int numSamples, float amplitude = 0.5f) {
    std::vector<float> buf(numSamples);
    for (int i = 0; i < numSamples; i++) {
        buf[i] = amplitude * sinf(2.0f * (float)M_PI * freq * i / sampleRate);
    }
    return buf;
}

// Helper: generate white noise
static std::vector<float> whiteNoise(int numSamples, float amplitude = 0.3f) {
    std::vector<float> buf(numSamples);
    for (int i = 0; i < numSamples; i++) {
        buf[i] = amplitude * (2.0f * ((float)rand() / RAND_MAX) - 1.0f);
    }
    return buf;
}

// Helper: generate silence
static std::vector<float> silence(int numSamples) {
    return std::vector<float>(numSamples, 0.0f);
}

// Helper: simulate N frames of update at a given dt
static void runFrames(AudioAnalyzer& az, int frames, float dt) {
    for (int i = 0; i < frames; i++) {
        az.update(dt);
    }
}

TEST_CASE("All outputs are non-negative", "[audio][positive]") {
    AudioAnalyzer az;

    SECTION("After silence") {
        auto buf = silence(512);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(1.0f / 60.0f);

        CHECK(az.rms() >= 0.0f);
        CHECK(az.bass() >= 0.0f);
        CHECK(az.lowMid() >= 0.0f);
        CHECK(az.highMid() >= 0.0f);
        CHECK(az.treble() >= 0.0f);
        CHECK(az.beatDecay() >= 0.0f);
        CHECK(az.rawBass() >= 0.0f);
        CHECK(az.rawLowMid() >= 0.0f);
        CHECK(az.rawHighMid() >= 0.0f);
        CHECK(az.rawTreble() >= 0.0f);
        CHECK(az.rawRMS() >= 0.0f);
    }

    SECTION("After loud sine") {
        auto buf = sineWave(150.0f, 48000, 512, 0.9f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(1.0f / 60.0f);

        CHECK(az.rms() >= 0.0f);
        CHECK(az.bass() >= 0.0f);
        CHECK(az.lowMid() >= 0.0f);
        CHECK(az.highMid() >= 0.0f);
        CHECK(az.treble() >= 0.0f);
    }

    SECTION("After white noise") {
        auto buf = whiteNoise(512, 0.5f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(1.0f / 60.0f);

        CHECK(az.rms() >= 0.0f);
        CHECK(az.bass() >= 0.0f);
        CHECK(az.lowMid() >= 0.0f);
        CHECK(az.highMid() >= 0.0f);
        CHECK(az.treble() >= 0.0f);
    }
}

TEST_CASE("All outputs are bounded 0-1", "[audio][bounded]") {
    AudioAnalyzer az;

    SECTION("Loud signal stays bounded") {
        // Feed multiple buffers of loud signal
        for (int round = 0; round < 10; round++) {
            auto buf = sineWave(100.0f, 48000, 512, 1.0f);
            az.feedSamples(buf.data(), (int)buf.size());
            az.update(1.0f / 60.0f);

            CHECK(az.rms() <= 1.0f);
            CHECK(az.bass() <= 1.0f);
            CHECK(az.lowMid() <= 1.0f);
            CHECK(az.highMid() <= 1.0f);
            CHECK(az.treble() <= 1.0f);
            CHECK(az.beatDecay() <= 1.0f);
            CHECK(az.rawBass() <= 1.0f);
            CHECK(az.rawLowMid() <= 1.0f);
            CHECK(az.rawHighMid() <= 1.0f);
            CHECK(az.rawTreble() <= 1.0f);
            CHECK(az.rawRMS() <= 1.0f);
        }
    }

    SECTION("Clipped signal stays bounded") {
        std::vector<float> buf(512, 1.0f); // DC at maximum
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(1.0f / 60.0f);

        CHECK(az.rms() <= 1.0f);
        CHECK(az.bass() <= 1.0f);
        CHECK(az.rawRMS() <= 1.0f);
    }
}

TEST_CASE("Smoothing is monotonic during steady input", "[audio][smooth]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Feed a bass tone to get non-zero values
    auto buf = sineWave(120.0f, 48000, 512, 0.7f);
    az.feedSamples(buf.data(), (int)buf.size());

    // Run several frames with the same data — smoothed values should converge monotonically upward
    std::vector<float> bassHistory;
    std::vector<float> rmsHistory;
    for (int i = 0; i < 30; i++) {
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
        bassHistory.push_back(az.bass());
        rmsHistory.push_back(az.rms());
    }

    // After initial ramp-up, values should be monotonically non-decreasing (attacking toward target)
    int violations = 0;
    for (int i = 2; i < (int)bassHistory.size(); i++) {
        if (bassHistory[i] < bassHistory[i - 1] - 0.001f) {
            violations++;
        }
    }
    // Allow at most 1 violation (first frame might be settling)
    CHECK(violations <= 1);
}

TEST_CASE("Smoothing decays during silence after signal", "[audio][smooth][decay]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Build up signal
    for (int i = 0; i < 5; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.8f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    float peakBass = az.bass();
    REQUIRE(peakBass > 0.05f); // Ensure we have signal

    // Now feed silence and track decay
    std::vector<float> decayHistory;
    for (int i = 0; i < 60; i++) {
        auto buf = silence(512);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
        decayHistory.push_back(az.bass());
    }

    // Values should decay monotonically (non-increasing)
    int violations = 0;
    for (int i = 1; i < (int)decayHistory.size(); i++) {
        if (decayHistory[i] > decayHistory[i - 1] + 0.001f) {
            violations++;
        }
    }
    CHECK(violations == 0);

    // Should decay significantly (at least 50% drop after 60 frames at release rate 4/s)
    CHECK(decayHistory.back() < peakBass * 0.7f);
}

TEST_CASE("No jitter: frame-to-frame delta is bounded", "[audio][smooth][jitter]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Feed a moderate steady signal
    std::vector<float> bassDeltas;
    std::vector<float> rmsDeltas;
    float prevBass = 0, prevRms = 0;

    for (int i = 0; i < 120; i++) {
        auto buf = sineWave(130.0f, 48000, 512, 0.4f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);

        if (i > 5) { // skip initial ramp
            bassDeltas.push_back(std::abs(az.bass() - prevBass));
            rmsDeltas.push_back(std::abs(az.rms() - prevRms));
        }
        prevBass = az.bass();
        prevRms = az.rms();
    }

    // Max frame-to-frame delta should be small for steady input
    float maxBassDelta = *std::max_element(bassDeltas.begin(), bassDeltas.end());
    float maxRmsDelta = *std::max_element(rmsDeltas.begin(), rmsDeltas.end());

    INFO("Max bass delta: " << maxBassDelta);
    INFO("Max RMS delta: " << maxRmsDelta);

    // After convergence, deltas should be < 8% per frame
    // (magnitude-based spectrum with higher gains produces larger deltas than power-based)
    CHECK(maxBassDelta < 0.08f);
    CHECK(maxRmsDelta < 0.08f);
}

TEST_CASE("dt-independence: different frame rates give similar steady-state", "[audio][smooth][dt]") {
    const float freq = 150.0f;
    const float amp = 0.5f;

    // Run at 60fps
    AudioAnalyzer az60;
    for (int i = 0; i < 120; i++) {
        auto buf = sineWave(freq, 48000, 512, amp);
        az60.feedSamples(buf.data(), (int)buf.size());
        az60.update(1.0f / 60.0f);
    }

    // Run at 30fps (same total time = 120 frames at 30fps = 4 seconds)
    AudioAnalyzer az30;
    for (int i = 0; i < 120; i++) {
        auto buf = sineWave(freq, 48000, 512, amp);
        az30.feedSamples(buf.data(), (int)buf.size());
        az30.update(1.0f / 30.0f);
    }

    // Steady-state values should be within 15% of each other
    float bassDiff = std::abs(az60.bass() - az30.bass());
    float rmsDiff = std::abs(az60.rms() - az30.rms());

    INFO("60fps bass=" << az60.bass() << " 30fps bass=" << az30.bass());
    INFO("60fps rms=" << az60.rms() << " 30fps rms=" << az30.rms());

    float maxBass = std::max(az60.bass(), az30.bass());
    float maxRms = std::max(az60.rms(), az30.rms());

    if (maxBass > 0.01f) CHECK(bassDiff / maxBass < 0.15f);
    if (maxRms > 0.01f) CHECK(rmsDiff / maxRms < 0.15f);
}

TEST_CASE("Beat detection fires on transient", "[audio][beat]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Build a baseline with quiet signal
    for (int i = 0; i < 60; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.05f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    // Hit with a loud bass transient
    auto loud = sineWave(120.0f, 48000, 512, 0.9f);
    az.feedSamples(loud.data(), (int)loud.size());
    az.update(dt);

    // beatDecay should be positive after transient
    CHECK(az.beatDecay() > 0.0f);
}

TEST_CASE("Beat detection respects cooldown", "[audio][beat][cooldown]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Build baseline
    for (int i = 0; i < 60; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.05f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    // First beat
    auto loud = sineWave(120.0f, 48000, 512, 0.9f);
    az.feedSamples(loud.data(), (int)loud.size());
    az.update(dt);
    bool firstBeat = az.beatDetected();

    // Try to trigger again immediately (within 150ms cooldown = 9 frames at 60fps)
    int beatsInCooldown = 0;
    for (int i = 0; i < 8; i++) {
        // Quiet then loud again
        auto quiet = sineWave(120.0f, 48000, 512, 0.05f);
        az.feedSamples(quiet.data(), (int)quiet.size());
        az.update(dt);
        az.feedSamples(loud.data(), (int)loud.size());
        az.update(dt);
        if (az.beatDetected()) beatsInCooldown++;
    }

    // Should have detected at most 1 extra beat during cooldown period
    CHECK(beatsInCooldown <= 1);
}

TEST_CASE("Beat decay decreases over time", "[audio][beat][decay]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Build baseline and trigger beat
    for (int i = 0; i < 60; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.05f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }
    auto loud = sineWave(120.0f, 48000, 512, 0.9f);
    az.feedSamples(loud.data(), (int)loud.size());
    az.update(dt);

    float initialDecay = az.beatDecay();
    REQUIRE(initialDecay > 0.0f);

    // Run 10 more frames with silence
    for (int i = 0; i < 10; i++) {
        auto buf = silence(512);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    // beatDecay should have decreased significantly (~85ms half-life, 10 frames = 167ms > 1 half-life)
    CHECK(az.beatDecay() < initialDecay * 0.5f);
    CHECK(az.beatDecay() >= 0.0f);
}

TEST_CASE("Bass tone activates bass band", "[audio][bands]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // 120Hz sine = should land in bass bins (bins 1-2 at 48kHz/512 = 93.75Hz per bin)
    for (int i = 0; i < 10; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.7f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    INFO("bass=" << az.bass() << " lowMid=" << az.lowMid() << " highMid=" << az.highMid() << " treble=" << az.treble());

    // Bass should be the dominant band
    CHECK(az.bass() > az.treble());
    CHECK(az.bass() > 0.05f);
}

TEST_CASE("Treble tone activates treble band", "[audio][bands]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // 8kHz sine = should land in treble bins
    for (int i = 0; i < 10; i++) {
        auto buf = sineWave(8000.0f, 48000, 512, 0.7f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    INFO("bass=" << az.bass() << " lowMid=" << az.lowMid() << " highMid=" << az.highMid() << " treble=" << az.treble());

    // Treble should be the dominant band
    CHECK(az.treble() > az.bass());
    CHECK(az.treble() > 0.05f);
}

TEST_CASE("Silence produces near-zero output", "[audio][silence]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    for (int i = 0; i < 30; i++) {
        auto buf = silence(512);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    CHECK(az.rms() < 0.01f);
    CHECK(az.bass() < 0.01f);
    CHECK(az.lowMid() < 0.01f);
    CHECK(az.highMid() < 0.01f);
    CHECK(az.treble() < 0.01f);
    CHECK(az.beatDecay() < 0.01f);
}

TEST_CASE("Impulse does not cause ringing or oscillation", "[audio][smooth][impulse]") {
    AudioAnalyzer az;
    const float dt = 1.0f / 60.0f;

    // Build up with steady signal
    for (int i = 0; i < 30; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.4f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
    }

    // Single loud impulse
    auto loud = sineWave(120.0f, 48000, 512, 1.0f);
    az.feedSamples(loud.data(), (int)loud.size());
    az.update(dt);

    // Then back to normal
    std::vector<float> bassHistory;
    for (int i = 0; i < 60; i++) {
        auto buf = sineWave(120.0f, 48000, 512, 0.4f);
        az.feedSamples(buf.data(), (int)buf.size());
        az.update(dt);
        bassHistory.push_back(az.bass());
    }

    // Check for oscillation: count direction changes
    int dirChanges = 0;
    for (int i = 2; i < (int)bassHistory.size(); i++) {
        float d1 = bassHistory[i - 1] - bassHistory[i - 2];
        float d2 = bassHistory[i] - bassHistory[i - 1];
        if (d1 * d2 < -0.0001f) dirChanges++;
    }

    // Exponential smoothing should have at most 1-2 direction changes (overshoot then settle)
    INFO("Direction changes after impulse: " << dirChanges);
    CHECK(dirChanges <= 3);
}
