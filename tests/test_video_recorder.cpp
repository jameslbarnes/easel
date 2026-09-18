// Offline harness for the VideoRecorder audio path: records N seconds of
// audio-only-relevant activity (video frames are never fed — the audio chain
// runs on the encode thread regardless), then stops cleanly so the trailer is
// written and the file is inspectable with ffprobe.
#include "app/VideoRecorder.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 12;
    const char* out = argc > 2 ? argv[2] : "test_rec_output.mp4";

    VideoRecorder rec;
    rec.setAudioDevice(-1);
    if (!rec.start(out, 64, 64, 30)) {
        std::cerr << "start() failed" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    rec.stop();
    std::cout << "done: " << out << std::endl;
    return 0;
}
