#ifdef HAS_OPENCV
#include "scanning/WebcamSource.h"
#include <opencv2/imgproc.hpp>
#include <iostream>

WebcamSource::~WebcamSource() {
    close();
}

bool WebcamSource::open(int cameraIndex) {
    close();

    if (!m_capture.open(cameraIndex, cv::CAP_ANY)) {
        std::cerr << "WebcamSource: failed to open camera " << cameraIndex << std::endl;
        return false;
    }

    // Try to set a reasonable resolution
    m_capture.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    m_capture.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

    m_camWidth = (int)m_capture.get(cv::CAP_PROP_FRAME_WIDTH);
    m_camHeight = (int)m_capture.get(cv::CAP_PROP_FRAME_HEIGHT);

    m_rgbaBuffer.resize(m_camWidth * m_camHeight * 4);
    m_texture.createEmpty(m_camWidth, m_camHeight);
    m_open = true;

    std::cout << "WebcamSource: opened camera " << cameraIndex
              << " (" << m_camWidth << "x" << m_camHeight << ")" << std::endl;
    return true;
}

void WebcamSource::close() {
    if (m_capture.isOpened()) {
        m_capture.release();
    }
    m_open = false;
}

bool WebcamSource::isOpen() const {
    return m_open && m_capture.isOpened();
}

void WebcamSource::update() {
    if (!isOpen()) return;

    cv::Mat frame;
    if (!m_capture.read(frame) || frame.empty()) return;

    // BGR -> RGBA
    cv::Mat rgba;
    cv::cvtColor(frame, rgba, cv::COLOR_BGR2RGBA);

    // Flip vertically for OpenGL (bottom-up)
    cv::flip(rgba, rgba, 0);

    m_texture.updateData(rgba.data, rgba.cols, rgba.rows, GL_RGBA, GL_UNSIGNED_BYTE);
}

cv::Mat WebcamSource::captureFrame() {
    if (!isOpen()) return cv::Mat();

    cv::Mat frame;
    m_capture.read(frame);
    return frame; // BGR
}

#endif // HAS_OPENCV
