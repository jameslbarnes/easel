#include "ui/PropertyPanel.h"
#include "compositing/BlendMode.h"
#include "sources/ShaderSource.h"
#include "sources/VideoSource.h"
#ifdef HAS_WHISPER
#include "speech/WhisperSpeech.h"
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>

// --- Theme ---
static const ImVec4 kDimText   = ImVec4(0.45f, 0.50f, 0.58f, 1.0f);
static const ImVec4 kMuted     = ImVec4(0.35f, 0.40f, 0.48f, 1.0f);
static const ImU32  kSepColor  = IM_COL32(255, 255, 255, 12);

static void thinSep() {
    ImGui::Dummy(ImVec2(0, 4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    draw->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), kSepColor);
    ImGui::Dummy(ImVec2(0, 4));
}

static bool accentBtn(const char* label, float w = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
    bool c = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleColor(4);
    return c;
}

// --- Two-column labeled drag helpers ---
// Draws: [Label: value] using format string as the label inside the widget

// Single drag with embedded label: "X  0.01"
static bool namedDrag(const char* id, const char* prefix, float* v, float speed, float lo, float hi, const char* fmt = "%.2f") {
    char fullFmt[64];
    snprintf(fullFmt, sizeof(fullFmt), "%s  %s", prefix, fmt);
    return ImGui::DragFloat(id, v, speed, lo, hi, fullFmt);
}

// Two drags side by side with embedded labels
static bool dragPair(const char* idA, const char* labelA, float* a,
                     const char* idB, const char* labelB, float* b,
                     float speed, float lo, float hi, const char* fmt = "%.2f") {
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (namedDrag(idA, labelA, a, speed, lo, hi, fmt)) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w);
    if (namedDrag(idB, labelB, b, speed, lo, hi, fmt)) changed = true;
    return changed;
}

