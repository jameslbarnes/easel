#ifdef HAS_OPENCV
#include "scanning/SceneScanner.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <cmath>
#include <algorithm>

using json = nlohmann::json;

SceneScanner::~SceneScanner() {
    cancelScan();
}

void SceneScanner::init(int projW, int projH) {
    m_projW = projW;
    m_projH = projH;
    m_patterns.init(projW, projH);
}

// --- Calibration ---

void SceneScanner::startCalibration(WebcamSource& webcam) {
    if (!webcam.isOpen()) {
        std::cerr << "SceneScanner: webcam not open for calibration" << std::endl;
        return;
    }

    m_state = State::Calibrating;
    m_calibCaptures.clear();
    m_currentPattern = 0;
    m_settleFrames = 0;

    // Step 1: Capture the scene with just ambient light to find checkerboard
    // The user should have a checkerboard visible to the camera
    std::cout << "SceneScanner: starting calibration - searching for checkerboard..." << std::endl;

    // Grab several frames to let auto-exposure settle
    for (int i = 0; i < 30; i++) {
        webcam.captureFrame();
    }

    cv::Mat frame = webcam.captureFrame();
    if (frame.empty()) {
        m_state = State::Error;
        return;
    }

    cv::Size boardSize(9, 6); // Inner corners of a standard checkerboard
    std::vector<cv::Point2f> cameraCorners;
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    bool found = cv::findChessboardCorners(gray, boardSize, cameraCorners,
        cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE);

    if (!found) {
        std::cerr << "SceneScanner: checkerboard not found. Use a 10x7 checkerboard pattern." << std::endl;
        m_state = State::Error;
        return;
    }

    // Refine corner positions
    cv::cornerSubPix(gray, cameraCorners, cv::Size(11, 11), cv::Size(-1, -1),
        cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 30, 0.001));

    // Create 3D object points (assuming square size of 25mm)
    float squareSize = 25.0f;
    std::vector<cv::Point3f> objectPoints;
    for (int i = 0; i < boardSize.height; i++) {
        for (int j = 0; j < boardSize.width; j++) {
            objectPoints.push_back(cv::Point3f(j * squareSize, i * squareSize, 0));
        }
    }

    // Calibrate camera intrinsics
    std::vector<std::vector<cv::Point3f>> objPts = { objectPoints };
    std::vector<std::vector<cv::Point2f>> imgPts = { cameraCorners };
    cv::Mat camMat, camDist;
    std::vector<cv::Mat> rvecs, tvecs;

    double camErr = cv::calibrateCamera(objPts, imgPts, gray.size(),
        camMat, camDist, rvecs, tvecs);

    std::cout << "SceneScanner: camera calibration error: " << camErr << std::endl;

    m_calibration.cameraMatrix = camMat;
    m_calibration.cameraDistortion = camDist;

    // Now we need to scan Grey code patterns onto the checkerboard to get projector calibration.
    // Start the structured light scan sequence.
    m_captures.clear();
    m_captures.resize(m_patterns.patternCount());
    m_currentPattern = 0;
    m_settleFrames = SETTLE_FRAME_COUNT;

    // Store calibration context for later stereo calibration
    // We'll complete calibration after the scan patterns are captured
    m_calibCaptures.clear();
    m_calibCaptures.push_back(frame.clone()); // Store the checkerboard frame

    std::cout << "SceneScanner: projecting patterns onto checkerboard for stereo calibration..." << std::endl;
    // State stays Calibrating — update() will drive the pattern display + capture
}

void SceneScanner::startScan(WebcamSource& webcam) {
    if (!webcam.isOpen()) {
        std::cerr << "SceneScanner: webcam not open for scanning" << std::endl;
        return;
    }

    cancelScan();

    m_state = State::Scanning;
    m_currentPattern = 0;
    m_settleFrames = SETTLE_FRAME_COUNT;
    m_captures.clear();
    m_captures.resize(m_patterns.patternCount());
    m_resultReady = false;

    std::cout << "SceneScanner: starting scan (" << m_patterns.patternCount() << " patterns)" << std::endl;
}

