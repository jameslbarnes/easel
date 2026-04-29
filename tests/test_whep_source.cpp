// Tests for WHEPSource — focuses on the data path that the frame-extraction
// optimization (requestVideoFrameCallback / backpressure) is going to touch.
//
// We test what's observable from public methods without standing up libdatachannel,
// FFmpeg decode, WKWebView, or an OpenGL context. The intent is regression cover
// for the WebView frame ingest path (onWebViewFrame / onWebViewStatus / disconnect).

#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#ifndef HAS_WHEP
TEST_CASE("WHEP not built", "[whep]") {
    SUCCEED("HAS_WHEP not defined — WHEP source compiled out");
}
#else

#include "sources/WHEPSource.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

static std::vector<uint8_t> makeFrame(int w, int h, uint8_t fill = 0xAB) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, fill);
}

TEST_CASE("Default-constructed source has empty state", "[whep][init]") {
    WHEPSource src;
    CHECK(src.width() == 0);
    CHECK(src.height() == 0);
    CHECK(src.isConnected() == false);
    CHECK(src.isFailed() == false);
    CHECK(src.statusText().empty());
    CHECK(src.typeName() == "WHEP");
    CHECK(src.isVideo() == true);
    CHECK(src.isPlaying() == false);
    CHECK(src.sourcePath().empty());
}

TEST_CASE("onWebViewFrame stores width/height and marks connected", "[whep][frame]") {
    WHEPSource src;
    auto data = makeFrame(320, 240);
    src.onWebViewFrame(320, 240, data.data(), static_cast<int>(data.size()));
    CHECK(src.width() == 320);
    CHECK(src.height() == 240);
    CHECK(src.isConnected() == true);
    CHECK(src.isPlaying() == true);
}

TEST_CASE("onWebViewFrame rejects mismatched dataLen", "[whep][frame][validation]") {
    SECTION("Underflow leaves source unconnected") {
        WHEPSource src;
        auto data = makeFrame(320, 240);
        src.onWebViewFrame(320, 240, data.data(), 100);
        CHECK(src.width() == 0);
        CHECK(src.height() == 0);
        CHECK(src.isConnected() == false);
    }
    SECTION("Overflow leaves source unconnected") {
        WHEPSource src;
        auto data = makeFrame(320, 240);
        src.onWebViewFrame(320, 240, data.data(),
                           static_cast<int>(data.size()) + 1);
        CHECK(src.width() == 0);
        CHECK(src.isConnected() == false);
    }
    SECTION("Zero length leaves source unconnected") {
        WHEPSource src;
        auto data = makeFrame(320, 240);
        src.onWebViewFrame(320, 240, data.data(), 0);
        CHECK(src.width() == 0);
        CHECK(src.isConnected() == false);
    }
    SECTION("Zero dimensions with zero length passes validation (lenient)") {
        // Documents current behavior: 0 == 0*0*4 so the dataLen check accepts
        // an empty frame and flips connected=true. The JS guard at
        // `videoWidth > 0` prevents this in practice, but if a future change
        // tightens C++-side validation this test will need updating.
        WHEPSource src;
        uint8_t dummy = 0;
        src.onWebViewFrame(0, 0, &dummy, 0);
        CHECK(src.width() == 0);
        CHECK(src.height() == 0);
    }
}

TEST_CASE("onWebViewFrame handles resolution changes", "[whep][frame]") {
    WHEPSource src;
    auto small = makeFrame(320, 240, 0x11);
    auto large = makeFrame(1920, 1080, 0x22);
    src.onWebViewFrame(320, 240, small.data(), static_cast<int>(small.size()));
    REQUIRE(src.width() == 320);
    REQUIRE(src.height() == 240);

    src.onWebViewFrame(1920, 1080, large.data(), static_cast<int>(large.size()));
    CHECK(src.width() == 1920);
    CHECK(src.height() == 1080);
    CHECK(src.isConnected() == true);
}

