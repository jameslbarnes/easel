#include "ui/PropertyPanel.h"
#include "compositing/BlendMode.h"
#include "sources/ShaderSource.h"
#ifdef HAS_WHISPER
#include "speech/WhisperSpeech.h"
#endif
#include <imgui.h>
#include <cstdio>

static const ImU32 kSectionLine = IM_COL32(0, 200, 255, 30);
static const ImVec4 kAccentText = ImVec4(0.0f, 0.78f, 1.0f, 0.7f);
static const ImVec4 kDimText    = ImVec4(0.45f, 0.50f, 0.58f, 1.0f);

static void sectionSep() {
    ImGui::Dummy(ImVec2(0, 2));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    draw->AddLine(pos, ImVec2(pos.x + width, pos.y), kSectionLine);
    ImGui::Dummy(ImVec2(0, 3));
}

static bool accentBtn(const char* label, float w = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
    bool c = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleColor(4);
    return c;
}

// Labeled drag float with inline label
static bool labeledDrag(const char* label, const char* id, float* v, float speed, float lo, float hi) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("%s", label);
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    return ImGui::DragFloat(id, v, speed, lo, hi, "%.2f");
}

void PropertyPanel::render(std::shared_ptr<Layer> layer, bool& maskEditMode, SpeechState* speech) {
    ImGui::Begin("Properties");

    if (!layer) {
        ImGui::TextDisabled("Select a layer");
        ImGui::End();
        return;
    }

    // Reset undo flag each frame
    undoNeeded = false;

    // Name
    ImGui::SetNextItemWidth(-1);
    char nameBuf[256];
    strncpy(nameBuf, layer->name.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    if (ImGui::InputText("##Name", nameBuf, sizeof(nameBuf))) {
        layer->name = nameBuf;
    }
    if (ImGui::IsItemActivated()) undoNeeded = true;

    // Source info (compact)
    if (layer->source) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::Text("%s  %dx%d", layer->source->typeName().c_str(),
                    layer->source->width(), layer->source->height());
        ImGui::PopStyleColor();
    }

    sectionSep();

    // Blend mode + opacity on one conceptual block
    ImGui::SetNextItemWidth(-1);
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

    // Opacity - fixed: display as 0-100%
    int opacityPct = (int)(layer->opacity * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##Opacity", &opacityPct, 0, 100, "Opacity %d%%")) {
        layer->opacity = opacityPct / 100.0f;
    }
    if (ImGui::IsItemActivated()) undoNeeded = true;

    sectionSep();

    // Transform - compact labeled rows
    labeledDrag("X", "##PosX", &layer->position.x, 0.01f, -2.0f, 2.0f);
    if (ImGui::IsItemActivated()) undoNeeded = true;
    labeledDrag("Y", "##PosY", &layer->position.y, 0.01f, -2.0f, 2.0f);
    if (ImGui::IsItemActivated()) undoNeeded = true;

    // Uniform size slider (drags both W and H together)
    float uniformScale = (layer->scale.x + layer->scale.y) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("Size");
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##Size", &uniformScale, 0.01f, 0.01f, 10.0f, "%.2f")) {
        float ratio = (layer->scale.x > 0.001f) ? layer->scale.y / layer->scale.x : 1.0f;
        layer->scale.x = uniformScale;
        layer->scale.y = uniformScale * ratio;
    }
    if (ImGui::IsItemActivated()) undoNeeded = true;

    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("W");
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##ScaleX", &layer->scale.x, 0.01f, 0.01f, 10.0f, "%.2f");
    if (ImGui::IsItemActivated()) undoNeeded = true;

    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("H");
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##ScaleY", &layer->scale.y, 0.01f, 0.01f, 10.0f, "%.2f");
    if (ImGui::IsItemActivated()) undoNeeded = true;

    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("Rot");
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##Rot", &layer->rotation, 1.0f, -360.0f, 360.0f, "%.1f");
    if (ImGui::IsItemActivated()) undoNeeded = true;

    // Flip toggles
    if (ImGui::Checkbox("Flip H", &layer->flipH)) undoNeeded = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Flip V", &layer->flipV)) undoNeeded = true;

    // Tile (mirror repeat)
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("Tile");
    ImGui::PopStyleColor();
    ImGui::SameLine(38);
    ImGui::SetNextItemWidth(-1);
    int tile[2] = { layer->tileX, layer->tileY };
    if (ImGui::SliderInt2("##Tile", tile, 1, 8, "%d")) {
        layer->tileX = tile[0];
        layer->tileY = tile[1];
        undoNeeded = true;
    }

    // Source crop
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::Text("Crop");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##CropT", &layer->cropTop, 0.005f, 0.0f, 0.49f, "Top %.3f"))
        undoNeeded = true;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##CropB", &layer->cropBottom, 0.005f, 0.0f, 0.49f, "Bottom %.3f"))
        undoNeeded = true;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##CropL", &layer->cropLeft, 0.005f, 0.0f, 0.49f, "Left %.3f"))
        undoNeeded = true;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##CropR", &layer->cropRight, 0.005f, 0.0f, 0.49f, "Right %.3f"))
        undoNeeded = true;

    if (accentBtn("Reset", -1)) {
        undoNeeded = true;
        layer->position = {0.0f, 0.0f};
        layer->scale = {1.0f, 1.0f};
        layer->rotation = 0.0f;
        layer->flipH = false;
        layer->flipV = false;
        layer->tileX = layer->tileY = 1;
        layer->cropTop = layer->cropBottom = layer->cropLeft = layer->cropRight = 0.0f;
    }

    sectionSep();

    // Mask - collapsible
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
            if (ImGui::Button("Clear", ImVec2(-1, 0))) {
                layer->maskPath.points().clear();
                layer->maskPath.markDirty();
                maskEditMode = false;
            }
            ImGui::PopStyleColor(4);
        }

        // Shape presets
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::Text("Shapes");
        ImGui::PopStyleColor();
        ImGui::SameLine(58);
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
            ImGui::TextWrapped("Click: add  |  Drag: move\nHandles: curve  |  R-click: del");
            ImGui::PopStyleColor();
        }
    } else {
        maskEditMode = false;
    }

    // Video controls
    if (layer->source && layer->source->isVideo()) {
        sectionSep();
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
    }

    // Shader (ISF) controls
    if (layer->source && layer->source->isShader()) {
        auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
        auto& inputs = shaderSrc->inputs();

        if (!inputs.empty()) {
            sectionSep();

            ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
            ImGui::Text("Shader Parameters");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 2));

            for (int i = 0; i < (int)inputs.size(); i++) {
                auto& input = inputs[i];
                ImGui::PushID(i + 10000);

                if (input.type == "float") {
                    float v = std::get<float>(input.value);
                    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
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
                    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat2("##val", &p[0], input.minVec.x, input.maxVec.x)) {
                        input.value = p;
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;
                } else if (input.type == "long") {
                    float v = std::get<float>(input.value);
                    int iv = (int)v;
                    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();
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

                    // Mic button width if speech is available
                    float micW = (speech && speech->available) ? 30.0f : 0.0f;
                    ImGui::SetNextItemWidth(-(1.0f + micW));
                    if (ImGui::InputText("##val", textBuf, (size_t)maxLen + 1,
                                         ImGuiInputTextFlags_CharsUppercase)) {
                        input.value = std::string(textBuf);
                    }
                    if (ImGui::IsItemActivated()) undoNeeded = true;

                    // Mic toggle button
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
        // Mic device selector
        if (speech && speech->available && speech->whisper) {
            auto& devices = speech->whisper->captureDevices();
            if (!devices.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                ImGui::Text("Mic");
                ImGui::PopStyleColor();
                int sel = speech->whisper->selectedDevice();
                // Build preview string
                std::string preview = (sel < 0) ? "Default" :
                    (sel < (int)devices.size() ? devices[sel].name : "Unknown");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##mic_device", preview.c_str())) {
                    // Default option
                    if (ImGui::Selectable("Default", sel < 0)) {
                        if (!speech->listening) speech->whisper->selectDevice(-1);
                    }
                    for (auto& d : devices) {
                        bool isSel = (d.index == sel);
                        std::string label = d.name + (d.isDefault ? " *" : "");
                        if (ImGui::Selectable(label.c_str(), isSel)) {
                            if (!speech->listening) speech->whisper->selectDevice(d.index);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
#endif

        // Show shader description if present
        if (!shaderSrc->description().empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
            ImGui::TextWrapped("%s", shaderSrc->description().c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}