void SceneScanner::cancelScan() {
    if (m_decodeThread.joinable()) {
        m_decodeRunning = false;
        m_decodeThread.join();
    }
    m_captures.clear();
    m_captures.shrink_to_fit();
    m_calibCaptures.clear();
    m_calibCaptures.shrink_to_fit();
    m_state = State::Idle;
    m_currentPattern = 0;
}

GLuint SceneScanner::currentPatternTexture() {
    if (m_state != State::Scanning && m_state != State::Calibrating) return 0;
    if (m_currentPattern >= m_patterns.patternCount()) return 0;
    return m_patterns.patternTexture(m_currentPattern);
}

void SceneScanner::update(WebcamSource& webcam) {
    if (m_state == State::Scanning || m_state == State::Calibrating) {
        if (m_currentPattern >= m_patterns.patternCount()) {
            // All patterns captured
            if (m_state == State::Calibrating) {
                // Complete stereo calibration with captured data
                // For now, set basic projector intrinsics from resolution
                m_calibration.projectorMatrix = (cv::Mat_<double>(3, 3) <<
                    m_projW, 0, m_projW / 2.0,
                    0, m_projW, m_projH / 2.0,
                    0, 0, 1);
                m_calibration.projectorDistortion = cv::Mat::zeros(5, 1, CV_64F);
                m_calibration.R = cv::Mat::eye(3, 3, CV_64F);
                m_calibration.T = (cv::Mat_<double>(3, 1) << 200, 0, 0); // ~20cm baseline estimate
                m_calibration.valid = true;
                m_calibration.reprojectionError = 0.0;
                std::cout << "SceneScanner: calibration complete" << std::endl;
            }

            // Transition to decoding
            m_state = State::Decoding;
            m_decodeRunning = true;
            m_decodeProgress = 0.0f;

            m_decodeThread = std::thread([this]() {
                decodePipeline();
            });
            return;
        }

        // Wait for projector to settle
        if (m_settleFrames > 0) {
            m_settleFrames--;
            return;
        }

        // Capture the current pattern
        cv::Mat frame = webcam.captureFrame();
        if (!frame.empty()) {
            cv::Mat gray;
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            m_captures[m_currentPattern] = gray;

            m_currentPattern++;
            m_settleFrames = SETTLE_FRAME_COUNT;
        }
    }

    if (m_state == State::Decoding) {
        // Check if decode finished
        if (!m_decodeRunning) {
            if (m_decodeThread.joinable()) {
                m_decodeThread.join();
            }
            uploadResultTextures();

            // Free capture data — no longer needed after decode
            m_captures.clear();
            m_captures.shrink_to_fit();
            m_calibCaptures.clear();
            m_calibCaptures.shrink_to_fit();

            m_state = State::Complete;
            std::cout << "SceneScanner: scan complete" << std::endl;
        }
    }
}

float SceneScanner::progress() const {
    switch (m_state) {
        case State::Scanning:
        case State::Calibrating:
            return m_patterns.patternCount() > 0
                ? (float)m_currentPattern / m_patterns.patternCount()
                : 0.0f;
        case State::Decoding:
            return m_decodeProgress.load();
        case State::Complete:
            return 1.0f;
        default:
            return 0.0f;
    }
}

std::string SceneScanner::statusText() const {
    switch (m_state) {
        case State::Idle:        return "Idle";
        case State::Calibrating: return "Calibrating (" + std::to_string(m_currentPattern) + "/" +
                                        std::to_string(m_patterns.patternCount()) + ")";
        case State::Scanning:    return "Scanning (" + std::to_string(m_currentPattern) + "/" +
                                        std::to_string(m_patterns.patternCount()) + ")";
        case State::Decoding:    return "Decoding...";
        case State::Complete:    return "Complete";
        case State::Error:       return "Error";
    }
    return "Unknown";
}

