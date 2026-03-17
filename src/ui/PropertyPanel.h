#pragma once
#include "compositing/Layer.h"
#include <memory>
#include <string>
#include <vector>

class ShaderSource;
class WhisperSpeech;

// Speech-to-text state shared between PropertyPanel and Application
struct SpeechState {
    bool available = false;
    bool listening = false;
    ShaderSource* targetSource = nullptr;
    std::string targetParam;
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
};

class PropertyPanel {
public:
    void render(std::shared_ptr<Layer> layer, bool& maskEditMode,
                SpeechState* speech = nullptr, MosaicAudioState* mosaicAudio = nullptr,
                float appTime = 0.0f);

    // Set to true when a property widget is first activated (signals Application to push undo state)
    bool undoNeeded = false;
};
