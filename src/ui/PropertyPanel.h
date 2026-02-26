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

class PropertyPanel {
public:
    void render(std::shared_ptr<Layer> layer, bool& maskEditMode, SpeechState* speech = nullptr);

    // Set to true when a property widget is first activated (signals Application to push undo state)
    bool undoNeeded = false;
};
