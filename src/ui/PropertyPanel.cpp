#include "ui/PropertyPanel.h"
#include "app/BPMSync.h"
#include "app/SceneManager.h"
#include "app/MIDIManager.h"
#include "compositing/BlendMode.h"
#include "compositing/LayerStack.h"
#include "sources/ShaderSource.h"
#include "sources/VideoSource.h"
#include "sources/ParticleSource.h"
#include "app/DataBus.h"
#include "app/MIDIManager.h"
#ifdef HAS_WHISPER
#include "speech/WhisperSpeech.h"
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <unordered_set>

// --- Theme ---
static const ImVec4 kDimText   = ImVec4(0.45f, 0.50f, 0.58f, 1.0f);
static const ImVec4 kMuted     = ImVec4(0.35f, 0.40f, 0.48f, 1.0f);
static const ImVec4 kRowLabel  = ImVec4(0.59f, 0.62f, 0.68f, 0.90f);
static const ImU32  kSepColor  = IM_COL32(255, 255, 255, 12);

// Dim label + optional inline follow-up. Use `sameLine=false` when the
// follow-up control lives on the next row; default keeps the legacy
// label-then-widget flow.
static void dimLabel(const char* text, const ImVec4& col = kRowLabel, bool sameLine = true) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    if (sameLine) ImGui::SameLine();
}

static void thinSep() {
    ImGui::Dummy(ImVec2(0, 4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    draw->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), kSepColor);
    ImGui::Dummy(ImVec2(0, 4));
}

static bool accentBtn(const char* label, float w = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool c = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleColor(4);
    return c;
}

// Section header with chevron; click anywhere in the row to toggle.
// Returns true when the section is OPEN (content should be drawn).
//
// Laws-of-UX notes:
//  - Proximity: tight (2px) top margin + 4px bottom margin so the header
//    visually groups with its content, not the previous section.
//  - Aesthetic-usability: chevron + label stay calm; hover brightens label.
//  - Fitts: full-row hit target (InvisibleButton spans the panel width).
static bool sectionHeader(const char* label, bool* open) {
    ImGui::Dummy(ImVec2(0, 2));
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float rowW  = ImGui::GetContentRegionAvail().x;
    float rowH  = ImGui::GetFontSize() + 6.0f;
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##sec", ImVec2(rowW, rowH));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked && open) *open = !*open;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 textCol = hovered ? IM_COL32(255, 255, 255, 255)
                            : IM_COL32(235, 240, 250, 240);
    dl->AddText(ImVec2(rowStart.x, rowStart.y + 3), textCol, label);
    float chevCx = rowStart.x + rowW - 10.0f;
    float chevCy = rowStart.y + rowH * 0.5f;
    ImU32 chevCol = IM_COL32(160, 170, 190, 220);
    if (open && *open) {
        dl->AddTriangleFilled(ImVec2(chevCx - 4, chevCy - 2),
                              ImVec2(chevCx + 4, chevCy - 2),
                              ImVec2(chevCx,     chevCy + 3), chevCol);
    } else {
        dl->AddTriangleFilled(ImVec2(chevCx - 2, chevCy - 4),
                              ImVec2(chevCx - 2, chevCy + 4),
                              ImVec2(chevCx + 3, chevCy),     chevCol);
    }
    ImGui::Dummy(ImVec2(0, 4));
    return open ? *open : true;
}

// Horizontal pill-group selector: active pill is filled, others are outlined.
// Returns the newly-selected index, or `current` if nothing changed.
static int pillGroup(const char* id, const char* const* labels, int count, int current) {
    ImGui::PushID(id);
    int result = current;
    ImGui::Dummy(ImVec2(0, 4)); // breathing room above the row
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 8));
    float avail = ImGui::GetContentRegionAvail().x;
    float rowX  = 0.0f;
    for (int i = 0; i < count; i++) {
        bool active = (i == current);
        ImVec2 sz = ImGui::CalcTextSize(labels[i]);
        float w = sz.x + 24.0f;
        if (rowX > 0 && rowX + w > avail) {
            ImGui::NewLine();
            rowX = 0;
        } else if (i > 0) {
            ImGui::SameLine();
        }
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 1.0f, 1.0f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.06f, 0.07f, 0.10f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.82f, 0.85f, 0.90f, 1.0f));
        }
        if (ImGui::Button(labels[i])) result = i;
        ImGui::PopStyleColor(4);
        rowX += w + 6.0f;
    }
    ImGui::PopStyleVar(3);
    ImGui::Dummy(ImVec2(0, 6)); // breathing room below the row
    ImGui::PopID();
    return result;
}

// Soft pill slider: label-left, value-right, thin pill track, circular handle.
// Draws on its own row, full width. Shift to snap to 0.05.
static bool pillSlider(const char* label, float* v, float lo, float hi,
                       const char* fmt = "%.2f") {
    ImGui::PushID(label);
    // Top padding was 6 — tightened to 3 for a denser vertical rhythm.
    ImGui::Dummy(ImVec2(0, 3));
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float labelH = ImGui::GetFontSize();
    float trackH = 6.0f;
    float handleR = 8.0f;
    float rowH = labelH + 14.0f; // was 16 — bring label/track a touch closer

    // Label + value row (drawn via drawlist so we control exact positions)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    char valbuf[32]; snprintf(valbuf, sizeof(valbuf), fmt, *v);
    ImVec2 valSize = ImGui::CalcTextSize(valbuf);
    dl->AddText(rowStart, IM_COL32(170, 175, 185, 220), label);
    dl->AddText(ImVec2(rowStart.x + w - valSize.x, rowStart.y),
                IM_COL32(235, 240, 250, 245), valbuf);

    // Track interaction (invisible button under the track region)
    float trackY = rowStart.y + labelH + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, trackY - 6.0f));
    bool pressed = ImGui::InvisibleButton("##track", ImVec2(w, trackH + 12.0f));
    bool active  = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (active || pressed) {
        float mx = ImGui::GetIO().MousePos.x - rowStart.x;
        float t = mx / w; if (t < 0) t = 0; if (t > 1) t = 1;
        float newV = lo + t * (hi - lo);
        if (ImGui::GetIO().KeyShift) newV = std::round(newV / 0.05f) * 0.05f;
        if (newV != *v) { *v = newV; changed = true; }
    }
    float norm = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    if (norm < 0) norm = 0; if (norm > 1) norm = 1;

    // Track background (dim) + fill (subtle)
    dl->AddRectFilled(ImVec2(rowStart.x, trackY),
                      ImVec2(rowStart.x + w, trackY + trackH),
                      IM_COL32(255, 255, 255, 14), trackH * 0.5f);
    dl->AddRectFilled(ImVec2(rowStart.x, trackY),
                      ImVec2(rowStart.x + w * norm + 0.5f, trackY + trackH),
                      IM_COL32(255, 255, 255, 44), trackH * 0.5f);
    // Handle
    float hx = rowStart.x + w * norm;
    float hy = trackY + trackH * 0.5f;
    ImU32 handleCol = active ? IM_COL32(255, 255, 255, 255)
                    : hovered ? IM_COL32(240, 244, 252, 255)
                              : IM_COL32(220, 225, 235, 255);
    dl->AddCircleFilled(ImVec2(hx, hy), handleR, handleCol);
    dl->AddCircle(ImVec2(hx, hy), handleR, IM_COL32(0, 0, 0, 110), 0, 1.2f);

    // Advance cursor past the row via Dummy (SetCursorScreenPos without a
    // follow-up item trips ImGui's bounds-check at window End). Trailing
    // padding tightened from 8 → 5 for a denser rhythm.
    ImVec2 curScreen = ImGui::GetCursorScreenPos();
    float targetY = rowStart.y + rowH + 5.0f;
    float advanceY = targetY - curScreen.y;
    if (advanceY > 0.0f) ImGui::Dummy(ImVec2(w, advanceY));
    ImGui::PopID();
    return changed;
}

