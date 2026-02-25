#pragma once
#ifdef HAS_OPENCV

#include "scanning/GrayCodePattern.h"
#include "scanning/WebcamSource.h"
#include "render/Texture.h"

#include <opencv2/core.hpp>
#include <glm/glm.hpp>

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

struct ScanResult {
    Texture colorMap;     // Camera view remapped to projector space
    Texture depthMap;     // Per-pixel depth (R32F)
    Texture normalMap;    // Per-pixel normals (RGB = XYZ mapped 0-1)
    Texture edgeMap;      // Edges + distance-to-edge (RG)

    cv::Mat correspondenceMap; // 2-channel: each camera pixel -> projector (x,y)
    cv::Mat depthMat;          // Raw depth data (freed after GPU upload)
    cv::Mat normalMat;         // Raw normal data (freed after GPU upload)
    cv::Mat edgeMat;           // Raw edge data (freed after GPU upload)
    cv::Mat colorMat;          // White reference frame (freed after GPU upload)

    bool valid = false;
};

struct CalibrationData {
    cv::Mat cameraMatrix;
    cv::Mat cameraDistortion;
    cv::Mat projectorMatrix;
    cv::Mat projectorDistortion;
    cv::Mat R;  // Rotation camera -> projector
    cv::Mat T;  // Translation camera -> projector
    double reprojectionError = 0.0;
    bool valid = false;
};

class SceneScanner {
public:
    enum class State {
        Idle,
        Calibrating,
        Scanning,
        Decoding,
        Complete,
        Error
    };

    SceneScanner() = default;
    ~SceneScanner();

    void init(int projW, int projH);

    // Calibration
    void startCalibration(WebcamSource& webcam);
    bool isCalibrated() const { return m_calibration.valid; }
    const CalibrationData& calibration() const { return m_calibration; }
    double calibrationError() const { return m_calibration.reprojectionError; }

    // Scanning
    void startScan(WebcamSource& webcam);
    void cancelScan();

    // Call each frame during scanning — returns pattern texture to display, or 0 if not scanning
    GLuint currentPatternTexture();

    // Must be called each frame to advance the scan state machine
    void update(WebcamSource& webcam);

    // Results
    State state() const { return m_state; }
    bool isScanning() const { return m_state == State::Scanning; }
    bool isComplete() const { return m_state == State::Complete; }
    float progress() const;
    std::string statusText() const;

    const ScanResult& result() const { return m_result; }
    ScanResult& result() { return m_result; }

    // Pattern info
    int patternCount() const { return m_patterns.patternCount(); }
    int currentPattern() const { return m_currentPattern; }

    // Serialization
    void saveCalibration(const std::string& path) const;
    bool loadCalibration(const std::string& path);

private:
    GrayCodePattern m_patterns;
    State m_state = State::Idle;
    int m_projW = 0, m_projH = 0;

    // Scanning state
    int m_currentPattern = 0;
    int m_settleFrames = 0;       // Frames to wait for projector to settle
    static constexpr int SETTLE_FRAME_COUNT = 2;
    std::vector<cv::Mat> m_captures;

    // Calibration
    CalibrationData m_calibration;
    std::vector<cv::Mat> m_calibCaptures; // Captures during calibration

    // Decode thread
    std::thread m_decodeThread;
    std::atomic<bool> m_decodeRunning{false};
    std::atomic<float> m_decodeProgress{0.0f};
    std::mutex m_resultMutex;
    ScanResult m_result;
    bool m_resultReady = false;

    // Decode pipeline
    void decodePipeline();
    cv::Mat computeShadowMask(const cv::Mat& white, const cv::Mat& black, int threshold = 40);
    cv::Mat decodeGrayCode(const std::vector<cv::Mat>& captures, int startIdx,
                            int bitCount, const cv::Mat& mask, bool isColumn);
    void triangulateDepth(const cv::Mat& correspondence, cv::Mat& depth3D);
    cv::Mat computeNormals(const cv::Mat& depth3D);
    cv::Mat computeEdges(const cv::Mat& depth, const cv::Mat& normals);

    void uploadResultTextures();
};

#endif // HAS_OPENCV