TEST_CASE("Many sequential frames don't grow state unboundedly", "[whep][frame][ring]") {
    // 30 frames at 1080p is more than the 3-slot ring; verify nothing leaks
    // observable state and the latest dimensions persist.
    WHEPSource src;
    auto data = makeFrame(1920, 1080, 0xCD);
    for (int i = 0; i < 30; i++) {
        src.onWebViewFrame(1920, 1080, data.data(), static_cast<int>(data.size()));
    }
    CHECK(src.width() == 1920);
    CHECK(src.height() == 1080);
    CHECK(src.isConnected() == true);
}

TEST_CASE("onWebViewStatus state machine", "[whep][status]") {
    SECTION("got-track marks connected") {
        WHEPSource src;
        src.onWebViewStatus("got-track: video");
        CHECK(src.isConnected() == true);
        CHECK(src.isFailed() == false);
        CHECK(src.statusText() == "got-track: video");
    }
    SECTION("pc:connected marks connected") {
        WHEPSource src;
        src.onWebViewStatus("pc:connected");
        CHECK(src.isConnected() == true);
        CHECK(src.isFailed() == false);
    }
    SECTION("pc:failed marks failed") {
        WHEPSource src;
        src.onWebViewStatus("pc:failed");
        CHECK(src.isFailed() == true);
        CHECK(src.isConnected() == false);
    }
    SECTION("error: marks failed") {
        WHEPSource src;
        src.onWebViewStatus("error: no answer from any endpoint");
        CHECK(src.isFailed() == true);
    }
    SECTION("Neutral statuses only update text") {
        WHEPSource src;
        src.onWebViewStatus("ice-gathered");
        CHECK(src.isConnected() == false);
        CHECK(src.isFailed() == false);
        CHECK(src.statusText() == "ice-gathered");
    }
    SECTION("Status text is the latest written") {
        WHEPSource src;
        src.onWebViewStatus("first");
        src.onWebViewStatus("second");
        src.onWebViewStatus("third");
        CHECK(src.statusText() == "third");
    }
    SECTION("Connected sticks once set, even on neutral followups") {
        WHEPSource src;
        src.onWebViewStatus("got-track: video");
        REQUIRE(src.isConnected() == true);
        src.onWebViewStatus("video-size: 1280x720");
        CHECK(src.isConnected() == true);  // not cleared by neutral status
    }
}

TEST_CASE("disconnect is idempotent on a fresh source", "[whep][lifecycle]") {
    WHEPSource src;
    REQUIRE_NOTHROW(src.disconnect());
    REQUIRE_NOTHROW(src.disconnect());
    REQUIRE_NOTHROW(src.disconnect());
    CHECK(src.width() == 0);
    CHECK(src.height() == 0);
    CHECK(src.isConnected() == false);
    CHECK(src.sourcePath().empty());
}

TEST_CASE("disconnect clears state set by onWebViewFrame", "[whep][lifecycle]") {
    WHEPSource src;
    auto data = makeFrame(640, 480);
    src.onWebViewFrame(640, 480, data.data(), static_cast<int>(data.size()));
    REQUIRE(src.width() == 640);
    REQUIRE(src.isConnected() == true);

    src.disconnect();
    CHECK(src.width() == 0);
    CHECK(src.height() == 0);
    CHECK(src.isConnected() == false);
}

TEST_CASE("Destructor on an active-looking source doesn't crash", "[whep][lifecycle]") {
    {
        WHEPSource src;
        auto data = makeFrame(256, 256);
        src.onWebViewFrame(256, 256, data.data(), static_cast<int>(data.size()));
        src.onWebViewStatus("got-track: video");
    }
    SUCCEED("Destructor returned");
}

TEST_CASE("Many disconnect/frame cycles don't crash or leak observable state",
          "[whep][lifecycle][stress]") {
    // Simulates a flaky network where we repeatedly receive and tear down.
    WHEPSource src;
    auto data = makeFrame(640, 480);
    for (int cycle = 0; cycle < 20; cycle++) {
        src.onWebViewStatus("got-track: video");
        src.onWebViewFrame(640, 480, data.data(), static_cast<int>(data.size()));
        REQUIRE(src.isConnected() == true);
        src.disconnect();
        CHECK(src.width() == 0);
        CHECK(src.isConnected() == false);
    }
}

