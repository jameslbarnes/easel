#pragma once
#include "app/OutputZone.h"
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
#include <unordered_map>

#include "sources/WindowCaptureSource.h"
#include "sources/ShaderSource.h"
#include "sources/ShaderClawBridge.h"

#ifdef HAS_OPENCV
#include "scanning/SceneScanner.h"
#include "scanning/WebcamSource.h"
#include "scanning/ScanPanel.h"
#endif

#ifdef HAS_WHISPER
#include "speech/WhisperSpeech.h"
#endif

#include "speech/EthereaClient.h"
#include "app/AudioAnalyzer.h"
#include "app/BPMSync.h"
#include "app/DataBus.h"

#ifdef HAS_NDI
#include "sources/NDISource.h"
#include "app/NDIOutput.h"
#endif

#ifdef HAS_WHEP
#include "sources/WHEPSource.h"
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

    // Output zones (replaces singular compositor/warp/FBO)
    std::vector<std::unique_ptr<OutputZone>> m_zones;
    int m_activeZone = 0;
    uint32_t m_nextLayerId = 1;
    OutputZone& activeZone() {
        if (m_activeZone < 0 || m_activeZone >= (int)m_zones.size())
            m_activeZone = 0;
        return *m_zones[m_activeZone];
    }

    LayerStack m_layerStack;
    std::unordered_map<int, std::unique_ptr<ProjectorOutput>> m_projectors;
    MaskRenderer m_maskRenderer;
    UndoStack m_undoStack;

    Mesh m_quad;
    ShaderProgram m_passthroughShader;
    ShaderProgram m_edgeBlendShader;
    Framebuffer m_edgeBlendFBO;
    Texture m_testPattern;

    int m_selectedLayer = -1;
    AudioAnalyzer m_audioAnalyzer;
    BPMSync m_bpmSync;
    float m_audioRMS = 0; // backward compat: smoothed audio level
    int m_mosaicAudioDevice = -1; // -1 = system loopback, >=0 = index into device list
    bool m_projectorAutoConnect = false;
    int m_lastMonitorCount = 0;
    bool m_maskEditMode = false;

    void updateSources();
    void compositeAndWarp();
    void compositeZone(OutputZone& zone);
    void presentOutputs();
    void renderUI();
    void renderMenuBar();
    void addZone();
    void removeZone(int index);
    void duplicateZone(int index);
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
    EthereaClient m_ethereaClient;
    DataBus m_dataBus;
    std::string m_prevTranscript;       // Last full_transcript for diffing

    // Voice decay: fade text layers after speech stops (matches Shader-Claw 3)
    double m_voiceLastInputTime = 0.0;  // glfwGetTime() of last transcript update
    float m_voiceDecayHold = 2.0f;      // seconds at full opacity after last input
    float m_voiceDecayDuration = 3.0f;  // seconds to fade out after hold
    bool m_voiceDecayEnabled = true;
#ifdef HAS_WHISPER
    WhisperSpeech m_whisperSpeech;
#endif

#ifdef HAS_NDI
    NDIOutput m_ndiOutput;
    NDIFinder m_ndiFinder;
    std::vector<NDISenderInfo> m_ndiSources;
    bool m_ndiOutputEnabled = true;
    void addNDISource(const std::string& senderName);
#endif

#ifdef HAS_WHEP
    void addWHEPSource(const std::string& whepUrl);
    void addScopeRTMP();
#endif

#ifdef HAS_FFMPEG
    RTMPOutput m_rtmpOutput;
    char m_streamKeyBuf[128] = {};
    int m_streamAspect = 0; // 0=16:9, 1=4:3, 2=16:10, 3=Source
    VideoRecorder m_recorder;
    std::vector<RecAudioDevice> m_audioDevices;
    int m_selectedAudioDevice = -1; // -1 = default loopback
    void renderTransportBar();

    // Audio level meter (WASAPI IAudioMeterInformation)
    void* m_audioMeterInfo = nullptr;
    void* m_audioMeterDevice = nullptr;
    int m_meterDeviceIdx = -2; // which device the meter is tracking (-2 = uninitialized)
    float m_audioLevelPeak = 0.0f;
    float m_audioLevelL = 0.0f, m_audioLevelR = 0.0f;
    float m_audioLevelSmooth = 0.0f, m_audioLevelSmoothL = 0.0f, m_audioLevelSmoothR = 0.0f;
    void updateAudioMeter();
    void cleanupAudioMeter();

    // Old mosaic meter removed — replaced by AudioAnalyzer
#endif

    // File drop handling
    std::vector<std::string> m_pendingDrops;
    void handleDroppedFiles();
    static void dropCallback(GLFWwindow* window, int count, const char** paths);

    // JSON save/load
    void saveProject(const std::string& path);
    void loadProject(const std::string& path);

    // Screenshot
    void captureScreenshot(const std::string& path);
    void captureWindow(const std::string& path);
    void pollScreenshotTrigger();
};
