#pragma once
#ifdef HAS_OPENCV

#include "scanning/SceneScanner.h"
#include "scanning/WebcamSource.h"

class ScanPanel {
public:
    ScanPanel() = default;

    void render(SceneScanner& scanner, WebcamSource& webcam);

private:
    int m_selectedCamera = 0;
    bool m_showDepth = false;
    bool m_showNormals = false;
    bool m_showEdges = false;
};

#endif // HAS_OPENCV
