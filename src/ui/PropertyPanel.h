#pragma once
#include "compositing/Layer.h"
#include <memory>
#include <string>

class ShaderSource;

// Speech-to-text state shared between PropertyPanel and Application
struct SpeechState {
    bool available = false;   // true if HAS_WEBVIEW2
    bool listening = false;   // true while recognition is active
    ShaderSource* targetSource = nullptr;
    std::string targetParam;
};

class PropertyPanel {
public:
    void render(std::shared_ptr<Layer> layer, bool& maskEditMode, SpeechState* speech = nullptr);

    // Set to true when a property widget is first activated (signals Application to push undo state)
    bool undoNeeded = false;
};
