#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <vector>
#include <fstream>
#include <filesystem>

// Pull in the scanning headers — tests don't need OpenGL context for pure math
#include "scanning/GrayCodePattern.h"
#include "scanning/SceneScanner.h"

// ============================================================================
// GrayCodePattern — encoding/decoding
// ============================================================================

TEST_CASE("Gray code encode/decode roundtrip", "[graycode]") {
    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t gray = GrayCodePattern::toGrayCode(i);
        uint32_t back = GrayCodePattern::fromGrayCode(gray);
        REQUIRE(back == i);
    }
}

TEST_CASE("Gray code values are correct for small inputs", "[graycode]") {
    // Known Gray code sequence: 0,1,3,2,6,7,5,4,12,13,15,14,10,11,9,8
    CHECK(GrayCodePattern::toGrayCode(0) == 0);
    CHECK(GrayCodePattern::toGrayCode(1) == 1);
    CHECK(GrayCodePattern::toGrayCode(2) == 3);
    CHECK(GrayCodePattern::toGrayCode(3) == 2);
    CHECK(GrayCodePattern::toGrayCode(4) == 6);
    CHECK(GrayCodePattern::toGrayCode(5) == 7);
    CHECK(GrayCodePattern::toGrayCode(6) == 5);
    CHECK(GrayCodePattern::toGrayCode(7) == 4);
}

TEST_CASE("Gray code adjacent values differ by exactly one bit", "[graycode]") {
    for (uint32_t i = 0; i < 10000; i++) {
        uint32_t g1 = GrayCodePattern::toGrayCode(i);
        uint32_t g2 = GrayCodePattern::toGrayCode(i + 1);
        uint32_t diff = g1 ^ g2;
        // diff should be a power of 2 (exactly one bit)
        REQUIRE(diff != 0);
        REQUIRE((diff & (diff - 1)) == 0);
    }
}

TEST_CASE("fromGrayCode is the inverse of toGrayCode for large values", "[graycode]") {
    // Test edge cases around powers of 2
    std::vector<uint32_t> testValues = {
        0, 1, 255, 256, 511, 512, 1023, 1024,
        1919, 1920, 2047, 2048, 4095, 4096,
        65535, 65536, 1048575
    };
    for (uint32_t v : testValues) {
        REQUIRE(GrayCodePattern::fromGrayCode(GrayCodePattern::toGrayCode(v)) == v);
    }
}

// ============================================================================
// GrayCodePattern — pattern counts
// ============================================================================

TEST_CASE("Pattern count for 1920x1080", "[graycode][patterns]") {
    GrayCodePattern pat;
    pat.init(1920, 1080);

    int colBits = (int)std::ceil(std::log2(1920.0)); // 11
    int rowBits = (int)std::ceil(std::log2(1080.0)); // 11

    CHECK(pat.columnPatternCount() == colBits);
    CHECK(pat.rowPatternCount() == rowBits);
    // white + black + colBits*2 + rowBits*2
    CHECK(pat.patternCount() == 2 + colBits * 2 + rowBits * 2);
    CHECK(pat.patternCount() == 46);
}

TEST_CASE("Pattern count for 1024x768", "[graycode][patterns]") {
    GrayCodePattern pat;
    pat.init(1024, 768);

    CHECK(pat.columnPatternCount() == 10); // ceil(log2(1024)) = 10
    CHECK(pat.rowPatternCount() == 10);    // ceil(log2(768)) = 10
    CHECK(pat.patternCount() == 42);
}

TEST_CASE("Pattern count for power-of-2 resolution", "[graycode][patterns]") {
    GrayCodePattern pat;
    pat.init(256, 256);

    CHECK(pat.columnPatternCount() == 8);
    CHECK(pat.rowPatternCount() == 8);
    CHECK(pat.patternCount() == 34);
}

