#ifdef __APPLE__
#include "app/AudioAnalyzer.h"
#include <iostream>

// Stub implementation — audio capture on macOS requires ScreenCaptureKit
// (for system audio loopback) or a virtual audio device.
// For now, the analyzer works via feedSamples() from AudioMixer or test code.

void AudioAnalyzer::initCapture() {
    static bool logged = false;
    if (!logged) {
        std::cout << "[AudioAnalyzer] macOS: audio capture stub (use feedSamples or AudioMixer)" << std::endl;
        logged = true;
    }
    m_initialized = false;
}

void AudioAnalyzer::cleanupCapture() {
    m_initialized = false;
    m_samplesAccumulated = 0;
}

void AudioAnalyzer::drainPackets() {
    // No-op on macOS stub
}
#endif