// Draw a small lightning-bolt glyph inside a square at (x,y) with size s.
// Two overlapping triangles produce a classic ⚡ silhouette.
static void drawBolt(ImDrawList* dl, float cx, float cy, float s, ImU32 col) {
    float h = s * 0.5f;
    // Top arm (slants up-right)
    dl->AddTriangleFilled(ImVec2(cx + h * 0.55f, cy - h),
                          ImVec2(cx - h * 0.55f, cy + h * 0.15f),
                          ImVec2(cx + h * 0.15f, cy + h * 0.15f),
                          col);
    // Bottom arm (slants down-left), overlapping at the middle
    dl->AddTriangleFilled(ImVec2(cx - h * 0.55f, cy + h),
                          ImVec2(cx + h * 0.55f, cy - h * 0.15f),
                          ImVec2(cx - h * 0.15f, cy - h * 0.15f),
                          col);
}

// Parameter row styled after the reference UI: muted label (top-left) + right-aligned
// value on the label row, a full-width pill track with circular handle below. A small
// lightning-bolt icon sits at the very left — click it to open the binding menu so the
// parameter can be driven by audio, MIDI, body tracking, etc. When bound the bolt fills
// amber and the slider's fill tints to match, echoing the "interactivity" state.
struct ParamSliderResult {
    bool changed      = false;
    bool openBindMenu = false; // bolt clicked OR row right-clicked
    bool activated    = false; // drag just started — for undo snapshots
};
static ParamSliderResult paramSlider(const char* id, const char* label, float* v,
                                     float lo, float hi, bool bound,
                                     const char* fmt = "%.2f") {
    ParamSliderResult r;
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(0, 8)); // top breathing room

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float labelH  = ImGui::GetFontSize();
    float trackH  = 6.0f;
    float handleR = 8.0f;
    float rowH    = labelH + 20.0f;
    float boltBox = 18.0f; // hit-target square around the bolt glyph

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ⚡ Bolt button — leftmost affordance. Clicking opens the bind menu.
    ImGui::SetCursorScreenPos(ImVec2(rowStart.x - 2.0f,
                                     rowStart.y + (labelH - boltBox) * 0.5f));
    bool boltClicked = ImGui::InvisibleButton("##bolt", ImVec2(boltBox, boltBox));
    bool boltHovered = ImGui::IsItemHovered();
    if (boltClicked) r.openBindMenu = true;

    ImU32 boltCol = bound       ? IM_COL32(232, 150,  70, 255)
                  : boltHovered ? IM_COL32(215, 225, 240, 230)
                                : IM_COL32(120, 128, 142, 180);
    drawBolt(dl, rowStart.x + boltBox * 0.5f - 2.0f,
                 rowStart.y + labelH * 0.55f,
                 12.0f, boltCol);

    float labelX = rowStart.x + boltBox + 4.0f;

    // Label (muted) + right-aligned value (bright).
    char valbuf[32]; snprintf(valbuf, sizeof(valbuf), fmt, *v);
    ImVec2 valSize = ImGui::CalcTextSize(valbuf);
    dl->AddText(ImVec2(labelX, rowStart.y),
                IM_COL32(150, 158, 172, 230), label);
    dl->AddText(ImVec2(rowStart.x + w - valSize.x, rowStart.y),
                IM_COL32(235, 240, 250, 245), valbuf);

    // Right-click anywhere on the row also opens the bind menu.
    float trackY = rowStart.y + labelH + 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(labelX, rowStart.y));
    ImGui::InvisibleButton("##row", ImVec2(w - (labelX - rowStart.x), labelH + 4));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) r.openBindMenu = true;

    // Track drag hit-target.
    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, trackY - 6.0f));
    ImGui::InvisibleButton("##track", ImVec2(w, trackH + 12.0f));
    bool tActive  = ImGui::IsItemActive();
    bool hovered  = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) r.activated = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) r.openBindMenu = true;
    if (tActive) {
        float mx = ImGui::GetIO().MousePos.x - rowStart.x;
        float t = mx / w; if (t < 0) t = 0; if (t > 1) t = 1;
        float newV = lo + t * (hi - lo);
        if (ImGui::GetIO().KeyShift) newV = std::round(newV / 0.05f) * 0.05f;
        if (newV != *v) { *v = newV; r.changed = true; }
    }

    float norm = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    if (norm < 0) norm = 0; if (norm > 1) norm = 1;

    // Track background + fill (fill tint shifts to amber when bound).
    ImU32 fillCol = bound ? IM_COL32(232, 150, 70, 190)
                          : IM_COL32(255, 255, 255, 52);
    dl->AddRectFilled(ImVec2(rowStart.x, trackY),
                      ImVec2(rowStart.x + w, trackY + trackH),
                      IM_COL32(255, 255, 255, 14), trackH * 0.5f);
    dl->AddRectFilled(ImVec2(rowStart.x, trackY),
                      ImVec2(rowStart.x + w * norm + 0.5f, trackY + trackH),
                      fillCol, trackH * 0.5f);
    // Handle
    float hx = rowStart.x + w * norm;
    float hy = trackY + trackH * 0.5f;
    ImU32 handleCol = tActive  ? IM_COL32(255, 255, 255, 255)
                    : hovered  ? IM_COL32(240, 244, 252, 255)
                               : IM_COL32(220, 225, 235, 255);
    dl->AddCircleFilled(ImVec2(hx, hy), handleR, handleCol);
    dl->AddCircle(ImVec2(hx, hy), handleR, IM_COL32(0, 0, 0, 110), 0, 1.2f);

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + rowH + 10.0f));
    ImGui::PopID();
    return r;
}