TEST_CASE("Pattern count for small resolution", "[graycode][patterns]") {
    GrayCodePattern pat;
    pat.init(2, 2);

    CHECK(pat.columnPatternCount() == 1);
    CHECK(pat.rowPatternCount() == 1);
    CHECK(pat.patternCount() == 6); // white + black + 2 col + 2 row
}

TEST_CASE("Projector dimensions stored correctly", "[graycode][patterns]") {
    GrayCodePattern pat;
    pat.init(1920, 1080);
    CHECK(pat.projectorWidth() == 1920);
    CHECK(pat.projectorHeight() == 1080);
}

// ============================================================================
// SceneScanner — shadow mask
// ============================================================================

// Expose private methods for testing via a test helper
// We test through the public interface where possible, and use synthetic cv::Mat data

TEST_CASE("Shadow mask basic thresholding", "[scanner][shadowmask]") {
    // Create synthetic white and black images
    int rows = 100, cols = 100;
    cv::Mat white(rows, cols, CV_8U, cv::Scalar(200));
    cv::Mat black(rows, cols, CV_8U, cv::Scalar(10));

    // Manually compute what the scanner does: abs(white - black) > threshold
    cv::Mat diff, mask;
    cv::absdiff(white, black, diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    // All pixels should be valid (200 - 10 = 190 > 40)
    int validCount = cv::countNonZero(mask);
    CHECK(validCount == rows * cols);
}

TEST_CASE("Shadow mask rejects low-contrast areas", "[scanner][shadowmask]") {
    int rows = 100, cols = 100;
    cv::Mat white(rows, cols, CV_8U, cv::Scalar(50));
    cv::Mat black(rows, cols, CV_8U, cv::Scalar(30));

    cv::Mat diff, mask;
    cv::absdiff(white, black, diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    // diff = 20, below threshold of 40
    int validCount = cv::countNonZero(mask);
    CHECK(validCount == 0);
}

TEST_CASE("Shadow mask mixed valid/invalid regions", "[scanner][shadowmask]") {
    int rows = 100, cols = 200;
    cv::Mat white(rows, cols, CV_8U);
    cv::Mat black(rows, cols, CV_8U, cv::Scalar(10));

    // Left half: bright (valid), right half: dim (invalid)
    white(cv::Rect(0, 0, 100, 100)) = cv::Scalar(200);
    white(cv::Rect(100, 0, 100, 100)) = cv::Scalar(30);

    cv::Mat diff, mask;
    cv::absdiff(white, black, diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    int validCount = cv::countNonZero(mask);
    CHECK(validCount == 100 * 100); // Only left half
}

// ============================================================================
// SceneScanner — Grey code decode with synthetic captures
// ============================================================================

// Helper: generate a synthetic set of captures as if a camera saw the patterns
// For a known projector column `px`, the Grey code is toGrayCode(px).
// The "normal" pattern for bit b is bright if that bit is 1, dark otherwise.
// The "inverted" pattern is the opposite.
static std::vector<cv::Mat> generateSyntheticCaptures(
    int camRows, int camCols,
    int projW, int projH,
    // For each camera pixel, what projector column and row does it see?
    std::function<std::pair<int,int>(int camY, int camX)> correspondenceFunc)
{
    int colBits = (int)std::ceil(std::log2((double)projW));
    int rowBits = (int)std::ceil(std::log2((double)projH));
    int totalPatterns = 2 + colBits * 2 + rowBits * 2;

    std::vector<cv::Mat> captures(totalPatterns);

    // White reference
    captures[0] = cv::Mat(camRows, camCols, CV_8U, cv::Scalar(200));
    // Black reference
    captures[1] = cv::Mat(camRows, camCols, CV_8U, cv::Scalar(10));

    // Column patterns
    for (int b = 0; b < colBits; b++) {
        int msBit = colBits - 1 - b;
        cv::Mat normal(camRows, camCols, CV_8U);
        cv::Mat inverted(camRows, camCols, CV_8U);

        for (int y = 0; y < camRows; y++) {
            for (int x = 0; x < camCols; x++) {
                auto [projX, projY] = correspondenceFunc(y, x);
                uint32_t gray = GrayCodePattern::toGrayCode((uint32_t)projX);
                bool bitOn = ((gray >> msBit) & 1) != 0;

                normal.at<uint8_t>(y, x) = bitOn ? 200 : 10;
                inverted.at<uint8_t>(y, x) = bitOn ? 10 : 200;
            }
        }
        captures[2 + b * 2] = normal;
        captures[2 + b * 2 + 1] = inverted;
    }

    // Row patterns
    for (int b = 0; b < rowBits; b++) {
        int msBit = rowBits - 1 - b;
        cv::Mat normal(camRows, camCols, CV_8U);
        cv::Mat inverted(camRows, camCols, CV_8U);

        for (int y = 0; y < camRows; y++) {
            for (int x = 0; x < camCols; x++) {
                auto [projX, projY] = correspondenceFunc(y, x);
                uint32_t gray = GrayCodePattern::toGrayCode((uint32_t)projY);
                bool bitOn = ((gray >> msBit) & 1) != 0;

                normal.at<uint8_t>(y, x) = bitOn ? 200 : 10;
                inverted.at<uint8_t>(y, x) = bitOn ? 10 : 200;
            }
        }
        int rowStart = 2 + colBits * 2;
        captures[rowStart + b * 2] = normal;
        captures[rowStart + b * 2 + 1] = inverted;
    }

    return captures;
}

TEST_CASE("Grey code decode recovers identity correspondence", "[scanner][decode]") {
    // Camera and projector are aligned 1:1 — camera pixel (x,y) sees projector pixel (x,y)
    int size = 64;
    auto captures = generateSyntheticCaptures(size, size, size, size,
        [](int y, int x) { return std::make_pair(x, y); });

    int colBits = (int)std::ceil(std::log2((double)size));

    // Compute shadow mask
    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    // Decode columns manually (same algorithm as SceneScanner)
    cv::Mat decoded = cv::Mat::zeros(size, size, CV_32S);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;

            uint32_t grayCode = 0;
            for (int b = 0; b < colBits; b++) {
                int normalIdx = 2 + b * 2;
                int invertIdx = 2 + b * 2 + 1;
                uint8_t nv = captures[normalIdx].at<uint8_t>(y, x);
                uint8_t iv = captures[invertIdx].at<uint8_t>(y, x);
                if (nv > iv) {
                    grayCode |= (1 << (colBits - 1 - b));
                }
            }
            decoded.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(grayCode);
        }
    }

    // Verify: decoded column at (x,y) should equal x
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            REQUIRE(decoded.at<int32_t>(y, x) == x);
        }
    }
}