TEST_CASE("Concurrent onWebViewFrame writes are safe", "[whep][thread]") {
    WHEPSource src;
    constexpr int NUM_WRITERS = 4;
    constexpr int FRAMES_PER_WRITER = 200;
    std::vector<std::thread> writers;

    for (int t = 0; t < NUM_WRITERS; t++) {
        writers.emplace_back([&src, t, framesPerWriter = FRAMES_PER_WRITER]() {
            std::vector<uint8_t> buf(160 * 120 * 4, static_cast<uint8_t>(t));
            for (int i = 0; i < framesPerWriter; i++) {
                src.onWebViewFrame(160, 120, buf.data(),
                                   static_cast<int>(buf.size()));
            }
        });
    }

    // Reader: poll public state (mimics main loop)
    for (int i = 0; i < 200; i++) {
        (void)src.width();
        (void)src.height();
        (void)src.isConnected();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    for (auto& t : writers) t.join();
    CHECK(src.isConnected() == true);
    CHECK(src.width() == 160);
    CHECK(src.height() == 120);
}

TEST_CASE("Concurrent onWebViewStatus writes are safe", "[whep][thread]") {
    WHEPSource src;
    constexpr int NUM_WRITERS = 4;
    std::vector<std::thread> writers;
    for (int t = 0; t < NUM_WRITERS; t++) {
        writers.emplace_back([&src, t]() {
            for (int i = 0; i < 100; i++) {
                src.onWebViewStatus("status-" + std::to_string(t) + "-" +
                                    std::to_string(i));
            }
        });
    }
    for (auto& t : writers) t.join();
    CHECK(!src.statusText().empty());
}

TEST_CASE("Mixed concurrent frame + status writes are safe", "[whep][thread]") {
    // Real workload: WKWebView posts both frame and status messages from
    // the WebContent process; both arrive on the main runloop but ordering
    // isn't deterministic. Verify neither path corrupts the other.
    WHEPSource src;
    std::atomic<bool> stop{false};

    std::thread frameThread([&]() {
        std::vector<uint8_t> buf(320 * 240 * 4, 0x55);
        while (!stop.load()) {
            src.onWebViewFrame(320, 240, buf.data(),
                               static_cast<int>(buf.size()));
        }
    });
    std::thread statusThread([&]() {
        int i = 0;
        while (!stop.load()) {
            src.onWebViewStatus("video-size: " + std::to_string(i++));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    frameThread.join();
    statusThread.join();

    CHECK(src.isConnected() == true);
    CHECK(src.width() == 320);
    CHECK(src.height() == 240);
}

TEST_CASE("isPlaying tracks isConnected", "[whep][meta]") {
    WHEPSource src;
    CHECK(src.isPlaying() == false);
    src.onWebViewStatus("got-track: video");
    CHECK(src.isPlaying() == true);
    src.disconnect();
    CHECK(src.isPlaying() == false);
}

TEST_CASE("Frame data is fully copied before onWebViewFrame returns",
          "[whep][frame][copy]") {
    // Caller must be free to deallocate the source buffer immediately after
    // onWebViewFrame returns. If we kept a pointer instead of copying, this
    // test would still pass — but a follow-up frame with a different fill
    // would corrupt earlier frames sitting in the ring buffer. We can't
    // observe ring buffer contents directly, but we can at least validate
    // the call sequence doesn't crash with a freed buffer.
    WHEPSource src;
    {
        auto data = makeFrame(128, 128, 0xEE);
        src.onWebViewFrame(128, 128, data.data(), static_cast<int>(data.size()));
    }  // data goes out of scope
    // Call again with a different buffer; if onWebViewFrame held a pointer
    // into the freed buffer, this would touch freed memory.
    {
        auto data = makeFrame(128, 128, 0xFF);
        src.onWebViewFrame(128, 128, data.data(), static_cast<int>(data.size()));
    }
    CHECK(src.width() == 128);
    CHECK(src.height() == 128);
}

#endif  // HAS_WHEP