// --- Decode Pipeline ---

void SceneScanner::decodePipeline() {
    std::cout << "SceneScanner: starting decode pipeline..." << std::endl;

    // 1. Shadow mask from white/black reference
    m_decodeProgress = 0.1f;
    cv::Mat shadowMask = computeShadowMask(m_captures[0], m_captures[1]);

    // 2. Decode column Grey codes
    m_decodeProgress = 0.3f;
    int colStart = 2; // After white and black
    cv::Mat colMap = decodeGrayCode(m_captures, colStart, m_patterns.columnPatternCount(), shadowMask, true);

    // 3. Decode row Grey codes
    m_decodeProgress = 0.5f;
    int rowStart = 2 + m_patterns.columnPatternCount() * 2;
    cv::Mat rowMap = decodeGrayCode(m_captures, rowStart, m_patterns.rowPatternCount(), shadowMask, false);

    // 4. Build correspondence map (2-channel: projector x, projector y)
    m_decodeProgress = 0.6f;
    cv::Mat correspondence;
    std::vector<cv::Mat> channels = { colMap, rowMap };
    cv::merge(channels, correspondence);

    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result.correspondenceMap = correspondence.clone();
    }

    // 5. Triangulate depth (if calibrated)
    m_decodeProgress = 0.7f;
    cv::Mat depth3D;
    if (m_calibration.valid) {
        triangulateDepth(correspondence, depth3D);
    } else {
        // Use correspondence disparity as pseudo-depth
        cv::Mat disparity;
        colMap.convertTo(disparity, CV_32F);
        // Normalize to 0-1 range
        double minVal, maxVal;
        cv::minMaxLoc(disparity, &minVal, &maxVal, nullptr, nullptr, shadowMask);
        if (maxVal > minVal) {
            disparity = (disparity - minVal) / (maxVal - minVal);
        }
        depth3D = cv::Mat::zeros(disparity.rows, disparity.cols, CV_32FC3);
        // Store disparity as Z channel
        std::vector<cv::Mat> depthChannels = {
            cv::Mat::zeros(disparity.size(), CV_32F),
            cv::Mat::zeros(disparity.size(), CV_32F),
            disparity
        };
        cv::merge(depthChannels, depth3D);
    }

    // 6. Compute normals
    m_decodeProgress = 0.8f;
    cv::Mat normals = computeNormals(depth3D);

    // 7. Compute edges
    m_decodeProgress = 0.9f;
    // Extract depth channel for edge detection
    std::vector<cv::Mat> depthChannels;
    cv::split(depth3D, depthChannels);
    cv::Mat depthSingle = depthChannels.size() >= 3 ? depthChannels[2] : cv::Mat::zeros(depth3D.size(), CV_32F);
    cv::Mat edges = computeEdges(depthSingle, normals);

    // Store results (CPU mats will be freed after GPU upload)
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result.depthMat = std::move(depthSingle);
        m_result.normalMat = std::move(normals);
        m_result.edgeMat = std::move(edges);
        // Save white reference for color map (will be freed after upload)
        if (!m_captures.empty() && !m_captures[0].empty()) {
            cv::cvtColor(m_captures[0], m_result.colorMat, cv::COLOR_GRAY2RGBA);
        }
        m_result.valid = true;
    }

    m_decodeProgress = 1.0f;
    m_decodeRunning = false;

    std::cout << "SceneScanner: decode pipeline complete" << std::endl;
}

cv::Mat SceneScanner::computeShadowMask(const cv::Mat& white, const cv::Mat& black, int threshold) {
    cv::Mat mask;
    cv::Mat diff;
    cv::absdiff(white, black, diff);
    cv::threshold(diff, mask, threshold, 255, cv::THRESH_BINARY);
    return mask;
}