TEST_CASE("Grey code decode recovers row correspondence", "[scanner][decode]") {
    int size = 64;
    auto captures = generateSyntheticCaptures(size, size, size, size,
        [](int y, int x) { return std::make_pair(x, y); });

    int colBits = (int)std::ceil(std::log2((double)size));
    int rowBits = colBits; // Same for square
    int rowStart = 2 + colBits * 2;

    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    cv::Mat decoded = cv::Mat::zeros(size, size, CV_32S);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;

            uint32_t grayCode = 0;
            for (int b = 0; b < rowBits; b++) {
                int normalIdx = rowStart + b * 2;
                int invertIdx = rowStart + b * 2 + 1;
                uint8_t nv = captures[normalIdx].at<uint8_t>(y, x);
                uint8_t iv = captures[invertIdx].at<uint8_t>(y, x);
                if (nv > iv) {
                    grayCode |= (1 << (rowBits - 1 - b));
                }
            }
            decoded.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(grayCode);
        }
    }

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            REQUIRE(decoded.at<int32_t>(y, x) == y);
        }
    }
}

TEST_CASE("Grey code decode with shifted correspondence", "[scanner][decode]") {
    // Camera pixel (x,y) sees projector pixel (x+10, y+5) — a simple offset
    int camSize = 50;
    int projW = 128, projH = 128;
    int offX = 10, offY = 5;

    auto captures = generateSyntheticCaptures(camSize, camSize, projW, projH,
        [offX, offY](int y, int x) { return std::make_pair(x + offX, y + offY); });

    int colBits = (int)std::ceil(std::log2((double)projW));

    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    cv::Mat decoded = cv::Mat::zeros(camSize, camSize, CV_32S);
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;

            uint32_t grayCode = 0;
            for (int b = 0; b < colBits; b++) {
                int normalIdx = 2 + b * 2;
                int invertIdx = 2 + b * 2 + 1;
                uint8_t nv = captures[normalIdx].at<uint8_t>(y, x);
                uint8_t iv = captures[invertIdx].at<uint8_t>(y, x);
                if (nv > iv) {
                    grayCode |= (1 << (colBits - 1 - b));
                }
            }
            decoded.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(grayCode);
        }
    }

    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            REQUIRE(decoded.at<int32_t>(y, x) == x + offX);
        }
    }
}

