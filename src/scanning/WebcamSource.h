#pragma once
#ifdef HAS_OPENCV

#include "sources/ContentSource.h"
#include "render/Texture.h"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <string>
#include <mutex>

class WebcamSource : public ContentSource {
public:
    WebcamSource() = default;
    ~WebcamSource() override;

    bool open(int cameraIndex = 0);
    void close();
    bool isOpen() const;

    // ContentSource interface
    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_texture.width(); }
    int height() const override { return m_texture.height(); }
    std::string typeName() const override { return "Webcam"; }

    // Scanner access — returns raw BGR frame
    cv::Mat captureFrame();

    // Camera properties
    int cameraWidth() const { return m_camWidth; }
    int cameraHeight() const { return m_camHeight; }

private:
    cv::VideoCapture m_capture;
    Texture m_texture;
    std::vector<uint8_t> m_rgbaBuffer;
    int m_camWidth = 0;
    int m_camHeight = 0;
    bool m_open = false;
};

#endif // HAS_OPENCV