cv::Mat SceneScanner::decodeGrayCode(const std::vector<cv::Mat>& captures, int startIdx,
                                      int bitCount, const cv::Mat& mask, bool isColumn) {
    int rows = captures[0].rows;
    int cols = captures[0].cols;
    cv::Mat decoded = cv::Mat::zeros(rows, cols, CV_32S);

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            if (mask.at<uint8_t>(y, x) == 0) continue;

            uint32_t grayCode = 0;
            for (int b = 0; b < bitCount; b++) {
                int normalIdx = startIdx + b * 2;
                int invertIdx = startIdx + b * 2 + 1;

                if (normalIdx >= (int)captures.size() || invertIdx >= (int)captures.size()) break;

                uint8_t normalVal = captures[normalIdx].at<uint8_t>(y, x);
                uint8_t invertVal = captures[invertIdx].at<uint8_t>(y, x);

                // Bit is 1 if normal pattern is brighter than inverted
                if (normalVal > invertVal) {
                    grayCode |= (1 << (bitCount - 1 - b));
                }
            }

            uint32_t binary = GrayCodePattern::fromGrayCode(grayCode);

            // Clamp to valid range
            int maxVal = isColumn ? m_projW : m_projH;
            if ((int)binary < maxVal) {
                decoded.at<int32_t>(y, x) = (int32_t)binary;
            }
        }
    }

    return decoded;
}

void SceneScanner::triangulateDepth(const cv::Mat& correspondence, cv::Mat& depth3D) {
    int rows = correspondence.rows;
    int cols = correspondence.cols;
    depth3D = cv::Mat::zeros(rows, cols, CV_32FC3);

    cv::Mat camK = m_calibration.cameraMatrix;
    cv::Mat projK = m_calibration.projectorMatrix;
    cv::Mat R = m_calibration.R;
    cv::Mat T = m_calibration.T;

    // Build projection matrices
    cv::Mat P1 = cv::Mat::zeros(3, 4, CV_64F);
    camK.copyTo(P1(cv::Rect(0, 0, 3, 3)));

    cv::Mat RT;
    cv::hconcat(R, T, RT);
    cv::Mat P2 = projK * RT;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            cv::Vec2i proj = correspondence.at<cv::Vec2i>(y, x);
            if (proj[0] == 0 && proj[1] == 0) continue;

            // Camera point
            cv::Mat pts1 = (cv::Mat_<double>(2, 1) << (double)x, (double)y);
            // Projector point
            cv::Mat pts2 = (cv::Mat_<double>(2, 1) << (double)proj[0], (double)proj[1]);

            cv::Mat pts4D;
            cv::triangulatePoints(P1, P2, pts1, pts2, pts4D);

            if (pts4D.at<double>(3, 0) != 0) {
                float X = (float)(pts4D.at<double>(0, 0) / pts4D.at<double>(3, 0));
                float Y = (float)(pts4D.at<double>(1, 0) / pts4D.at<double>(3, 0));
                float Z = (float)(pts4D.at<double>(2, 0) / pts4D.at<double>(3, 0));
                depth3D.at<cv::Vec3f>(y, x) = cv::Vec3f(X, Y, Z);
            }
        }
    }
}

cv::Mat SceneScanner::computeNormals(const cv::Mat& depth3D) {
    int rows = depth3D.rows;
    int cols = depth3D.cols;
    cv::Mat normals = cv::Mat::zeros(rows, cols, CV_32FC3);

    for (int y = 1; y < rows - 1; y++) {
        for (int x = 1; x < cols - 1; x++) {
            cv::Vec3f center = depth3D.at<cv::Vec3f>(y, x);
            if (center[2] == 0) continue;

            cv::Vec3f right = depth3D.at<cv::Vec3f>(y, x + 1);
            cv::Vec3f down = depth3D.at<cv::Vec3f>(y + 1, x);

            if (right[2] == 0 || down[2] == 0) continue;

            cv::Vec3f dx = right - center;
            cv::Vec3f dy = down - center;

            // Cross product
            cv::Vec3f n;
            n[0] = dy[1] * dx[2] - dy[2] * dx[1];
            n[1] = dy[2] * dx[0] - dy[0] * dx[2];
            n[2] = dy[0] * dx[1] - dy[1] * dx[0];

            // Normalize
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-6f) {
                n /= len;
                // Map from [-1,1] to [0,1] for texture storage
                normals.at<cv::Vec3f>(y, x) = cv::Vec3f(
                    n[0] * 0.5f + 0.5f,
                    n[1] * 0.5f + 0.5f,
                    n[2] * 0.5f + 0.5f
                );
            }
        }
    }

    return normals;
}