TEST_CASE("Grey code decode with scaled correspondence", "[scanner][decode]") {
    // Camera is 2x the projector — each camera pixel maps to projector pixel x/2
    int camSize = 64;
    int projW = 32, projH = 32;

    auto captures = generateSyntheticCaptures(camSize, camSize, projW, projH,
        [](int y, int x) { return std::make_pair(x / 2, y / 2); });

    int colBits = (int)std::ceil(std::log2((double)projW));

    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    cv::Mat decoded = cv::Mat::zeros(camSize, camSize, CV_32S);
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;

            uint32_t grayCode = 0;
            for (int b = 0; b < colBits; b++) {
                int normalIdx = 2 + b * 2;
                int invertIdx = 2 + b * 2 + 1;
                uint8_t nv = captures[normalIdx].at<uint8_t>(y, x);
                uint8_t iv = captures[invertIdx].at<uint8_t>(y, x);
                if (nv > iv) {
                    grayCode |= (1 << (colBits - 1 - b));
                }
            }
            decoded.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(grayCode);
        }
    }

    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            REQUIRE(decoded.at<int32_t>(y, x) == x / 2);
        }
    }
}

// ============================================================================
// SceneScanner — normal computation
// ============================================================================

TEST_CASE("Normal computation on a flat plane", "[scanner][normals]") {
    // A flat plane at z=1.0 should have normals pointing straight up: (0, 0, 1)
    int size = 10;
    cv::Mat depth3D(size, size, CV_32FC3);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            depth3D.at<cv::Vec3f>(y, x) = cv::Vec3f((float)x, (float)y, 1.0f);
        }
    }

    // Compute normals the same way SceneScanner does
    cv::Mat normals = cv::Mat::zeros(size, size, CV_32FC3);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            cv::Vec3f center = depth3D.at<cv::Vec3f>(y, x);
            cv::Vec3f right = depth3D.at<cv::Vec3f>(y, x + 1);
            cv::Vec3f down = depth3D.at<cv::Vec3f>(y + 1, x);

            cv::Vec3f dx = right - center;
            cv::Vec3f dy = down - center;

            cv::Vec3f n;
            n[0] = dy[1] * dx[2] - dy[2] * dx[1];
            n[1] = dy[2] * dx[0] - dy[0] * dx[2];
            n[2] = dy[0] * dx[1] - dy[1] * dx[0];

            float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-6f) {
                n /= len;
                normals.at<cv::Vec3f>(y, x) = cv::Vec3f(
                    n[0] * 0.5f + 0.5f,
                    n[1] * 0.5f + 0.5f,
                    n[2] * 0.5f + 0.5f
                );
            }
        }
    }

    // Interior normals should be (0.5, 0.5, 1.0) — mapping (0,0,1) to [0,1]
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            cv::Vec3f n = normals.at<cv::Vec3f>(y, x);
            CHECK_THAT((double)n[0], Catch::Matchers::WithinAbs(0.5, 0.01));
            CHECK_THAT((double)n[1], Catch::Matchers::WithinAbs(0.5, 0.01));
            CHECK_THAT((double)n[2], Catch::Matchers::WithinAbs(1.0, 0.01));
        }
    }
}

