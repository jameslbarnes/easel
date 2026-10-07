#include "app/RTMPOutput.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>

int main(int argc, char** argv) {
    // WASAPI QPC jitter must not add/delete samples in a continuous waveform.
    AudioCaptureClock clock;
    for (int i = 0; i < 10000; ++i) {
        int64_t expected = int64_t(i) * 480;
        int64_t stamp = expected + (i == 0 ? 0 : ((i % 7) - 3) * 8);
        auto actual = clock.sampleAt(1000 + expected, stamp, true);
        if (actual != expected) return 12;
        clock.advance(actual, 480);
    }
    // Missing packets preserve their real gap; an invalid timestamp continues
    // from the last packet, and a device position reset reanchors safely.
    auto gap = clock.sampleAt(1000 + 4800000 + 960, 4800960, true);
    if (gap != 4800960) return 13;
    clock.advance(gap, 480);
    if (clock.sampleAt(0, 0, false) != 4801440) return 14;
    if (clock.sampleAt(0, 4802000, true) != 4802000) return 15;
    if (argc < 2 || !glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(64, 64, "RTMP test", nullptr, nullptr);
    if (!window) return 2;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 3;
    GLuint texture;
    const bool software = std::getenv("EASEL_STREAM_ENCODER") &&
        std::string(std::getenv("EASEL_STREAM_ENCODER")) == "software";
    const int width = software ? 64 : 1920, height = software ? 64 : 1080;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    std::vector<unsigned char> pixels(width * height * 4, 128);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    {
        RTMPOutput output;
        // A local FLV exercises the identical encoding/mux/audio path without
        // publishing anything. Restart after geometry failure must join safely.
        for (int pass = 0; pass < 2; ++pass) {
            std::string dest = std::string(argv[1]) + std::to_string(pass) + ".flv";
            if (!output.startCustom(dest, width, height)) return 4;
            auto started = std::chrono::steady_clock::now();
            auto until = started + std::chrono::seconds(12);
            while (std::chrono::steady_clock::now() < until) {
                double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                // A two-second renderer stall plus slow/jittery delivery must
                // not shorten media time or stop the constant-rate broadcast.
                if (elapsed < 4 || elapsed > 6) output.sendFrame(texture, width, height);
                std::this_thread::sleep_for(std::chrono::milliseconds(pass ? 71 : 2));
            }
            if (!output.isActive() || output.packetsWritten() == 0) return 5;
            // GPU scheduling can stall while the live show is running. Overdue
            // slots may be skipped, but must be accounted for and never cause
            // a restart or let the media clock fall behind.
            std::cout << "Encoder=" << output.videoEncoder()
                      << " capture gaps=" << output.audioCaptureGaps()
                      << " video packets=" << output.videoPacketsWritten()
                      << " dropped=" << output.droppedFrames() << "\n";
            if (output.videoPacketsWritten() < (software ? 350 : 300) ||
                output.videoPacketsWritten() > 363 ||
                output.videoPacketsWritten() + output.droppedFrames() < 350) return 9;
            if (output.uptimeSeconds() - output.videoTimeSeconds() > 0.1) return 10;
            if (output.audioTimeSeconds() > 0 &&
                output.uptimeSeconds() - output.audioTimeSeconds() > 0.15) return 11;
            if (output.audioCaptureGaps() != 0) return 16;
            output.sendFrame(texture, width + 2, height);
            if (output.isActive()) return 6;
        }
        output.stop();
        // Optional blackhole RTMP listener proves handshakes time out.
        if (argc > 2) {
            auto before = std::chrono::steady_clock::now();
            if (output.startCustom(argv[2], 64, 64)) return 7;
            if (std::chrono::steady_clock::now() - before > std::chrono::seconds(12)) return 8;
        }
    }
    glDeleteTextures(1, &texture);
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "RTMP encoding, pacing, failure status and restart passed\n";
}