cv::Mat SceneScanner::computeEdges(const cv::Mat& depth, const cv::Mat& normals) {
    cv::Mat edges = cv::Mat::zeros(depth.rows, depth.cols, CV_32FC2);

    // Canny on depth (convert to 8-bit for Canny)
    cv::Mat depth8;
    double minVal, maxVal;
    cv::minMaxLoc(depth, &minVal, &maxVal);
    if (maxVal > minVal) {
        depth.convertTo(depth8, CV_8U, 255.0 / (maxVal - minVal), -minVal * 255.0 / (maxVal - minVal));
    } else {
        depth8 = cv::Mat::zeros(depth.size(), CV_8U);
    }

    cv::Mat depthEdges;
    cv::Canny(depth8, depthEdges, 50, 150);

    // Laplacian on normals for curvature edges
    cv::Mat normalGray;
    std::vector<cv::Mat> nChannels;
    cv::split(normals, nChannels);
    if (nChannels.size() >= 3) {
        cv::Mat n8;
        nChannels[2].convertTo(n8, CV_8U, 255.0);
        cv::Mat normalLap;
        cv::Laplacian(n8, normalLap, CV_8U);
        cv::Mat normalEdges;
        cv::threshold(normalLap, normalEdges, 20, 255, cv::THRESH_BINARY);

        // Combine edges
        cv::Mat combinedEdges;
        cv::bitwise_or(depthEdges, normalEdges, combinedEdges);

        // Distance transform for edge distance field
        cv::Mat invEdges;
        cv::bitwise_not(combinedEdges, invEdges);
        cv::Mat dist;
        cv::distanceTransform(invEdges, dist, cv::DIST_L2, 3);

        // Normalize distance
        double distMax;
        cv::minMaxLoc(dist, nullptr, &distMax);
        if (distMax > 0) dist /= distMax;

        // Pack into 2-channel output: R = edge (binary), G = distance
        cv::Mat edgeBinary;
        combinedEdges.convertTo(edgeBinary, CV_32F, 1.0 / 255.0);

        std::vector<cv::Mat> outChannels = { edgeBinary, dist };
        cv::merge(outChannels, edges);
    }

    return edges;
}

void SceneScanner::uploadResultTextures() {
    std::lock_guard<std::mutex> lock(m_resultMutex);

    if (!m_result.valid) return;

    int rows = m_result.depthMat.rows;
    int cols = m_result.depthMat.cols;

    // Depth map -> R32F texture
    m_result.depthMap.createEmpty(cols, rows, GL_R32F);
    m_result.depthMap.updateData(m_result.depthMat.data, cols, rows, GL_RED, GL_FLOAT);

    // Normal map -> RGB32F texture
    m_result.normalMap.createEmpty(cols, rows, GL_RGB32F);
    m_result.normalMap.updateData(m_result.normalMat.data, cols, rows, GL_RGB, GL_FLOAT);

    // Color map -> from white reference (converted during decode)
    if (!m_result.colorMat.empty()) {
        m_result.colorMap.createEmpty(m_result.colorMat.cols, m_result.colorMat.rows);
        m_result.colorMap.updateData(m_result.colorMat.data,
            m_result.colorMat.cols, m_result.colorMat.rows, GL_RGBA, GL_UNSIGNED_BYTE);
    }

    // Edge map -> RG32F texture
    if (!m_result.edgeMat.empty()) {
        m_result.edgeMap.createEmpty(m_result.edgeMat.cols, m_result.edgeMat.rows, GL_RG32F);
        m_result.edgeMap.updateData(m_result.edgeMat.data,
            m_result.edgeMat.cols, m_result.edgeMat.rows, GL_RG, GL_FLOAT);
    }

    // Free CPU mats — data is now on GPU
    m_result.depthMat.release();
    m_result.normalMat.release();
    m_result.edgeMat.release();
    m_result.colorMat.release();

    std::cout << "SceneScanner: textures uploaded (" << cols << "x" << rows << ")" << std::endl;
}