TEST_CASE("Normal computation on a tilted plane", "[scanner][normals]") {
    // Plane tilted in X: z = 1.0 + x * 0.5
    int size = 10;
    cv::Mat depth3D(size, size, CV_32FC3);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            depth3D.at<cv::Vec3f>(y, x) = cv::Vec3f((float)x, (float)y, 1.0f + x * 0.5f);
        }
    }

    cv::Mat normals = cv::Mat::zeros(size, size, CV_32FC3);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            cv::Vec3f center = depth3D.at<cv::Vec3f>(y, x);
            cv::Vec3f right = depth3D.at<cv::Vec3f>(y, x + 1);
            cv::Vec3f down = depth3D.at<cv::Vec3f>(y + 1, x);

            cv::Vec3f dx = right - center;
            cv::Vec3f dy = down - center;

            cv::Vec3f n;
            n[0] = dy[1] * dx[2] - dy[2] * dx[1];
            n[1] = dy[2] * dx[0] - dy[0] * dx[2];
            n[2] = dy[0] * dx[1] - dy[1] * dx[0];

            float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-6f) {
                n /= len;
                normals.at<cv::Vec3f>(y, x) = cv::Vec3f(
                    n[0] * 0.5f + 0.5f,
                    n[1] * 0.5f + 0.5f,
                    n[2] * 0.5f + 0.5f
                );
            }
        }
    }

    // All interior normals should be consistent (same tilt everywhere)
    cv::Vec3f refNormal = normals.at<cv::Vec3f>(2, 2);
    REQUIRE(refNormal[2] > 0.5f); // Z component should be positive (mapped)
    REQUIRE(refNormal[2] < 1.0f); // But less than straight up since plane is tilted

    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            cv::Vec3f n = normals.at<cv::Vec3f>(y, x);
            CHECK_THAT((double)n[0], Catch::Matchers::WithinAbs((double)refNormal[0], 0.01));
            CHECK_THAT((double)n[1], Catch::Matchers::WithinAbs((double)refNormal[1], 0.01));
            CHECK_THAT((double)n[2], Catch::Matchers::WithinAbs((double)refNormal[2], 0.01));
        }
    }
}

TEST_CASE("Normal computation skips zero-depth pixels", "[scanner][normals]") {
    int size = 10;
    cv::Mat depth3D = cv::Mat::zeros(size, size, CV_32FC3);

    // Only set a few pixels with valid depth
    depth3D.at<cv::Vec3f>(3, 3) = cv::Vec3f(3, 3, 1);
    depth3D.at<cv::Vec3f>(3, 4) = cv::Vec3f(4, 3, 1);
    depth3D.at<cv::Vec3f>(4, 3) = cv::Vec3f(3, 4, 1);

    cv::Mat normals = cv::Mat::zeros(size, size, CV_32FC3);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            cv::Vec3f center = depth3D.at<cv::Vec3f>(y, x);
            if (center[2] == 0) continue;

            cv::Vec3f right = depth3D.at<cv::Vec3f>(y, x + 1);
            cv::Vec3f down = depth3D.at<cv::Vec3f>(y + 1, x);
            if (right[2] == 0 || down[2] == 0) continue;

            cv::Vec3f dx = right - center;
            cv::Vec3f dy = down - center;

            cv::Vec3f n;
            n[0] = dy[1] * dx[2] - dy[2] * dx[1];
            n[1] = dy[2] * dx[0] - dy[0] * dx[2];
            n[2] = dy[0] * dx[1] - dy[1] * dx[0];

            float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-6f) {
                n /= len;
                normals.at<cv::Vec3f>(y, x) = cv::Vec3f(n[0]*0.5f+0.5f, n[1]*0.5f+0.5f, n[2]*0.5f+0.5f);
            }
        }
    }

    // Only pixel (3,3) should have a valid normal (it has right and down neighbors)
    cv::Vec3f n33 = normals.at<cv::Vec3f>(3, 3);
    CHECK(n33[2] > 0.0f); // Should have a valid normal

    // Pixel (0,0) should be zero (no depth)
    cv::Vec3f n00 = normals.at<cv::Vec3f>(0, 0);
    CHECK(n00 == cv::Vec3f(0, 0, 0));
}

