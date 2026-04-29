#pragma once
#include "compositing/Layer.h"
#include <memory>
#include <string>
#include <vector>

class ShaderSource;
class WhisperSpeech;
class DataBus;
class LayerStack;
class BPMSync;
class SceneManager;
class MIDIManager;

// Speech-to-text state shared between PropertyPanel and Application
struct SpeechState {
    bool available = false;
    bool listening = false;
    ShaderSource* targetSource = nullptr;
    std::string targetParam;
    DataBus* dataBus = nullptr;
    uint32_t activeLayerId = 0; // for data bus binding key
    MIDIManager* midi = nullptr; // for MIDI Learn on shader parameter bindings
#ifdef HAS_WHISPER
    WhisperSpeech* whisper = nullptr; // for device selection UI
#endif
};

struct MosaicAudioSource {
    std::string name;
    bool isMic = false;
};

struct MosaicAudioState {
    int* selectedDevice = nullptr;       // -1 = system loopback
    std::vector<MosaicAudioSource> devices;
    float bass = 0, lowMid = 0, highMid = 0, treble = 0;
    float beatDecay = 0;
};

class StageView;

class PropertyPanel {
public:
    void render(std::shared_ptr<Layer> layer, bool& maskEditMode,
                SpeechState* speech = nullptr, MosaicAudioState* mosaicAudio = nullptr,
                float appTime = 0.0f, LayerStack* layerStack = nullptr,
                BPMSync* bpmSync = nullptr, SceneManager* sceneManager = nullptr,
                int* audioDeviceIdx = nullptr, MIDIManager* midi = nullptr);

    // Stage hookups — when set, the panel renders a Stage Setup section
    // (displays / projectors / surfaces) at the top of the Properties
    // window when the workspace mode is Stage.
    void setStageView(StageView* sv) { m_stageView = sv; }
    void setZoneTextures(const std::vector<unsigned int>* z) { m_zoneTexs = z; }

    // Set to true when a property widget is first activated (signals Application to push undo state)
    bool undoNeeded = false;
private:
    StageView* m_stageView = nullptr;
    const std::vector<unsigned int>* m_zoneTexs = nullptr;
};