void PropertyPanel::render(std::shared_ptr<Layer> layer, bool& maskEditMode,
                           SpeechState* speech, MosaicAudioState* mosaicAudio,
                           float appTime) {
    ImGui::Begin("Properties");

    if (!layer) {
        ImGui::TextDisabled("No layer selected");
        ImGui::End();
        return;
    }

    undoNeeded = false;

    // --- Header ---
    ImGui::SetNextItemWidth(-1);
    char nameBuf[256];
    strncpy(nameBuf, layer->name.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    if (ImGui::InputText("##Name", nameBuf, sizeof(nameBuf))) {
        layer->name = nameBuf;
    }
    if (ImGui::IsItemActivated()) undoNeeded = true;

    if (layer->source) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::Text("%dx%d", layer->source->width(), layer->source->height());
        ImGui::PopStyleColor();
    }

    thinSep();

    // --- Blend + Opacity ---
    {
        float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(w);
        const char* currentBlend = blendModeName(layer->blendMode);
        if (ImGui::BeginCombo("##Blend", currentBlend)) {
            for (int i = 0; i < (int)BlendMode::COUNT; i++) {
                BlendMode mode = (BlendMode)i;
                bool selected = (layer->blendMode == mode);
                if (ImGui::Selectable(blendModeName(mode), selected)) {
                    undoNeeded = true;
                    layer->blendMode = mode;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        int opacityPct = (int)(layer->opacity * 100.0f + 0.5f);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("##Opacity", &opacityPct, 0, 100, "%d%%")) {
            layer->opacity = opacityPct / 100.0f;
        }
        if (ImGui::IsItemActivated()) undoNeeded = true;
    }

    thinSep();

    // --- Transform (always visible, compact two-column grid) ---
    // Row 1: X / Y
    if (dragPair("##PosX", "X", &layer->position.x, "##PosY", "Y", &layer->position.y,
                 0.01f, -2.0f, 2.0f))
    {}
    if (ImGui::IsItemActivated()) undoNeeded = true;

    // Row 2: Size / Rotation
    {
        float uniformScale = (layer->scale.x + layer->scale.y) * 0.5f;
        float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(w);
        if (namedDrag("##Size", "Size", &uniformScale, 0.01f, 0.01f, 10.0f)) {
            float ratio = (layer->scale.x > 0.001f) ? layer->scale.y / layer->scale.x : 1.0f;
            layer->scale.x = uniformScale;
            layer->scale.y = uniformScale * ratio;
        }
        if (ImGui::IsItemActivated()) undoNeeded = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        namedDrag("##Rot", "Rot", &layer->rotation, 1.0f, -360.0f, 360.0f, "%.1f");
        if (ImGui::IsItemActivated()) undoNeeded = true;
    }

    // Row 3: W / H
    if (dragPair("##ScaleX", "W", &layer->scale.x, "##ScaleY", "H", &layer->scale.y,
                 0.01f, 0.01f, 10.0f))
    {}
    if (ImGui::IsItemActivated()) undoNeeded = true;

    // Flip toggles
    if (ImGui::Checkbox("Flip H", &layer->flipH)) undoNeeded = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Flip V", &layer->flipV)) undoNeeded = true;
    ImGui::SameLine();
    if (accentBtn("Reset")) {
        undoNeeded = true;
        layer->position = {0.0f, 0.0f};
        layer->scale = {1.0f, 1.0f};
        layer->rotation = 0.0f;
        layer->flipH = false;
        layer->flipV = false;
        layer->mosaicModeFrom = layer->mosaicMode;
        layer->mosaicTransitionStart = appTime;
        layer->mosaicMode = MosaicMode::Mirror;
        layer->tileX = layer->tileY = 1.0f;
        layer->mosaicDensity = 4.0f;
        layer->mosaicSpin = 0.0f;
        layer->audioReactive = false;
        layer->audioStrength = 0.15f;
        layer->cropTop = layer->cropBottom = layer->cropLeft = layer->cropRight = 0.0f;
    }

    // --- Mosaic mode ---
    {
        float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        // Mode dropdown
        ImGui::SetNextItemWidth(halfW);
        const char* currentMode = mosaicModeName(layer->mosaicMode);
        if (ImGui::BeginCombo("##MosaicMode", currentMode)) {
            for (int i = 0; i < (int)MosaicMode::COUNT; i++) {
                MosaicMode mode = (MosaicMode)i;
                bool selected = (layer->mosaicMode == mode);
                if (ImGui::Selectable(mosaicModeName(mode), selected)) {
                    undoNeeded = true;
                    layer->mosaicModeFrom = layer->mosaicMode;
                    layer->mosaicTransitionStart = appTime;
                    layer->mosaicMode = mode;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Audio toggle
        ImGui::SameLine();
        if (layer->audioReactive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("~ Audio", ImVec2(halfW, 0))) {
                layer->audioReactive = false;
                undoNeeded = true;
            }
            ImGui::PopStyleColor(4);
        } else {
            if (ImGui::Button("~ Audio", ImVec2(halfW, 0))) {
                layer->audioReactive = true;
                undoNeeded = true;
            }
        }

        // Audio strength slider + source selector (only when active)
        if (layer->audioReactive) {
            ImGui::SetNextItemWidth(-1);
            if (namedDrag("##AudioStr", "Strength", &layer->audioStrength, 0.005f, 0.0f, 1.0f)) {}
            if (ImGui::IsItemActivated()) undoNeeded = true;

            // Audio source dropdown
            if (mosaicAudio && mosaicAudio->selectedDevice) {
                const char* srcLabel = "System Audio";
                int sel = *mosaicAudio->selectedDevice;
                if (sel >= 0 && sel < (int)mosaicAudio->devices.size()) {
                    srcLabel = mosaicAudio->devices[sel].name.c_str();
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##AudioSrc", srcLabel)) {
                    if (ImGui::Selectable("System Audio", sel == -1)) {
                        *mosaicAudio->selectedDevice = -1;
                    }
                    for (int i = 0; i < (int)mosaicAudio->devices.size(); i++) {
                        auto& d = mosaicAudio->devices[i];
                        char label[256];
                        snprintf(label, sizeof(label), "%s%s", d.name.c_str(),
                                 d.isMic ? "  (mic)" : "");
                        if (ImGui::Selectable(label, sel == i)) {
                            *mosaicAudio->selectedDevice = i;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }

        // Mode-specific controls
        switch (layer->mosaicMode) {
            case MosaicMode::Mirror: {
                // 8x8 clickable tile grid
                const int maxTile = 8;
                float avail = ImGui::GetContentRegionAvail().x;
                float cellSize = avail / (float)maxTile;
                if (cellSize > 24.0f) cellSize = 24.0f;
                float gridW = cellSize * maxTile;
                float gridH = cellSize * maxTile;
                int itx = (int)(layer->tileX + 0.5f);
                int ity = (int)(layer->tileY + 0.5f);

                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::Text("Tile  %dx%d", itx, ity);
                ImGui::PopStyleColor();

                ImVec2 origin = ImGui::GetCursorScreenPos();
                float indent = (avail - gridW) * 0.5f;
                if (indent > 0) origin.x += indent;

                ImDrawList* draw = ImGui::GetWindowDrawList();
                ImGui::InvisibleButton("##TileGrid", ImVec2(avail, gridH));

                for (int gy = 0; gy < maxTile; gy++) {
                    for (int gx = 0; gx < maxTile; gx++) {
                        ImVec2 cMin(origin.x + gx * cellSize, origin.y + gy * cellSize);
                        ImVec2 cMax(cMin.x + cellSize - 1.0f, cMin.y + cellSize - 1.0f);
                        bool active = (gx < itx && gy < ity);
                        bool hovered = false;
                        ImVec2 mouse = ImGui::GetIO().MousePos;
                        if (mouse.x >= cMin.x && mouse.x < cMax.x &&
                            mouse.y >= cMin.y && mouse.y < cMax.y) {
                            hovered = true;
                        }
                        if (active) {
                            draw->AddRectFilled(cMin, cMax, IM_COL32(0, 180, 235, 90), 2.0f);
                            draw->AddRect(cMin, cMax, IM_COL32(0, 200, 255, 60), 2.0f);
                        } else if (hovered) {
                            draw->AddRectFilled(cMin, cMax, IM_COL32(0, 180, 235, 35), 2.0f);
                            draw->AddRect(cMin, cMax, IM_COL32(255, 255, 255, 15), 2.0f);
                        } else {
                            draw->AddRectFilled(cMin, cMax, IM_COL32(255, 255, 255, 6), 2.0f);
                            draw->AddRect(cMin, cMax, IM_COL32(255, 255, 255, 10), 2.0f);
                        }
                    }
                }

                if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
                    ImVec2 mouse = ImGui::GetIO().MousePos;
                    int clickX = (int)((mouse.x - origin.x) / cellSize) + 1;
                    int clickY = (int)((mouse.y - origin.y) / cellSize) + 1;
                    if (clickX >= 1 && clickX <= maxTile && clickY >= 1 && clickY <= maxTile) {
                        if (clickX != itx || clickY != ity) {
                            layer->tileX = (float)clickX;
                            layer->tileY = (float)clickY;
                            undoNeeded = true;
                        }
                    }
                }
                break;
            }
            case MosaicMode::Hex: {
                float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                ImGui::SetNextItemWidth(w);
                if (namedDrag("##Density", "Cells", &layer->mosaicDensity, 0.1f, 1.0f, 20.0f, "%.1f")) {}
                if (ImGui::IsItemActivated()) undoNeeded = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(w);
                if (namedDrag("##Spin", "Spin", &layer->mosaicSpin, 1.0f, -360.0f, 360.0f, "%.1f")) {}
                if (ImGui::IsItemActivated()) undoNeeded = true;
                break;
            }
            default: break;
        }
    }

    // --- Crop ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.04f));
    if (ImGui::TreeNode("Crop")) {
        if (ImGui::Checkbox("Auto-trim black borders", &layer->autoCrop)) {
            if (layer->autoCrop) {
                // Re-run detection
                layer->autoCropDone = false;
            } else {
                // Clear crop when disabling
                layer->cropTop = layer->cropBottom = layer->cropLeft = layer->cropRight = 0.0f;
            }
            undoNeeded = true;
        }
        if (dragPair("##CropT", "Top", &layer->cropTop, "##CropB", "Btm", &layer->cropBottom,
                     0.005f, 0.0f, 0.49f, "%.3f"))
            undoNeeded = true;
        if (dragPair("##CropL", "Left", &layer->cropLeft, "##CropR", "Right", &layer->cropRight,
                     0.005f, 0.0f, 0.49f, "%.3f"))
            undoNeeded = true;
        ImGui::TreePop();
    }
    ImGui::PopStyleColor(2);

    thinSep();

    // --- Mask ---
    if (ImGui::Checkbox("Mask", &layer->maskEnabled)) undoNeeded = true;

    if (layer->maskEnabled) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::Text("(%d pts)", layer->maskPath.count());
        ImGui::PopStyleColor();

        if (maskEditMode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.60f));
            if (ImGui::Button("Done Editing", ImVec2(-1, 0))) {
                maskEditMode = false;
            }
            ImGui::PopStyleColor(3);
        } else {
            if (accentBtn("Edit Mask", -1)) {
                maskEditMode = true;
            }
        }

        if (layer->maskPath.count() > 0) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.15f, 0.15f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.2f, 0.2f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            if (ImGui::Button("Clear Mask", ImVec2(-1, 0))) {
                layer->maskPath.points().clear();
                layer->maskPath.markDirty();
                maskEditMode = false;
            }
            ImGui::PopStyleColor(4);
        }

        float shapeW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
        if (accentBtn("Rect", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeRectangle({0.5f, 0.5f}, {0.6f, 0.6f});
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Ellipse", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeEllipse({0.5f, 0.5f}, {0.6f, 0.6f});
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Tri", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeTriangle({0.5f, 0.5f}, 0.3f);
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Star", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeStar({0.5f, 0.5f}, 0.3f, 0.15f, 5);
            maskEditMode = true;
        }

        if (maskEditMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
            ImGui::TextWrapped("Click: add  |  Drag: move  |  Handles: curve  |  R-click: del");
            ImGui::PopStyleColor();
        }
    } else {
        maskEditMode = false;
    }

    // --- Video controls ---
    if (layer->source && layer->source->isVideo()) {
        thinSep();
        if (layer->source->isPlaying()) {
            if (accentBtn("Pause", -1)) layer->source->pause();
        } else {
            if (accentBtn("Play", -1)) layer->source->play();
        }
        ImGui::SetNextItemWidth(-1);
        float t = (float)layer->source->currentTime();
        float dur = (float)layer->source->duration();
        if (ImGui::SliderFloat("##Time", &t, 0.0f, dur, "%.1fs")) {
            layer->source->seek(t);
        }
        auto* vidSrc = static_cast<VideoSource*>(layer->source.get());
        if (vidSrc->hasAudio()) {
            float vol = vidSrc->volume();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##Volume", &vol, 0.0f, 1.0f, "Vol %.0f%%")) {
                vidSrc->setVolume(vol);
            }
        }
    }

    // --- Shader (ISF) controls ---
    if (layer->source && layer->source->isShader()) {
        auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
        auto& inputs = shaderSrc->inputs();

        if (!inputs.empty()) {
            thinSep();

            for (int i = 0; i < (int)inputs.size(); i++) {
                auto& input = inputs[i];
                ImGui::PushID(i + 10000);

                if (input.type == "float") {
                    float v = std::get<float>(input.value);
                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##val", &v, input.minVal, input.maxVal)) {
                        input.value = v;
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;
                } else if (input.type == "color") {
                    glm::vec4 c = std::get<glm::vec4>(input.value);
                    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::ColorEdit4("##val", &c[0], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                        input.value = c;
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;
                } else if (input.type == "bool") {
                    bool b = std::get<bool>(input.value);
                    if (ImGui::Checkbox(input.name.c_str(), &b)) {
                        input.value = b;
                        undoNeeded = true;
                    }
                } else if (input.type == "point2D") {
                    glm::vec2 p = std::get<glm::vec2>(input.value);
                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat2("##val", &p[0], input.minVec.x, input.maxVec.x)) {
                        input.value = p;
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;
                } else if (input.type == "long") {
                    float v = std::get<float>(input.value);
                    int iv = (int)v;
                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##val", &iv, (int)input.minVal, (int)input.maxVal)) {
                        input.value = (float)iv;
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;
                } else if (input.type == "text") {
                    std::string text = std::get<std::string>(input.value);
                    int maxLen = (int)input.maxVal;
                    if (maxLen <= 0) maxLen = 12;

                    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();

                    char textBuf[256] = {};
                    strncpy(textBuf, text.c_str(), sizeof(textBuf) - 1);

                    float micW = (speech && speech->available) ? 30.0f : 0.0f;
                    ImGui::SetNextItemWidth(-(1.0f + micW));
                    if (ImGui::InputText("##val", textBuf, (size_t)maxLen + 1,
                                         ImGuiInputTextFlags_CharsUppercase)) {
                        input.value = std::string(textBuf);
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;

                    if (speech && speech->available) {
                        ImGui::SameLine();
                        bool isTarget = speech->listening &&
                                        speech->targetSource == shaderSrc &&
                                        speech->targetParam == input.name;
                        if (isTarget) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 0.35f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.15f, 0.15f, 0.50f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.2f, 0.2f, 0.65f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                            if (ImGui::Button("STOP", ImVec2(micW, 0))) {
                                speech->listening = false;
                                speech->targetSource = nullptr;
                                speech->targetParam.clear();
                            }
                            ImGui::PopStyleColor(4);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                            if (ImGui::Button("MIC", ImVec2(micW, 0))) {
                                speech->listening = true;
                                speech->targetSource = shaderSrc;
                                speech->targetParam = input.name;
                            }
                            ImGui::PopStyleColor(4);
                        }
                    }
                }

                ImGui::PopID();
            }
        }

#ifdef HAS_WHISPER
        if (speech && speech->available && speech->whisper) {
            auto& devices = speech->whisper->captureDevices();
            if (!devices.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                ImGui::Text("Mic");
                ImGui::PopStyleColor();
                int sel = speech->whisper->selectedDevice();
                std::string preview = (sel < 0) ? "Default" :
                    (sel < (int)devices.size() ? devices[sel].name : "Unknown");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##mic_device", preview.c_str())) {
                    if (ImGui::Selectable("Default", sel < 0)) {
                        if (!speech->listening) speech->whisper->selectDevice(-1);
                    }
                    for (auto& d : devices) {
                        bool isSel = (d.index == sel);
                        std::string lbl = d.name + (d.isDefault ? " *" : "");
                        if (ImGui::Selectable(lbl.c_str(), isSel)) {
                            if (!speech->listening) speech->whisper->selectDevice(d.index);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
#endif

        if (!shaderSrc->description().empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
            ImGui::TextWrapped("%s", shaderSrc->description().c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}