// ============================================================================
// SceneScanner — edge detection
// ============================================================================

TEST_CASE("Edge detection finds depth discontinuities", "[scanner][edges]") {
    int size = 100;

    // Depth with a sharp step in the middle
    cv::Mat depth(size, size, CV_32F, cv::Scalar(0.5f));
    depth(cv::Rect(50, 0, 50, size)) = cv::Scalar(1.0f);

    // Convert to 8-bit for Canny
    cv::Mat depth8;
    depth.convertTo(depth8, CV_8U, 255.0);
    cv::Mat edges;
    cv::Canny(depth8, edges, 50, 150);

    // Should have edges along the column x=50
    int edgeCount = cv::countNonZero(edges);
    CHECK(edgeCount > 0);

    // Check that edges appear near column 50
    int edgesNear50 = 0;
    for (int y = 0; y < size; y++) {
        for (int x = 48; x <= 52; x++) {
            if (edges.at<uint8_t>(y, x) > 0) edgesNear50++;
        }
    }
    CHECK(edgesNear50 > size / 2); // Most rows should have an edge near x=50
}

// ============================================================================
// SceneScanner — calibration serialization
// ============================================================================

TEST_CASE("Calibration save/load roundtrip", "[scanner][calibration]") {
    // We can't call SceneScanner methods directly (they need OpenGL context for textures)
    // but we can test the JSON serialization logic with known matrices

    // Build calibration data
    cv::Mat camMat = (cv::Mat_<double>(3, 3) <<
        1000.0, 0, 960.0,
        0, 1000.0, 540.0,
        0, 0, 1);
    cv::Mat camDist = (cv::Mat_<double>(5, 1) << 0.1, -0.2, 0.001, 0.002, 0.05);
    cv::Mat projMat = (cv::Mat_<double>(3, 3) <<
        1200.0, 0, 960.0,
        0, 1200.0, 540.0,
        0, 0, 1);
    cv::Mat projDist = cv::Mat::zeros(5, 1, CV_64F);
    cv::Mat R = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat T = (cv::Mat_<double>(3, 1) << 200.0, 0, 0);

    // Serialize to JSON (same format as SceneScanner::saveCalibration)
    nlohmann::json j;
    auto matToJson = [](const cv::Mat& mat) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        for (int r = 0; r < mat.rows; r++)
            for (int c = 0; c < mat.cols; c++)
                arr.push_back(mat.at<double>(r, c));
        return arr;
    };

    j["cameraMatrix"] = matToJson(camMat);
    j["cameraMatrix_rows"] = camMat.rows;
    j["cameraMatrix_cols"] = camMat.cols;
    j["cameraDistortion"] = matToJson(camDist);
    j["cameraDistortion_rows"] = camDist.rows;
    j["cameraDistortion_cols"] = camDist.cols;
    j["projectorMatrix"] = matToJson(projMat);
    j["projectorMatrix_rows"] = projMat.rows;
    j["projectorMatrix_cols"] = projMat.cols;
    j["projectorDistortion"] = matToJson(projDist);
    j["projectorDistortion_rows"] = projDist.rows;
    j["projectorDistortion_cols"] = projDist.cols;
    j["R"] = matToJson(R);
    j["T"] = matToJson(T);
    j["reprojectionError"] = 0.42;

    // Write to temp file
    std::string tmpPath = (std::filesystem::temp_directory_path() / "test_calib.json").string();
    {
        std::ofstream f(tmpPath);
        f << j.dump(2);
    }

    // Read back
    nlohmann::json loaded;
    {
        std::ifstream f(tmpPath);
        REQUIRE(f.is_open());
        f >> loaded;
    }

    auto jsonToMat = [](const nlohmann::json& arr, int rows, int cols) -> cv::Mat {
        cv::Mat mat(rows, cols, CV_64F);
        int idx = 0;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                mat.at<double>(r, c) = arr[idx++].get<double>();
        return mat;
    };

    cv::Mat loadedCamMat = jsonToMat(loaded["cameraMatrix"],
        loaded["cameraMatrix_rows"].get<int>(), loaded["cameraMatrix_cols"].get<int>());
    cv::Mat loadedT = jsonToMat(loaded["T"], 3, 1);

    // Verify values match
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            CHECK_THAT(loadedCamMat.at<double>(r, c),
                Catch::Matchers::WithinAbs(camMat.at<double>(r, c), 1e-10));
        }
    }
    CHECK_THAT(loadedT.at<double>(0, 0), Catch::Matchers::WithinAbs(200.0, 1e-10));
    CHECK_THAT(loaded["reprojectionError"].get<double>(), Catch::Matchers::WithinAbs(0.42, 1e-10));

    // Cleanup
    std::filesystem::remove(tmpPath);
}

