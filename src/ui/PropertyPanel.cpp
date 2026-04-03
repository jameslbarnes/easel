#include "ui/PropertyPanel.h"
#include "app/BPMSync.h"
#include "app/SceneManager.h"
#include "compositing/BlendMode.h"
#include "compositing/LayerStack.h"
#include "sources/ShaderSource.h"
#include "sources/VideoSource.h"
#include "app/DataBus.h"
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
                           float appTime, LayerStack* layerStack,
                           BPMSync* bpmSync, SceneManager* sceneManager) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(250, 200), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Properties");

    // --- BPM Sync (always visible) ---
    if (bpmSync) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
        bool bpmOpen = ImGui::CollapsingHeader("BPM Sync", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);

        if (bpmOpen) {
            float currentBPM = bpmSync->bpm();

            // BPM value + beat dots on same row
            float w = ImGui::GetContentRegionAvail().x;

            // Beat indicator dots
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float dotY = p.y + 8;
                for (int b = 0; b < 4; b++) {
                    float dotCX = p.x + b * 16.0f;
                    int beatInBar = bpmSync->beatCount() % 4;
                    bool isCurrent = (b == beatInBar) && currentBPM > 0;
                    float pulse = isCurrent ? bpmSync->beatPulse() : 0.0f;
                    float r = 4.0f + pulse * 2.0f;
                    dl->AddCircleFilled(ImVec2(dotCX + 6, dotY), r,
                                        isCurrent ? IM_COL32(0, 220, 255, (int)(140 + pulse * 115))
                                                  : IM_COL32(50, 60, 80, 120));
                }
                // BPM text next to dots
                char bpmBuf[16];
                if (currentBPM > 0) snprintf(bpmBuf, sizeof(bpmBuf), "%.1f BPM", currentBPM);
                else snprintf(bpmBuf, sizeof(bpmBuf), "--- BPM");
                dl->AddText(ImVec2(p.x + 74, p.y + 2),
                            currentBPM > 0 ? IM_COL32(0, 200, 255, 255) : IM_COL32(100, 115, 140, 180),
                            bpmBuf);
                ImGui::Dummy(ImVec2(w, 18));
            }

            // TAP + BPM input + Reset on same row
            {
                float btnW = (w - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
                if (ImGui::Button("TAP", ImVec2(btnW, 0))) {
                    bpmSync->tap();
                }
                ImGui::PopStyleColor(4);

                ImGui::SameLine();
                ImGui::SetNextItemWidth(btnW);
                float bpmVal = currentBPM;
                if (ImGui::DragFloat("##BPMVal", &bpmVal, 0.5f, 0.0f, 300.0f, "%.0f BPM")) {
                    bpmSync->setBPM(bpmVal);
                }

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.05f, 0.05f, 0.15f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.1f, 0.1f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.35f, 0.35f, 0.8f));
                if (ImGui::Button("Reset", ImVec2(btnW, 0))) {
                    bpmSync->setBPM(0);
                    bpmSync->resetPhase();
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Dummy(ImVec2(0, 2));
        }

        thinSep();
    }

    // --- Scenes (always visible) ---
    if (sceneManager && layerStack) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
        bool scenesOpen = ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);

        if (scenesOpen) {
            // Save current state as scene
            if (accentBtn("Save Scene", -1)) {
                char name[32];
                snprintf(name, sizeof(name), "Scene %d", sceneManager->count() + 1);
                sceneManager->saveScene(name, *layerStack);
            }

            // Scene list
            int removeIdx = -1;
            for (int s = 0; s < sceneManager->count(); s++) {
                ImGui::PushID(30000 + s);
                auto& scene = (*sceneManager)[s];

                // Recall button (full width minus delete button)
                float w = ImGui::GetContentRegionAvail().x - 24;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.10f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.35f));
                if (ImGui::Button(scene.name.c_str(), ImVec2(w, 0))) {
                    sceneManager->recallScene(s, *layerStack);
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
                if (ImGui::SmallButton("x")) removeIdx = s;
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
            if (removeIdx >= 0) sceneManager->removeScene(removeIdx);
        }

        thinSep();
    }

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

    // --- Transition ---
    {
        float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(w);
        const char* currentTrans = transitionTypeName(layer->transitionType);
        if (ImGui::BeginCombo("##TransType", currentTrans)) {
            for (int i = 0; i < (int)TransitionType::COUNT; i++) {
                TransitionType t = (TransitionType)i;
                bool selected = (layer->transitionType == t);
                if (ImGui::Selectable(transitionTypeName(t), selected)) {
                    layer->transitionType = t;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        namedDrag("##TransDur", "Dur", &layer->transitionDuration, 0.01f, 0.0f, 5.0f, "%.2fs");
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

    // --- Audio Reactivity Bindings ---
    {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
        bool audioOpen = ImGui::CollapsingHeader("Audio Reactive");
        ImGui::PopStyleColor(3);

        if (audioOpen) {
            static const char* targetNames[] = { "None", "Opacity", "Pos X", "Pos Y", "Scale", "Rotation" };
            static const char* signalNames[] = { "Bass", "Mid", "High", "Beat" };

            if (accentBtn("+ Add Binding", -1)) {
                Layer::AudioBinding ab;
                ab.target = Layer::AudioTarget::Scale;
                ab.signal = 0;
                ab.strength = 0.3f;
                layer->audioBindings.push_back(ab);
            }

            int removeIdx = -1;
            for (int b = 0; b < (int)layer->audioBindings.size(); b++) {
                auto& ab = layer->audioBindings[b];
                ImGui::PushID(40000 + b);

                float w = ImGui::GetContentRegionAvail().x;
                float third = (w - ImGui::GetStyle().ItemSpacing.x * 2 - 20) / 3.0f;

                // Target dropdown
                ImGui::SetNextItemWidth(third);
                int tgt = (int)ab.target;
                if (ImGui::Combo("##tgt", &tgt, targetNames, (int)Layer::AudioTarget::COUNT)) {
                    ab.target = (Layer::AudioTarget)tgt;
                }
                ImGui::SameLine();

                // Signal dropdown
                ImGui::SetNextItemWidth(third);
                ImGui::Combo("##sig", &ab.signal, signalNames, 4);
                ImGui::SameLine();

                // Strength
                ImGui::SetNextItemWidth(third);
                ImGui::SliderFloat("##str", &ab.strength, 0.0f, 2.0f, "%.2f");
                ImGui::SameLine();

                // Remove
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
                if (ImGui::SmallButton("x")) removeIdx = b;
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
            if (removeIdx >= 0) layer->audioBindings.erase(layer->audioBindings.begin() + removeIdx);
        }
    }

    thinSep();

    // --- Effects ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.78f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
    bool effectsOpen = ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(3);

    if (effectsOpen) {

    // --- Layer Effects Chain ---
    {
        // Add effect button
        if (accentBtn("+ Add Effect", -1)) {
            ImGui::OpenPopup("##AddEffect");
        }
        if (ImGui::BeginPopup("##AddEffect")) {
            for (int t = 0; t < (int)EffectType::COUNT; t++) {
                if (ImGui::MenuItem(effectTypeName((EffectType)t))) {
                    LayerEffect fx;
                    fx.type = (EffectType)t;
                    layer->effects.push_back(fx);
                    undoNeeded = true;
                }
            }
            ImGui::EndPopup();
        }

        // Render each effect
        int removeIdx = -1;
        for (int e = 0; e < (int)layer->effects.size(); e++) {
            auto& fx = layer->effects[e];
            ImGui::PushID(20000 + e);

            // Effect header row: checkbox + name + remove
            ImGui::Checkbox("##en", &fx.enabled);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, fx.enabled ? ImVec4(0.85f, 0.90f, 0.95f, 1.0f) : ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("%s", effectTypeName(fx.type));
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 20);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
            if (ImGui::SmallButton("x")) removeIdx = e;
            ImGui::PopStyleColor();

            if (fx.enabled) {
                float w = ImGui::GetContentRegionAvail().x;
                switch (fx.type) {
                case EffectType::Blur:
                    ImGui::SetNextItemWidth(w);
                    ImGui::SliderFloat("##blur", &fx.blurRadius, 0.0f, 20.0f, "Blur %.1f");
                    break;
                case EffectType::ColorAdjust: {
                    float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##brt", &fx.brightness, -1.0f, 1.0f, "Brt %.2f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##ctr", &fx.contrast, -1.0f, 1.0f, "Ctr %.2f");
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##sat", &fx.saturation, -1.0f, 1.0f, "Sat %.2f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##hue", &fx.hueShift, 0.0f, 360.0f, "Hue %.0f");
                    break;
                }
                case EffectType::Invert:
                    // No params
                    break;
                case EffectType::Pixelate:
                    ImGui::SetNextItemWidth(w);
                    ImGui::SliderFloat("##pix", &fx.pixelSize, 1.0f, 64.0f, "Size %.0f");
                    break;
                case EffectType::Feedback: {
                    float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##fbmix", &fx.feedbackMix, 0.0f, 0.99f, "Mix %.2f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##fbzm", &fx.feedbackZoom, 0.95f, 1.1f, "Zoom %.3f");
                    break;
                }
                default: break;
                }
            }

            ImGui::PopID();
        }

        if (removeIdx >= 0) {
            layer->effects.erase(layer->effects.begin() + removeIdx);
            undoNeeded = true;
        }

        if (!layer->effects.empty()) {
            ImGui::Dummy(ImVec2(0, 2));
        }
    }

    } // end Effects section (layer effects chain only)

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

            // Mini spectrum bars (bass=red, lowMid=orange, highMid=green, treble=cyan)
            if (mosaicAudio) {
                float barH = 24.0f;
                float avail = ImGui::GetContentRegionAvail().x;
                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImDrawList* draw = ImGui::GetWindowDrawList();

                struct BandInfo { float level; ImU32 color; };
                BandInfo bands[4] = {
                    { mosaicAudio->bass,    IM_COL32(220, 50, 50, 200) },
                    { mosaicAudio->lowMid,  IM_COL32(230, 150, 30, 200) },
                    { mosaicAudio->highMid, IM_COL32(50, 200, 80, 200) },
                    { mosaicAudio->treble,  IM_COL32(30, 200, 220, 200) },
                };

                float bandW = avail / 4.0f;
                for (int b = 0; b < 4; b++) {
                    float h = bands[b].level * barH;
                    ImVec2 bMin(origin.x + b * bandW + 1, origin.y + barH - h);
                    ImVec2 bMax(origin.x + (b + 1) * bandW - 1, origin.y + barH);
                    draw->AddRectFilled(bMin, bMax, bands[b].color, 2.0f);
                }

                // Beat flash overlay
                if (mosaicAudio->beatDecay > 0.05f) {
                    ImU32 flashCol = IM_COL32(255, 255, 255, (int)(mosaicAudio->beatDecay * 60));
                    draw->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + barH), flashCol, 2.0f);
                }

                ImGui::Dummy(ImVec2(avail, barH + 2));
            }

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

    // --- Feather ---
    {
        ImGui::SetNextItemWidth(-1);
        if (namedDrag("##Feather", "Feather", &layer->feather, 0.005f, 0.0f, 0.5f, "%.3f"))
            undoNeeded = true;
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

        // Shape presets - 5 buttons in a row
        float shapeW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.0f;
        if (accentBtn("Rect", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeRectangle({0.5f, 0.5f}, {0.6f, 0.6f});
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Circle", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeEllipse({0.5f, 0.5f}, {0.3f, 0.3f});
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Tri", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makeTriangle({0.5f, 0.5f}, 0.3f);
            maskEditMode = true;
        }
        ImGui::SameLine();
        if (accentBtn("Oct", shapeW)) {
            undoNeeded = true;
            layer->maskPath.makePolygon({0.5f, 0.5f}, 0.3f, 8);
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
            bool muted = (vol == 0.0f);

            // Mute toggle button
            ImGui::PushStyleColor(ImGuiCol_Button, muted ? ImVec4(0.6f, 0.1f, 0.1f, 0.25f) : ImVec4(0.0f, 0.78f, 1.0f, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muted ? ImVec4(0.8f, 0.15f, 0.15f, 0.40f) : ImVec4(0.0f, 0.78f, 1.0f, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Text, muted ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button(muted ? "Unmute" : "Mute", ImVec2(54, 0))) {
                static float s_preMuteVol = 1.0f;
                if (muted) {
                    vidSrc->setVolume(s_preMuteVol > 0.01f ? s_preMuteVol : 1.0f);
                } else {
                    s_preMuteVol = vol;
                    vidSrc->setVolume(0.0f);
                }
                vol = vidSrc->volume();
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
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
                    auto& bindings = shaderSrc->audioBindings();
                    auto bit = bindings.find(input.name);
                    bool isBound = (bit != bindings.end() && bit->second.signal != AudioSignal::None);

                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();

                    // Audio bind button
                    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, isBound ? ImVec4(0.9f, 0.3f, 0.1f, 0.35f) : ImVec4(0.3f, 0.3f, 0.3f, 0.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_Text, isBound ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 0.7f));
                    if (ImGui::SmallButton("~")) {
                        ImGui::OpenPopup("##audiobind");
                    }
                    ImGui::PopStyleColor(3);

                    if (ImGui::BeginPopup("##audiobind")) {
                        static const char* signalNames[] = { "None", "Level", "Bass", "Mid", "High", "Beat" };
                        AudioBinding& ab = bindings[input.name];
                        int sigIdx = (int)ab.signal;
                        ImGui::Text("Audio Signal");
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("##sig", &sigIdx, signalNames, 6)) {
                            ab.signal = (AudioSignal)sigIdx;
                        }
                        if (ab.signal != AudioSignal::None) {
                            ImGui::Text("Range");
                            ImGui::SetNextItemWidth(55);
                            ImGui::DragFloat("##rmin", &ab.rangeMin, 0.01f, input.minVal, input.maxVal, "%.2f");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(55);
                            ImGui::DragFloat("##rmax", &ab.rangeMax, 0.01f, input.minVal, input.maxVal, "%.2f");
                            ImGui::SetNextItemWidth(120);
                            ImGui::SliderFloat("Smooth", &ab.smoothing, 0.0f, 0.95f);
                        }
                        ImGui::EndPopup();
                    }

                    if (!isBound) {
                        float v = std::get<float>(input.value);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::SliderFloat("##val", &v, input.minVal, input.maxVal)) {
                            input.value = v;
                        }
                        if (ImGui::IsItemActivated()) undoNeeded = true;
                    } else {
                        // Show current value as read-only colored bar
                        float v = std::get<float>(input.value);
                        float frac = (input.maxVal > input.minVal) ? (v - input.minVal) / (input.maxVal - input.minVal) : 0.0f;
                        ImGui::SameLine();
                        float barW = ImGui::GetContentRegionAvail().x;
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + barW, p.y + 14),
                            IM_COL32(30, 30, 30, 180), 3.0f);
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + barW * frac, p.y + 14),
                            IM_COL32(230, 120, 40, 200), 3.0f);
                        ImGui::Dummy(ImVec2(barW, 14));
                    }
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

                    // Data bus binding dropdown
                    DataBus* bus = (speech) ? speech->dataBus : nullptr;
                    uint32_t layerId = (speech) ? speech->activeLayerId : 0;
                    std::string currentBinding = bus ? bus->binding(layerId, input.name) : "";
                    bool isBound = !currentBinding.empty();

                    if (bus) {
                        auto keys = DataBus::availableKeys();
                        // Find current label
                        std::string currentLabel = "Manual";
                        for (auto& k : keys) {
                            if (k.key == currentBinding) { currentLabel = k.label; break; }
                        }
                        ImGui::SetNextItemWidth(100);
                        if (ImGui::BeginCombo("##bind", currentLabel.c_str(), ImGuiComboFlags_NoArrowButton)) {
                            for (auto& k : keys) {
                                bool sel = (k.key == currentBinding);
                                if (ImGui::Selectable(k.label.c_str(), sel)) {
                                    bus->bind(layerId, input.name, k.key);
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                    }

                    if (isBound) {
                        // Show current bound value (read-only)
                        std::string val = bus ? bus->get(currentBinding) : "";
                        if (val.size() > (size_t)maxLen) val = val.substr(val.size() - maxLen);
                        ImGui::SetNextItemWidth(-1);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.82f, 1.0f, 0.85f));
                        ImGui::TextWrapped("%s", val.empty() ? "..." : val.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        // Manual text input + MIC button
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
                } else if (input.type == "image" && layerStack) {
                    // Image input — dropdown to pick a layer as texture source
                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", input.name.c_str());
                    ImGui::PopStyleColor();

                    auto& bindings = shaderSrc->imageBindings();
                    auto it = bindings.find(input.name);
                    uint32_t currentSrcId = (it != bindings.end()) ? it->second.sourceLayerId : 0;

                    // Build label for current selection
                    std::string preview = "None";
                    for (int li = 0; li < layerStack->count(); li++) {
                        auto& other = (*layerStack)[li];
                        if (other->id == currentSrcId && other->source) {
                            preview = other->name + " (" + other->source->typeName() + ")";
                            break;
                        }
                    }

                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##imgsrc", preview.c_str())) {
                        // "None" option
                        if (ImGui::Selectable("None", currentSrcId == 0)) {
                            shaderSrc->unbindImageInput(input.name);
                        }
                        // List all other layers that have a texture
                        for (int li = 0; li < layerStack->count(); li++) {
                            auto& other = (*layerStack)[li];
                            if (other->id == layer->id) continue; // skip self
                            if (!other->source || other->source->textureId() == 0) continue;
                            std::string label = other->name + " (" + other->source->typeName() + ")";
                            bool selected = (other->id == currentSrcId);
                            if (ImGui::Selectable(label.c_str(), selected)) {
                                shaderSrc->bindImageInput(input.name,
                                    other->source->textureId(),
                                    other->source->width(),
                                    other->source->height(),
                                    other->id,
                                    other->source->isFlippedV());
                            }
                        }
                        ImGui::EndCombo();
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