// Color row — muted label on the left, circular swatch on the right that opens
// the native color picker. One row per color input, matching the reference's
// quiet layout (no cramped dropdown + tiny chip).
static bool paramColorRow(const char* id, const char* label, glm::vec4* c) {
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(0, 8));
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float labelH = ImGui::GetFontSize();
    float swatchR = 10.0f;
    float rowH = std::max(labelH, swatchR * 2.0f) + 8.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(rowStart.x, rowStart.y + (rowH - labelH) * 0.5f),
                IM_COL32(150, 158, 172, 230), label);

    // Reserve the whole row so the invisible button / swatch live on one line.
    ImGui::InvisibleButton("##row", ImVec2(w, rowH));

    // Circular swatch, right-aligned.
    float cx = rowStart.x + w - swatchR - 2.0f;
    float cy = rowStart.y + rowH * 0.5f;
    ImU32 fill = IM_COL32((int)(c->r * 255), (int)(c->g * 255),
                          (int)(c->b * 255), (int)(c->a * 255));
    dl->AddCircleFilled(ImVec2(cx, cy), swatchR, fill);
    dl->AddCircle(ImVec2(cx, cy), swatchR, IM_COL32(255, 255, 255, 60), 0, 1.0f);

    // A small invisible hit-target around the swatch so clicking it opens the picker.
    ImVec2 hitMin(cx - swatchR - 4, cy - swatchR - 4);
    ImGui::SetCursorScreenPos(hitMin);
    ImGui::InvisibleButton("##swatch", ImVec2(swatchR * 2 + 8, swatchR * 2 + 8));
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##picker");

    bool changed = false;
    if (ImGui::BeginPopup("##picker")) {
        if (ImGui::ColorPicker4("##cp", &(*c)[0],
                                ImGuiColorEditFlags_NoLabel |
                                ImGuiColorEditFlags_AlphaBar)) {
            changed = true;
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + rowH + 6.0f));
    ImGui::PopID();
    return changed;
}

// Toggle row — label on the left, pill-style switch on the right. One row per bool.
static bool paramToggleRow(const char* id, const char* label, bool* b) {
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(0, 8));
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float labelH = ImGui::GetFontSize();
    float switchW = 30.0f, switchH = 16.0f;
    float rowH = std::max(labelH, switchH) + 8.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(rowStart.x, rowStart.y + (rowH - labelH) * 0.5f),
                IM_COL32(150, 158, 172, 230), label);

    float sx = rowStart.x + w - switchW - 2.0f;
    float sy = rowStart.y + (rowH - switchH) * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(sx, sy));
    bool clicked = ImGui::InvisibleButton("##sw", ImVec2(switchW, switchH));
    if (clicked) { *b = !*b; }

    ImU32 trackCol = *b ? IM_COL32(232, 150, 70, 200) : IM_COL32(255, 255, 255, 28);
    dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + switchW, sy + switchH),
                      trackCol, switchH * 0.5f);
    float knobR = switchH * 0.5f - 2.0f;
    float knobX = *b ? sx + switchW - knobR - 2.0f : sx + knobR + 2.0f;
    float knobY = sy + switchH * 0.5f;
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, IM_COL32(240, 244, 250, 255));

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + rowH + 6.0f));
    ImGui::PopID();
    return clicked;
}

// Small inter-section gap — use between logical groups of controls.
// 10 px rhythm was chosen via UX pass: tight enough to group visually
// (Proximity) but loose enough that headers don't blur into prior content.
static void sectionBreak() {
    ImGui::Dummy(ImVec2(0, 10));
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

// Two drags side by side with independent speed/range/format per slot.
// Use when the neighboring values aren't homogeneous (e.g. Size + Rot).
struct DragCfg { float speed, lo, hi; const char* fmt; };
struct DragPairResult { bool changedA, changedB, activated; };
static DragPairResult dragPair2(const char* idA, const char* labelA, float* a, DragCfg ca,
                                const char* idB, const char* labelB, float* b, DragCfg cb) {
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    DragPairResult r{false, false, false};
    ImGui::SetNextItemWidth(w);
    r.changedA = namedDrag(idA, labelA, a, ca.speed, ca.lo, ca.hi, ca.fmt);
    if (ImGui::IsItemActivated()) r.activated = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w);
    r.changedB = namedDrag(idB, labelB, b, cb.speed, cb.lo, cb.hi, cb.fmt);
    if (ImGui::IsItemActivated()) r.activated = true;
    return r;
}

void PropertyPanel::render(std::shared_ptr<Layer> layer, bool& maskEditMode,
                           SpeechState* speech, MosaicAudioState* mosaicAudio,
                           float appTime, LayerStack* layerStack,
                           BPMSync* bpmSync, SceneManager* sceneManager,
                           int* audioDeviceIdx, MIDIManager* midi) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(250, 200), ImVec2(FLT_MAX, FLT_MAX));
    // Window name uses the "display###ID" form so the tab shows a minimal
    // "    ###Properties" — a few spaces reserve the tab's visible width
    // without rendering any readable label text. UIManager::drawMyTabIcon
    // then paints the filter icon over the tab rect so the tab reads as
    // an icon. The "###ID" half keeps the internal window name stable
    // for dock/focus lookups.
    ImGui::Begin("        ###Properties");

    // BPM/Audio controls now live in the Audio panel.
    // Scene management now lives in the Scenes panel.
    // Properties is empty unless a layer is selected.
