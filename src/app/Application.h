#pragma once
#include "app/ProjectorOutput.h"
#include "app/UndoStack.h"
#include "compositing/CompositeEngine.h"
#include "compositing/LayerStack.h"
#include "compositing/MaskRenderer.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"
#include "render/Framebuffer.h"
#include "render/ShaderProgram.h"
#include "render/Mesh.h"
#include "render/Texture.h"
#include "ui/UIManager.h"
#include "ui/ViewportPanel.h"
#include "ui/LayerPanel.h"
#include "ui/PropertyPanel.h"
#include "ui/WarpEditor.h"

#include "sources/WindowCaptureSource.h"
#include "sources/ShaderSource.h"
#include "sources/ShaderClawBridge.h"

#ifdef HAS_OPENCV
#include "scanning/SceneScanner.h"
#include "scanning/WebcamSource.h"
#include "scanning/ScanPanel.h"
#endif

#ifdef HAS_WEBVIEW2
#include "speech/SpeechBridge.h"
#endif

#ifdef HAS_NDI
#include "sources/NDISource.h"
#include "app/NDIOutput.h"
#endif

#ifdef HAS_FFMPEG
#include "app/RTMPOutput.h"
#include "app/VideoRecorder.h"
#endif

#include <GLFW/glfw3.h>
#include <string>
#include <vector>

class Application {
public:
    bool init();
    void run();
    void shutdown();

private:
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1280, m_windowHeight = 720;

    UIManager m_ui;
    ViewportPanel m_viewportPanel;
    LayerPanel m_layerPanel;
    PropertyPanel m_propertyPanel;
    WarpEditor m_warpEditor;

    CompositeEngine m_compositor;
    LayerStack m_layerStack;
    CornerPinWarp m_cornerPin;
    MeshWarp m_meshWarp;
    ProjectorOutput m_projector;
    MaskRenderer m_maskRenderer;
    UndoStack m_undoStack;

    Framebuffer m_warpFBO;
    Mesh m_quad;
    ShaderProgram m_passthroughShader;
    Texture m_testPattern;

    ViewportPanel::WarpMode m_warpMode = ViewportPanel::WarpMode::CornerPin;
    int m_selectedLayer = -1;
    bool m_projectorAutoConnect = false;
    int m_lastMonitorCount = 0;
    bool m_maskEditMode = false;

    void updateSources();
    void compositeAndWarp();
    void renderUI();
    void renderMenuBar();
    void loadImage(const std::string& path);
    void loadVideo(const std::string& path);
    void addScreenCapture(int monitorIndex);
    void addWindowCapture(HWND hwnd, const std::string& title);
    void loadShader(const std::string& path);

    std::vector<WindowInfo> m_windowList;
    ShaderClawBridge m_shaderClaw;

#ifdef HAS_OPENCV
    SceneScanner m_scanner;
    WebcamSource m_webcam;
    ScanPanel m_scanPanel;
#endif

    SpeechState m_speechState;
#ifdef HAS_WEBVIEW2
    SpeechBridge m_speechBridge;
#endif

#ifdef HAS_NDI
    NDIOutput m_ndiOutput;
    std::vector<NDISenderInfo> m_ndiSources;
    bool m_ndiOutputEnabled = false;
    void addNDISource(const std::string& senderName);
#endif

#ifdef HAS_FFMPEG
    RTMPOutput m_rtmpOutput;
    char m_streamKeyBuf[128] = {};
    int m_streamAspect = 0; // 0=16:9, 1=4:3, 2=16:10, 3=Source
    VideoRecorder m_recorder;
#endif

    // JSON save/load
    void saveProject(const std::string& path);
    void loadProject(const std::string& path);

    // Screenshot
    void captureScreenshot(const std::string& path);
    void captureWindow(const std::string& path);
    void pollScreenshotTrigger();
};