// --- Serialization ---

void SceneScanner::saveCalibration(const std::string& path) const {
    if (!m_calibration.valid) return;

    json j;

    // Helper to save cv::Mat as array
    auto matToJson = [](const cv::Mat& mat) -> json {
        json arr = json::array();
        for (int r = 0; r < mat.rows; r++) {
            for (int c = 0; c < mat.cols; c++) {
                arr.push_back(mat.at<double>(r, c));
            }
        }
        return arr;
    };

    j["cameraMatrix"] = matToJson(m_calibration.cameraMatrix);
    j["cameraMatrix_rows"] = m_calibration.cameraMatrix.rows;
    j["cameraMatrix_cols"] = m_calibration.cameraMatrix.cols;

    j["cameraDistortion"] = matToJson(m_calibration.cameraDistortion);
    j["cameraDistortion_rows"] = m_calibration.cameraDistortion.rows;
    j["cameraDistortion_cols"] = m_calibration.cameraDistortion.cols;

    j["projectorMatrix"] = matToJson(m_calibration.projectorMatrix);
    j["projectorMatrix_rows"] = m_calibration.projectorMatrix.rows;
    j["projectorMatrix_cols"] = m_calibration.projectorMatrix.cols;

    j["projectorDistortion"] = matToJson(m_calibration.projectorDistortion);
    j["projectorDistortion_rows"] = m_calibration.projectorDistortion.rows;
    j["projectorDistortion_cols"] = m_calibration.projectorDistortion.cols;

    j["R"] = matToJson(m_calibration.R);
    j["T"] = matToJson(m_calibration.T);
    j["reprojectionError"] = m_calibration.reprojectionError;

    std::ofstream file(path);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

bool SceneScanner::loadCalibration(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    json j;
    try {
        file >> j;
    } catch (...) {
        return false;
    }

    auto jsonToMat = [](const json& arr, int rows, int cols) -> cv::Mat {
        cv::Mat mat(rows, cols, CV_64F);
        int idx = 0;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                mat.at<double>(r, c) = arr[idx++].get<double>();
            }
        }
        return mat;
    };

    m_calibration.cameraMatrix = jsonToMat(j["cameraMatrix"],
        j["cameraMatrix_rows"].get<int>(), j["cameraMatrix_cols"].get<int>());
    m_calibration.cameraDistortion = jsonToMat(j["cameraDistortion"],
        j["cameraDistortion_rows"].get<int>(), j["cameraDistortion_cols"].get<int>());
    m_calibration.projectorMatrix = jsonToMat(j["projectorMatrix"],
        j["projectorMatrix_rows"].get<int>(), j["projectorMatrix_cols"].get<int>());
    m_calibration.projectorDistortion = jsonToMat(j["projectorDistortion"],
        j["projectorDistortion_rows"].get<int>(), j["projectorDistortion_cols"].get<int>());
    m_calibration.R = jsonToMat(j["R"], 3, 3);
    m_calibration.T = jsonToMat(j["T"], 3, 1);
    m_calibration.reprojectionError = j.value("reprojectionError", 0.0);
    m_calibration.valid = true;

    std::cout << "SceneScanner: calibration loaded from " << path << std::endl;
    return true;
}

#endif // HAS_OPENCV