#if 0
    // --- Audio (BPM + bindings, always visible) ---
    if (bpmSync) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
        bool audioSectionOpen = ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);

        if (audioSectionOpen) {
            float currentBPM = bpmSync->bpm();
            float w = ImGui::GetContentRegionAvail().x;

            // Beat indicator dots + BPM text
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
                char bpmBuf[16];
                if (currentBPM > 0) snprintf(bpmBuf, sizeof(bpmBuf), "%.1f BPM", currentBPM);
                else snprintf(bpmBuf, sizeof(bpmBuf), "--- BPM");
                dl->AddText(ImVec2(p.x + 74, p.y + 2),
                            currentBPM > 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 115, 140, 180),
                            bpmBuf);
                ImGui::Dummy(ImVec2(w, 18));
            }

            // TAP + BPM input + Reset
            {
                float btnW = (w - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (ImGui::Button("TAP", ImVec2(btnW, 0))) bpmSync->tap();
                ImGui::PopStyleColor(4);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(btnW);
                float bpmVal = currentBPM;
                if (ImGui::DragFloat("##BPMVal", &bpmVal, 0.5f, 0.0f, 300.0f, "%.0f BPM"))
                    bpmSync->setBPM(bpmVal);
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

            // Audio reactive bindings (only when a layer is selected)
            if (layer) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                ImGui::Text("Bindings");
                ImGui::PopStyleColor();

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
                    float bw = ImGui::GetContentRegionAvail().x;
                    float third = (bw - ImGui::GetStyle().ItemSpacing.x * 2 - 20) / 3.0f;
                    ImGui::SetNextItemWidth(third);
                    int tgt = (int)ab.target;
                    if (ImGui::Combo("##tgt", &tgt, targetNames, (int)Layer::AudioTarget::COUNT))
                        ab.target = (Layer::AudioTarget)tgt;
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::Combo("##sig", &ab.signal, signalNames, 4);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::SliderFloat("##str", &ab.strength, 0.0f, 2.0f, "%.2f");
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.3f, 0.3f, 0.7f));
                    if (ImGui::SmallButton("x")) removeIdx = b;
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                if (removeIdx >= 0) layer->audioBindings.erase(layer->audioBindings.begin() + removeIdx);
            }
        }

        thinSep();
    }

    // --- Scenes (always visible) ---
    if (sceneManager && layerStack) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
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
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
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
#endif
    // Silence unused-parameter warnings for sections now routed elsewhere.
    (void)bpmSync; (void)sceneManager;

    if (!layer) {
        // Empty Properties — content appears only when a layer/shader/etc. is selected.
        ImGui::End();
        return;
    }

    undoNeeded = false;

    // (Layer-name rename field removed — the Layer panel already shows and
    //  edits the name; duplicating it here just added scroll distance before
    //  the user could reach the shader parameters.)
    ImGui::Dummy(ImVec2(0, 4));

    // --- Blend + Opacity ---
    {
        // Blend as a full-width dropdown (12 modes is too many for a pill row).
        ImGui::SetNextItemWidth(-FLT_MIN);
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
        pillSlider("Opacity", &layer->opacity, 0.0f, 1.0f, "%.2f");
    }

    // (Transition section moved to the BOTTOM of the Parameters panel —
    // see the matching block below the shader-inputs loop.)

    // --- Transform (collapsible, default closed) — secondary controls go
    // under a header so the main event (shader parameters / video) isn't
    // buried under a wall of position/scale/rotation.
    static bool transformOpen = false;
    if (sectionHeader("Transform", &transformOpen)) {
        if (dragPair("##PosX", "X", &layer->position.x, "##PosY", "Y", &layer->position.y,
                     0.01f, -2.0f, 2.0f))
        {}
        if (ImGui::IsItemActivated()) undoNeeded = true;

        {
            float uniformScale = (layer->scale.x + layer->scale.y) * 0.5f;
            auto sr = dragPair2(
                "##Size", "Size", &uniformScale, {0.01f, 0.01f, 10.0f, "%.2f"},
                "##Rot",  "Rot",  &layer->rotation, {1.0f, -360.0f, 360.0f, "%.1f"});
            if (sr.changedA) {
                float ratio = (layer->scale.x > 0.001f) ? layer->scale.y / layer->scale.x : 1.0f;
                layer->scale.x = uniformScale;
                layer->scale.y = uniformScale * ratio;
            }
            if (sr.activated) undoNeeded = true;
        }

        if (dragPair("##ScaleX", "W", &layer->scale.x, "##ScaleY", "H", &layer->scale.y,
                     0.01f, 0.01f, 10.0f))
        {}
        if (ImGui::IsItemActivated()) undoNeeded = true;

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

        // Crop sits inside Transform — cropping is a spatial adjustment,
        // so it belongs with position/scale/rotation rather than as its
        // own top-level section.
        ImGui::Dummy(ImVec2(0, 8));
        dimLabel("Crop", kRowLabel, false);
        if (ImGui::Checkbox("Auto-trim black borders", &layer->autoCrop)) {
            if (layer->autoCrop) {
                layer->autoCropDone = false;
            } else {
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
    }

    // --- Effects ---
    // When no effects exist yet, show a single inline row: "Effects" label on
    // the left + "+ Add Effect" button on the right. Skips the wasteful
    // collapsible header + full-width button of the empty state.
    // Once effects exist, falls back to the normal collapsible section so the
    // per-effect rows can stack below.
    static bool effectsOpen = false;
    auto openAddEffectPopup = [&]() {
        if (ImGui::BeginPopup("##AddEffect")) {
            for (int t = 0; t < (int)EffectType::COUNT; t++) {
                if (ImGui::MenuItem(effectTypeName((EffectType)t))) {
                    LayerEffect fx;
                    fx.type = (EffectType)t;
                    layer->effects.push_back(fx);
                    undoNeeded = true;
                    effectsOpen = true; // auto-expand once something's been added
                }
            }
            ImGui::EndPopup();
        }
    };
    if (layer->effects.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.95f, 0.98f, 1.0f));
        ImGui::Text("Effects");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const char* btn = "+ Add Effect";
        float btnW = ImGui::CalcTextSize(btn).x
                   + ImGui::GetStyle().FramePadding.x * 2.0f + 12.0f;
        float rightX = ImGui::GetWindowContentRegionMax().x - btnW;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
        if (accentBtn(btn)) ImGui::OpenPopup("##AddEffect");
        openAddEffectPopup();
        ImGui::Dummy(ImVec2(0, 2));
    } else
    if (sectionHeader("Effects", &effectsOpen)) {
    {

    // --- Layer Effects Chain ---
    {
        // Add effect button (full-width once the section holds effect rows)
        if (accentBtn("+ Add Effect", -1)) {
            ImGui::OpenPopup("##AddEffect");
        }
        openAddEffectPopup();

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
            ImGui::SameLine();
            {
                float rightX = ImGui::GetWindowContentRegionMax().x - 20.0f;
                if (ImGui::GetCursorPosX() < rightX) ImGui::SetCursorPosX(rightX);
            }
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
                case EffectType::Glow: {
                    ImGui::SetNextItemWidth(w);
                    ImGui::SliderFloat("##glowT", &fx.glowThreshold, 0.0f, 1.0f, "Thresh %.2f");
                    float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##glowR", &fx.glowRadius, 1.0f, 40.0f, "Rad %.1f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(half);
                    ImGui::SliderFloat("##glowI", &fx.glowIntensity, 0.0f, 3.0f, "Int %.2f");
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
    } // end sectionHeader("Effects")

    // --- Mosaic + Feather (collapsible, default closed) ---
    static bool tilingOpen = false;
    if (sectionHeader("Tiling", &tilingOpen)) {
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
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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
                ImGui::SetCursorScreenPos(origin);
                ImGui::InvisibleButton("##TileGrid", ImVec2(gridW, gridH));

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
                            draw->AddRect(cMin, cMax, IM_COL32(255, 255, 255, 60), 2.0f);
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

    // --- Feather (inside Tiling collapsible) ---
    pillSlider("Feather", &layer->feather, 0.0f, 0.5f, "%.3f");
    } // end sectionHeader("Tiling")

    // --- Drop Shadow ---
    // Inline header row: label + Enable checkbox on the same line. If the
    // checkbox is off, there's nothing to tweak so we skip the controls
    // entirely — saving a dropdown click for the common "just want it on"
    // case. When on, controls appear directly below.
    {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.95f, 0.98f, 1.0f));
        ImGui::Text("Drop Shadow");
        ImGui::PopStyleColor();

        // Right-anchored cluster: "Enable" label + checkbox (label on the
        // LEFT of the square). Use the "##" hidden-label trick so ImGui draws
        // just the square, then place our own Text before it.
        const float squareW = ImGui::GetFrameHeight();
        const float gap     = ImGui::GetStyle().ItemInnerSpacing.x;
        const float textW   = ImGui::CalcTextSize("Enable").x;
        const float clusterW = textW + gap + squareW + 4.0f;
        ImGui::SameLine();
        float rightX = ImGui::GetWindowContentRegionMax().x - clusterW;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Enable");
        ImGui::SameLine(0, gap);
        if (ImGui::Checkbox("##dshadow", &layer->dropShadowEnabled)) undoNeeded = true;
    }
    if (layer->dropShadowEnabled) {
        {
            if (dragPair("##DsOx", "X", &layer->dropShadowOffsetX,
                         "##DsOy", "Y", &layer->dropShadowOffsetY,
                         0.002f, -1.0f, 1.0f, "%.3f"))
                undoNeeded = true;
            ImGui::SetNextItemWidth(-1);
            if (namedDrag("##DsBlur", "Blur", &layer->dropShadowBlur, 0.25f, 0.0f, 80.0f, "%.1f"))
                undoNeeded = true;
            ImGui::SetNextItemWidth(-1);
            if (namedDrag("##DsOpac", "Opacity", &layer->dropShadowOpacity, 0.01f, 0.0f, 1.0f, "%.2f"))
                undoNeeded = true;
            ImGui::SetNextItemWidth(-1);
            if (namedDrag("##DsSpread", "Spread", &layer->dropShadowSpread, 0.02f, 0.5f, 8.0f, "%.2f"))
                undoNeeded = true;
            // Color picker
            float col[3] = { layer->dropShadowColorR, layer->dropShadowColorG, layer->dropShadowColorB };
            if (ImGui::ColorEdit3("Color##dsCol", col, ImGuiColorEditFlags_NoInputs)) {
                layer->dropShadowColorR = col[0];
                layer->dropShadowColorG = col[1];
                layer->dropShadowColorB = col[2];
                undoNeeded = true;
            }
        }
    }

    // --- Video controls ---
    if (layer->source && layer->source->isVideo()) {
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
            ImGui::PushStyleColor(ImGuiCol_Button, muted ? ImVec4(0.6f, 0.1f, 0.1f, 0.25f) : ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muted ? ImVec4(0.8f, 0.15f, 0.15f, 0.40f) : ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Text, muted ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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

    // --- Particle System controls ----------------------------------
    // Niagara-style emitter inspector: spawn config + module stack. Each
    // module has its own header so users can collapse/reorder them.
    if (layer->source && layer->source->typeName() == "Particles") {
        auto* psrc = static_cast<ParticleSource*>(layer->source.get());
        auto& em = psrc->emitter();

        sectionBreak();
        static bool emitterOpen = true;
        if (sectionHeader("Emitter", &emitterOpen)) {
            // All-vertical layout — no inline SameLine with absolute column
            // positions. Each control gets its own row so ImGui never has to
            // compute a negative cursor offset on narrow panels.
            ImGui::Dummy(ImVec2(0, 4));

            dimLabel("Spawn Shape", kRowLabel, false);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##PSShape", particleSpawnShapeName(em.spawnShape))) {
                for (int i = 0; i < (int)ParticleSpawnShape::COUNT; i++) {
                    bool sel = ((int)em.spawnShape == i);
                    if (ImGui::Selectable(particleSpawnShapeName((ParticleSpawnShape)i), sel))
                        em.spawnShape = (ParticleSpawnShape)i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            pillSlider("Spawn Rate", &em.spawnRate, 0.0f, 2000.0f, "%.0f/s");
            {
                float mp = (float)em.maxParticles;
                if (pillSlider("Max Particles", &mp, 100.0f, 20000.0f, "%.0f"))
                    em.maxParticles = (int)mp;
            }
            pillSlider("Lifetime Min", &em.lifetimeMin, 0.1f, 10.0f, "%.2fs");
            pillSlider("Lifetime Max", &em.lifetimeMax, 0.1f, 10.0f, "%.2fs");
            pillSlider("Initial Size", &em.initialSize, 0.001f, 0.5f, "%.3f");
            pillSlider("Size Jitter",  &em.sizeJitter,  0.0f, 1.0f, "%.2f");
            pillSlider("Velocity Jitter", &em.velocityJitter, 0.0f, 3.0f, "%.2f");

            if (ImGui::Checkbox("Additive Blend", &em.additive)) {}

            dimLabel("Render Mode", kRowLabel, false);
            const char* modes[] = {"Soft Sprite", "Textured", "Ring"};
            int rm = em.renderMode; if (rm < 0 || rm > 2) rm = 0;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##PSRender", modes[rm])) {
                for (int i = 0; i < 3; i++) {
                    bool sel = (rm == i);
                    if (ImGui::Selectable(modes[i], sel)) em.renderMode = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TextDisabled("%d live particles", psrc->liveParticleCount());
        }

        sectionBreak();
        static bool modulesOpen = true;
        if (sectionHeader("Modules", &modulesOpen)) {
            // + Add Module dropdown
            if (accentBtn("+ Add Module", -1)) {
                ImGui::OpenPopup("##AddParticleModule");
            }
            if (ImGui::BeginPopup("##AddParticleModule")) {
                for (int t = 0; t < (int)ParticleModuleType::COUNT; t++) {
                    if (ImGui::MenuItem(particleModuleTypeName((ParticleModuleType)t))) {
                        psrc->addModule((ParticleModuleType)t);
                    }
                }
                ImGui::EndPopup();
            }

            int toRemove = -1;
            int toMoveIdx = -1, toMoveDir = 0;
            auto& mods = em.modules;
            for (int i = 0; i < (int)mods.size(); i++) {
                auto& mod = mods[i];
                ImGui::PushID(50000 + i);
                ImGui::Dummy(ImVec2(0, 4));

                // Row 1: [X] Module Name — no right-aligned cluster (that
                // pattern kept tripping ImGui's cursor-bounds assertion in
                // narrow panels). Action buttons get their own row below.
                if (ImGui::Checkbox("##en", &mod.enabled)) {}
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text,
                    mod.enabled ? ImVec4(0.93f, 0.95f, 0.98f, 1.0f)
                                : ImVec4(0.55f, 0.58f, 0.64f, 1.0f));
                ImGui::Text("%s", particleModuleTypeName(mod.type));
                ImGui::PopStyleColor();

                // Row 2: small action buttons — safe SameLine chain.
                if (ImGui::SmallButton("up")) { toMoveIdx = i; toMoveDir = -1; }
                ImGui::SameLine();
                if (ImGui::SmallButton("dn")) { toMoveIdx = i; toMoveDir = +1; }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.35f, 0.35f, 1.0f));
                if (ImGui::SmallButton("remove")) toRemove = i;
                ImGui::PopStyleColor();

                // Per-module parameters
                if (mod.enabled) {
                    ImGui::Indent(8.0f);
                    switch (mod.type) {
                        case ParticleModuleType::InitialVelocity:
                            pillSlider("Vel X", &mod.vec3A.x, -5.0f, 5.0f, "%.2f");
                            pillSlider("Vel Y", &mod.vec3A.y, -5.0f, 5.0f, "%.2f");
                            pillSlider("Vel Z", &mod.vec3A.z, -5.0f, 5.0f, "%.2f");
                            pillSlider("Randomness", &mod.randomness, 0.0f, 1.0f, "%.2f");
                            break;
                        case ParticleModuleType::Gravity:
                            pillSlider("G X", &mod.vec3A.x, -5.0f, 5.0f, "%.2f");
                            pillSlider("G Y", &mod.vec3A.y, -5.0f, 5.0f, "%.2f");
                            pillSlider("G Z", &mod.vec3A.z, -5.0f, 5.0f, "%.2f");
                            break;
                        case ParticleModuleType::Drag:
                            pillSlider("Drag", &mod.floatA, 0.0f, 5.0f, "%.2f");
                            break;
                        case ParticleModuleType::Orbital:
                            pillSlider("Axis X", &mod.vec3A.x, -1.0f, 1.0f, "%.2f");
                            pillSlider("Axis Y", &mod.vec3A.y, -1.0f, 1.0f, "%.2f");
                            pillSlider("Axis Z", &mod.vec3A.z, -1.0f, 1.0f, "%.2f");
                            pillSlider("Speed",  &mod.floatA, -8.0f, 8.0f, "%.2f");
                            break;
                        case ParticleModuleType::Turbulence:
                            pillSlider("Strength",  &mod.floatA, 0.0f, 4.0f, "%.2f");
                            pillSlider("Frequency", &mod.floatB, 0.1f, 8.0f, "%.2f");
                            break;
                        case ParticleModuleType::SizeOverLife:
                            pillSlider("Size × (start)", &mod.floatA, 0.0f, 5.0f, "%.2f");
                            pillSlider("Size × (end)",   &mod.floatB, 0.0f, 5.0f, "%.2f");
                            break;
                        case ParticleModuleType::ColorOverLife:
                            if (ImGui::ColorEdit4("Start##cA", &mod.colorA.r,
                                                  ImGuiColorEditFlags_NoInputs)) {}
                            if (ImGui::ColorEdit4("End##cB",   &mod.colorB.r,
                                                  ImGuiColorEditFlags_NoInputs)) {}
                            break;
                        case ParticleModuleType::RotationOverLife:
                            pillSlider("Start (rad)", &mod.floatA, -6.28f, 6.28f, "%.2f");
                            pillSlider("End (rad)",   &mod.floatB, -6.28f, 6.28f, "%.2f");
                            break;
                        case ParticleModuleType::TextureSampleColor:
                            ImGui::TextDisabled("Color is sampled from the bound\n"
                                                "image/video layer at spawn.");
                            break;
                        default: break;
                    }
                    ImGui::Unindent(8.0f);
                }
                ImGui::PopID();
            }
            if (toMoveIdx >= 0) psrc->moveModule(toMoveIdx, toMoveDir);
            if (toRemove >= 0)  psrc->removeModule(toRemove);
        }
    }

    // --- Shader (ISF) controls ---
    if (layer->source && layer->source->isShader()) {
        auto* shaderSrc = static_cast<ShaderSource*>(layer->source.get());
        auto& inputs = shaderSrc->inputs();

        // Shader resolution override
        {
            sectionBreak();

            struct ResPreset { const char* label; int w; int h; };
            ResPreset presets[] = {
                {"Canvas", 0, 0},
                {"720p",  1280, 720},
                {"1080p", 1920, 1080},
                {"1440p", 2560, 1440},
                {"4K",    3840, 2160},
            };
            // Per-layer "custom mode" flag — once the user picks Custom...,
            // stay in custom mode even if the width/height happen to coincide
            // with a preset. Keyed by layer id so selecting a different layer
            // starts fresh. Without this the combo bounces straight back to
            // "1080p" (etc.) and the Size input row never appears.
            static std::unordered_set<uint32_t> s_customMode;
            bool customMode = s_customMode.count(layer->id) > 0;
            bool matchesPreset = false;
            const char* currentLabel = "Custom";
            for (auto& p : presets) {
                if (layer->shaderWidth == p.w && layer->shaderHeight == p.h) {
                    currentLabel = p.label;
                    matchesPreset = true;
                    break;
                }
            }
            if (customMode) matchesPreset = false;
            // "Custom" label shows the current dimensions inline so users
            // can see the exact value at a glance without opening the combo.
            static char customLabel[64];
            if (!matchesPreset) {
                snprintf(customLabel, sizeof(customLabel),
                         "Custom  (%d x %d)",
                         layer->shaderWidth, layer->shaderHeight);
                currentLabel = customLabel;
            }

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ShaderRes", currentLabel)) {
                for (auto& p : presets) {
                    bool sel = !customMode
                             && layer->shaderWidth == p.w
                             && layer->shaderHeight == p.h;
                    if (ImGui::Selectable(p.label, sel)) {
                        layer->shaderWidth = p.w;
                        layer->shaderHeight = p.h;
                        s_customMode.erase(layer->id);
                    }
                }
                // Dedicated "Custom..." entry — flips the layer into custom
                // mode so the Size input row appears. If the layer was on a
                // preset, seed the custom fields with that preset's size
                // (or 1920x1080 for Canvas) so editing starts from a sane
                // default instead of 0x0.
                ImGui::Separator();
                if (ImGui::Selectable("Custom...", customMode)) {
                    if (layer->shaderWidth == 0 && layer->shaderHeight == 0) {
                        layer->shaderWidth  = 1920;
                        layer->shaderHeight = 1080;
                    }
                    s_customMode.insert(layer->id);
                }
                ImGui::EndCombo();
            }

            // Inline custom W x H inputs — shown beneath the dropdown when
            // the layer is in custom mode. Full-width, labeled, with Apply
            // so the value isn't committed on every keystroke.
            if (!matchesPreset) {
                static int   customW = 1920, customH = 1080;
                static Layer* lastLayer = nullptr;
                if (lastLayer != layer.get()) {
                    customW = layer->shaderWidth  > 0 ? layer->shaderWidth  : 1920;
                    customH = layer->shaderHeight > 0 ? layer->shaderHeight : 1080;
                    lastLayer = layer.get();
                }
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                ImGui::Text("Size");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                float inputW = (ImGui::GetContentRegionAvail().x - 70.0f) * 0.5f;
                if (inputW < 50.0f) inputW = 50.0f;
                ImGui::SetNextItemWidth(inputW);
                ImGui::InputInt("##cw", &customW, 0);
                ImGui::SameLine();
                ImGui::TextDisabled("x");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(inputW);
                ImGui::InputInt("##ch", &customH, 0);
                ImGui::SameLine();
                bool changed = (customW != layer->shaderWidth || customH != layer->shaderHeight);
                if (!changed) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Apply")) {
                    if (customW >= 64 && customH >= 64
                        && customW <= 7680 && customH <= 4320) {
                        layer->shaderWidth  = customW;
                        layer->shaderHeight = customH;
                        undoNeeded = true;
                    }
                }
                if (!changed) ImGui::EndDisabled();
            }
        }

        if (!inputs.empty()) {
            sectionBreak();

            // "Parameters" header removed — shader inputs render directly
            // after the composition section. Each input has its own label
            // row so the group heading was redundant noise.
            ImGui::Dummy(ImVec2(0, 4));
            {
            for (int i = 0; i < (int)inputs.size(); i++) {
                auto& input = inputs[i];
                ImGui::PushID(i + 10000);

                if (input.type == "float") {
                    auto& bindings = shaderSrc->audioBindings();
                    auto bit = bindings.find(input.name);
                    bool isBound = (bit != bindings.end() && bit->second.signal != AudioSignal::None);

                    // Pick a format that fits the range.
                    const char* fmt = "%.2f";
                    float range = input.maxVal - input.minVal;
                    if (range > 100.0f)      fmt = "%.0f";
                    else if (range > 10.0f)  fmt = "%.1f";
                    else if (range < 1.0f)   fmt = "%.3f";

                    float v = std::get<float>(input.value);
                    ParamSliderResult ps = paramSlider("##val", input.name.c_str(),
                                                      &v, input.minVal, input.maxVal,
                                                      isBound, fmt);
                    if (ps.changed) input.value = v;
                    if (ps.activated) undoNeeded = true;
                    if (ps.openBindMenu) ImGui::OpenPopup("##audiobind");

                    if (ImGui::BeginPopup("##audiobind")) {
                        static const char* signalNames[] = { "None", "Level", "Bass", "Mid", "High", "Beat", "MIDI" };
                        bool isNew = (bindings.find(input.name) == bindings.end());
                        AudioBinding& ab = bindings[input.name];
                        if (isNew) { ab.rangeMin = input.minVal; ab.rangeMax = input.maxVal; }
                        int sigIdx = (int)ab.signal;
                        ImGui::Text("Source");
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("##sig", &sigIdx, signalNames, IM_ARRAYSIZE(signalNames))) {
                            ab.signal = (AudioSignal)sigIdx;
                        }
                        if (ab.signal == AudioSignal::MidiCC) {
                            ImGui::Text("MIDI CC");
                            ImGui::SetNextItemWidth(55);
                            ImGui::InputInt("##cc", &ab.midiCC, 1, 1);
                            if (ab.midiCC < -1) ab.midiCC = -1;
                            if (ab.midiCC > 127) ab.midiCC = 127;
                            ImGui::SameLine();
                            ImGui::Text("Ch");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(55);
                            int ch1 = ab.midiChannel + 1; // display 1-16 (0 = any)
                            if (ImGui::InputInt("##chan", &ch1, 1, 1)) {
                                if (ch1 < 0) ch1 = 0;
                                if (ch1 > 16) ch1 = 16;
                                ab.midiChannel = ch1 - 1; // -1 = any
                            }
                            if (midi) {
                                bool learning = midi->isLearning();
                                if (learning) {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.3f, 0.1f, 0.6f));
                                    if (ImGui::Button("Learning... (move a knob)", ImVec2(-1, 0))) {
                                        midi->stopLearn();
                                    }
                                    ImGui::PopStyleColor();
                                    if (midi->hasLearnEvent()) {
                                        auto evt = midi->lastLearnEvent();
                                        ab.midiCC = evt.number;
                                        ab.midiChannel = evt.channel;
                                        midi->stopLearn();
                                    }
                                } else {
                                    if (ImGui::Button("MIDI Learn", ImVec2(-1, 0))) {
                                        midi->startLearn();
                                    }
                                }
                                if (!midi->isOpen()) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                                    ImGui::TextWrapped("No MIDI device open");
                                    ImGui::PopStyleColor();
                                }
                            }
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

                    // (The bound/unbound bar below has been replaced by paramSlider
                    // above, which draws a unified pill track and tints its fill amber
                    // when a binding is active.)
                } else if (input.type == "color") {
                    glm::vec4 c = std::get<glm::vec4>(input.value);
                    if (paramColorRow("##col", input.name.c_str(), &c)) {
                        input.value = c;
                        undoNeeded = true;
                    }
                } else if (input.type == "bool") {
                    bool b = std::get<bool>(input.value);
                    if (paramToggleRow("##bool", input.name.c_str(), &b)) {
                        input.value = b;
                        undoNeeded = true;
                    }
                } else if (input.type == "point2D") {
                    // Two stacked paramSliders (X / Y) keeps the clean
                    // label-top, pill-track-below rhythm.
                    glm::vec2 p = std::get<glm::vec2>(input.value);
                    std::string labelX = input.name + "  X";
                    std::string labelY = input.name + "  Y";
                    auto rx = paramSlider("##px", labelX.c_str(), &p.x,
                                          input.minVec.x, input.maxVec.x, false);
                    auto ry = paramSlider("##py", labelY.c_str(), &p.y,
                                          input.minVec.y, input.maxVec.y, false);
                    if (rx.changed || ry.changed) input.value = p;
                    if (rx.activated || ry.activated) undoNeeded = true;
                } else if (input.type == "long") {
                    // Named enum → dropdown combo; numeric-only → paramSlider
                    // with integer format. Dropdowns keep long option lists
                    // (ease-in variants, blend modes, etc.) from taking up
                    // a wall of horizontal pills.
                    float v = std::get<float>(input.value);
                    int iv = (int)v;
                    if (!input.longLabels.empty()) {
                        ImGui::Dummy(ImVec2(0, 6));
                        dimLabel(input.name.c_str());

                        int cur = iv;
                        if (cur < 0) cur = 0;
                        if (cur > (int)input.longLabels.size() - 1)
                            cur = (int)input.longLabels.size() - 1;
                        const char* preview = input.longLabels[cur].c_str();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::PushID(input.name.c_str());
                        if (ImGui::BeginCombo("##longCombo", preview)) {
                            for (int i = 0; i < (int)input.longLabels.size(); i++) {
                                bool sel = (i == cur);
                                if (ImGui::Selectable(input.longLabels[i].c_str(), sel)) {
                                    input.value = (float)i;
                                    undoNeeded = true;
                                }
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();
                        ImGui::Dummy(ImVec2(0, 2));
                    } else {
                        float fv = (float)iv;
                        auto r = paramSlider("##val", input.name.c_str(),
                                             &fv, input.minVal, input.maxVal,
                                             false, "%.0f");
                        if (r.changed) input.value = (float)(int)(fv + 0.5f);
                        if (r.activated) undoNeeded = true;
                    }
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
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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
                    // Image input — dropdown to pick a layer as texture source.
                    // Common ShaderClaw placeholder names ("inputTex",
                    // "inputImage", "input", "tex", "sourceTex", "iChannel0")
                    // all render as a friendlier "Texture" label so every
                    // shader that accepts one reads consistently. Non-generic
                    // names (e.g. "from"/"to" on transitions, or descriptive
                    // per-shader names) keep the original label.
                    auto isGenericImageName = [](const std::string& n) {
                        static const char* generics[] = {
                            "inputTex", "inputtex", "inputImage", "inputimage",
                            "input", "tex", "texture",
                            "sourceTex", "sourcetex", "source",
                            "iChannel0", "ichannel0",
                            "image",
                        };
                        for (const char* g : generics) if (n == g) return true;
                        return false;
                    };
                    const char* displayLabel = isGenericImageName(input.name)
                                               ? "Texture"
                                               : input.name.c_str();
                    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                    ImGui::Text("%s", displayLabel);
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
            } // end sectionHeader("Parameters")

            // --- Transition (moved to the BOTTOM of Parameters) ---
            // Transition type + duration + optional shader-transition block.
            // Rendered after all shader parameters so scrolling reveals the
            // transition controls as a "next step" rather than a header.
            {
                static const char* transLabels[(int)TransitionType::COUNT] = {};
                for (int i = 0; i < (int)TransitionType::COUNT; i++)
                    transLabels[i] = transitionTypeName((TransitionType)i);
                int curT = (int)layer->transitionType;
                ImGui::Dummy(ImVec2(0, 10));
                dimLabel("Transition");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##TransType", transLabels[curT])) {
                    for (int i = 0; i < (int)TransitionType::COUNT; i++) {
                        bool sel = (i == curT);
                        if (ImGui::Selectable(transLabels[i], sel)) {
                            layer->transitionType = (TransitionType)i;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                pillSlider("Duration", &layer->transitionDuration, 0.0f, 5.0f, "%.2fs");

                if (layer->transitionType == TransitionType::Shader) {
                    static char pathBuf[512];
                    static const Layer* lastLayer = nullptr;
                    static std::string lastPath;
                    if (lastLayer != layer.get() || lastPath != layer->transitionShaderPath) {
                        std::snprintf(pathBuf, sizeof(pathBuf), "%s", layer->transitionShaderPath.c_str());
                        lastLayer = layer.get();
                        lastPath = layer->transitionShaderPath;
                    }
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##TransShader", pathBuf, sizeof(pathBuf))) {
                        layer->transitionShaderPath = pathBuf;
                        layer->transitionShaderInst.reset();
                    }
                    if (ImGui::SmallButton("Dissolve")) {
                        layer->transitionShaderPath = "shaders/transitions/dissolve_noise.fs";
                        layer->transitionShaderInst.reset();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Wet Paint")) {
                        layer->transitionShaderPath = "shaders/transitions/wet_paint.fs";
                        layer->transitionShaderInst.reset();
                    }

                    if (layerStack) {
                        static int triggerTargetIdx = -1;
                        const char* preview = "<pick source layer>";
                        if (triggerTargetIdx >= 0 && triggerTargetIdx < layerStack->count() &&
                            (*layerStack)[triggerTargetIdx].get() != layer.get()) {
                            preview = (*layerStack)[triggerTargetIdx]->name.c_str();
                        }
                        ImGui::SetNextItemWidth(-60.0f);
                        if (ImGui::BeginCombo("##TransB", preview)) {
                            for (int li = 0; li < layerStack->count(); li++) {
                                auto other = (*layerStack)[li];
                                if (!other || other.get() == layer.get() || !other->source) continue;
                                bool sel = (triggerTargetIdx == li);
                                if (ImGui::Selectable(other->name.c_str(), sel)) triggerTargetIdx = li;
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        bool canTrigger = triggerTargetIdx >= 0 && triggerTargetIdx < layerStack->count()
                                          && (*layerStack)[triggerTargetIdx].get() != layer.get()
                                          && (*layerStack)[triggerTargetIdx]->source
                                          && !layer->transitionShaderPath.empty()
                                          && !layer->shaderTransitionActive;
                        if (!canTrigger) ImGui::BeginDisabled();
                        if (ImGui::Button("Trigger", ImVec2(-FLT_MIN, 0))) {
                            layer->startShaderTransition((*layerStack)[triggerTargetIdx]->source);
                        }
                        if (!canTrigger) ImGui::EndDisabled();
                    }
                }
            }
        }

#ifdef HAS_WHISPER
        if (speech && speech->available && speech->whisper) {
            auto& devices = speech->whisper->captureDevices();
            if (!devices.empty()) {
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
            ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
            ImGui::TextWrapped("%s", shaderSrc->description().c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}