// ============================================================================
// SceneScanner — state machine
// ============================================================================

TEST_CASE("Scanner starts in Idle state", "[scanner][state]") {
    SceneScanner scanner;
    CHECK(scanner.state() == SceneScanner::State::Idle);
    CHECK_FALSE(scanner.isScanning());
    CHECK_FALSE(scanner.isComplete());
    CHECK_FALSE(scanner.isCalibrated());
    CHECK(scanner.progress() == 0.0f);
}

TEST_CASE("Scanner init sets pattern count", "[scanner][state]") {
    SceneScanner scanner;
    scanner.init(1920, 1080);
    CHECK(scanner.patternCount() == 46);
}

TEST_CASE("Scanner status text reflects state", "[scanner][state]") {
    SceneScanner scanner;
    CHECK(scanner.statusText() == "Idle");
}

// ============================================================================
// Full pipeline integration — synthetic end-to-end
// ============================================================================

TEST_CASE("Full decode pipeline with identity mapping", "[scanner][integration]") {
    // Simulate the full decode pipeline with synthetic data
    int camSize = 32;
    int projW = 32, projH = 32;

    auto captures = generateSyntheticCaptures(camSize, camSize, projW, projH,
        [](int y, int x) { return std::make_pair(x, y); });

    int colBits = (int)std::ceil(std::log2((double)projW));
    int rowBits = (int)std::ceil(std::log2((double)projH));

    // 1. Shadow mask
    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);
    CHECK(cv::countNonZero(mask) == camSize * camSize);

    // 2. Decode columns
    cv::Mat colMap = cv::Mat::zeros(camSize, camSize, CV_32S);
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;
            uint32_t gray = 0;
            for (int b = 0; b < colBits; b++) {
                uint8_t nv = captures[2 + b*2].at<uint8_t>(y, x);
                uint8_t iv = captures[2 + b*2 + 1].at<uint8_t>(y, x);
                if (nv > iv) gray |= (1 << (colBits - 1 - b));
            }
            colMap.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(gray);
        }
    }

    // 3. Decode rows
    int rowStart = 2 + colBits * 2;
    cv::Mat rowMap = cv::Mat::zeros(camSize, camSize, CV_32S);
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;
            uint32_t gray = 0;
            for (int b = 0; b < rowBits; b++) {
                uint8_t nv = captures[rowStart + b*2].at<uint8_t>(y, x);
                uint8_t iv = captures[rowStart + b*2 + 1].at<uint8_t>(y, x);
                if (nv > iv) gray |= (1 << (rowBits - 1 - b));
            }
            rowMap.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(gray);
        }
    }

    // 4. Verify full correspondence
    int correct = 0;
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (colMap.at<int32_t>(y, x) == x && rowMap.at<int32_t>(y, x) == y) {
                correct++;
            }
        }
    }
    CHECK(correct == camSize * camSize);
}

