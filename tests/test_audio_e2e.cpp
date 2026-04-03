// End-to-end audio test: WASAPI capture → AudioAnalyzer → band values
// Verifies the full pipeline that feeds shader uniforms
#include "app/AudioAnalyzer.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

int main() {
    std::cout << "=== AudioAnalyzer End-to-End Test ===" << std::endl;
    std::cout << "This test captures system audio for 5 seconds and reports" << std::endl;
    std::cout << "the band values that would be sent to shader uniforms." << std::endl;
    std::cout << "Make sure audio is playing through your speakers." << std::endl;
    std::cout << std::endl;

    AudioAnalyzer analyzer;
    analyzer.setDevice(-1); // system loopback

    // Simulate the app's frame loop at ~60fps for 5 seconds
    const float dt = 1.0f / 60.0f;
    const int totalFrames = 300; // 5 seconds at 60fps

    int framesWithSignal = 0;
    float maxRMS = 0, maxBass = 0, maxMid = 0, maxHigh = 0;
    float avgRMS = 0, avgBass = 0, avgMid = 0, avgHigh = 0;
    int beatsDetected = 0;

    for (int frame = 0; frame < totalFrames; frame++) {
        analyzer.update(dt);

        float rms = analyzer.smoothedRMS();
        float bass = analyzer.bass();
        float mid = (analyzer.lowMid() + analyzer.highMid()) * 0.5f;
        float high = analyzer.treble();
        float beat = analyzer.beatDecay();

        if (rms > 0.001f) framesWithSignal++;
        if (analyzer.beatDetected()) beatsDetected++;

        maxRMS = std::max(maxRMS, rms);
        maxBass = std::max(maxBass, bass);
        maxMid = std::max(maxMid, mid);
        maxHigh = std::max(maxHigh, high);

        avgRMS += rms;
        avgBass += bass;
        avgMid += mid;
        avgHigh += high;

        // Print live values every 30 frames (~0.5s)
        if (frame % 30 == 0) {
            printf("  [%4.1fs] rms=%.3f  bass=%.3f  mid=%.3f  high=%.3f  beat=%.2f\n",
                   frame * dt, rms, bass, mid, high, beat);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds((int)(dt * 1000)));
    }

    avgRMS /= totalFrames;
    avgBass /= totalFrames;
    avgMid /= totalFrames;
    avgHigh /= totalFrames;

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Frames with signal: " << framesWithSignal << "/" << totalFrames
              << " (" << (100.0f * framesWithSignal / totalFrames) << "%)" << std::endl;
    std::cout << "Beats detected: " << beatsDetected << std::endl;
    std::cout << std::endl;
    std::cout << "  Uniform        Avg      Max" << std::endl;
    std::cout << "  ─────────────────────────────" << std::endl;
    printf("  audioLevel   %6.3f   %6.3f\n", avgRMS, maxRMS);
    printf("  audioBass    %6.3f   %6.3f\n", avgBass, maxBass);
    printf("  audioMid     %6.3f   %6.3f\n", avgMid, maxMid);
    printf("  audioHigh    %6.3f   %6.3f\n", avgHigh, maxHigh);
    std::cout << std::endl;

    // Calibration assessment
    std::cout << "=== Calibration Assessment ===" << std::endl;
    if (framesWithSignal < 10) {
        std::cout << "FAIL: No audio signal detected. Check system audio device." << std::endl;
        return 1;
    }

    bool needsCalibration = false;

    if (maxRMS < 0.05f) {
        std::cout << "WARNING: audioLevel very low (max " << maxRMS << "). Audio may be quiet." << std::endl;
    } else if (maxRMS > 0.95f) {
        std::cout << "WARNING: audioLevel clipping (max " << maxRMS << "). Consider reducing gain." << std::endl;
        needsCalibration = true;
    } else {
        std::cout << "OK: audioLevel range good (max " << maxRMS << ")" << std::endl;
    }

    if (maxBass < 0.05f) {
        std::cout << "WARNING: audioBass very low. Low-frequency content missing or gain too low." << std::endl;
    } else if (maxBass > 0.95f) {
        std::cout << "NOTE: audioBass hitting ceiling. Bass gain (6x) may be high for this content." << std::endl;
        needsCalibration = true;
    } else {
        std::cout << "OK: audioBass range good (max " << maxBass << ")" << std::endl;
    }

    if (maxMid < 0.05f) {
        std::cout << "WARNING: audioMid very low." << std::endl;
    } else {
        std::cout << "OK: audioMid range good (max " << maxMid << ")" << std::endl;
    }

    if (maxHigh < 0.05f) {
        std::cout << "WARNING: audioHigh very low." << std::endl;
    } else {
        std::cout << "OK: audioHigh range good (max " << maxHigh << ")" << std::endl;
    }

    if (!needsCalibration) {
        std::cout << std::endl << "PASS: Audio pipeline working. Values are reaching shader uniforms." << std::endl;
    } else {
        std::cout << std::endl << "PASS with notes: Audio is flowing but some bands may need gain adjustment." << std::endl;
    }

    return 0;
}