TEST_CASE("Full decode with noisy captures still recovers correctly", "[scanner][integration][noise]") {
    // Add moderate noise to synthetic captures — Grey code should be robust
    int camSize = 32;
    int projW = 32, projH = 32;

    auto captures = generateSyntheticCaptures(camSize, camSize, projW, projH,
        [](int y, int x) { return std::make_pair(x, y); });

    // Add noise: shift values by up to +-30 (but bright=200, dark=10, so still distinguishable)
    cv::RNG rng(42);
    for (auto& cap : captures) {
        cv::Mat noise(cap.size(), CV_8S);
        rng.fill(noise, cv::RNG::NORMAL, 0, 15);
        cv::Mat capSigned;
        cap.convertTo(capSigned, CV_16S);
        cv::Mat noiseSigned;
        noise.convertTo(noiseSigned, CV_16S);
        capSigned += noiseSigned;
        // Clamp to [0, 255]
        cv::Mat clamped;
        capSigned.convertTo(clamped, CV_8U);
        clamped.copyTo(cap);
    }

    int colBits = (int)std::ceil(std::log2((double)projW));
    int rowBits = colBits;

    cv::Mat diff, mask;
    cv::absdiff(captures[0], captures[1], diff);
    cv::threshold(diff, mask, 40, 255, cv::THRESH_BINARY);

    // Most pixels should still be valid
    CHECK(cv::countNonZero(mask) > camSize * camSize * 0.9);

    // Decode columns
    cv::Mat colMap = cv::Mat::zeros(camSize, camSize, CV_32S);
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;
            uint32_t gray = 0;
            for (int b = 0; b < colBits; b++) {
                uint8_t nv = captures[2 + b*2].at<uint8_t>(y, x);
                uint8_t iv = captures[2 + b*2 + 1].at<uint8_t>(y, x);
                if (nv > iv) gray |= (1 << (colBits - 1 - b));
            }
            colMap.at<int32_t>(y, x) = (int32_t)GrayCodePattern::fromGrayCode(gray);
        }
    }

    // With normal+inverted comparison, most pixels should decode correctly
    // despite noise (the gap is 200 vs 10, noise sigma=15)
    int correct = 0;
    int total = 0;
    for (int y = 0; y < camSize; y++) {
        for (int x = 0; x < camSize; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;
            total++;
            if (colMap.at<int32_t>(y, x) == x) correct++;
        }
    }
    float accuracy = (float)correct / total;
    CHECK(accuracy > 0.95f); // At least 95% accuracy with moderate noise
}

// ============================================================================
// Distance transform for edge field
// ============================================================================

TEST_CASE("Distance transform produces gradient from edges", "[scanner][edges]") {
    int size = 100;
    cv::Mat edges = cv::Mat::zeros(size, size, CV_8U);

    // Single vertical edge at x=50
    for (int y = 0; y < size; y++) {
        edges.at<uint8_t>(y, 50) = 255;
    }

    cv::Mat invEdges;
    cv::bitwise_not(edges, invEdges);
    cv::Mat dist;
    cv::distanceTransform(invEdges, dist, cv::DIST_L2, 3);

    // Distance should be 0 at the edge and increase away from it
    CHECK_THAT((double)dist.at<float>(50, 50), Catch::Matchers::WithinAbs(0.0, 0.5));
    CHECK(dist.at<float>(50, 0) > 10.0f);   // Far from edge
    CHECK(dist.at<float>(50, 99) > 10.0f);   // Far from edge

    // Distance should increase monotonically away from edge
    CHECK(dist.at<float>(50, 40) < dist.at<float>(50, 30));
    CHECK(dist.at<float>(50, 60) < dist.at<float>(50, 70));
}
