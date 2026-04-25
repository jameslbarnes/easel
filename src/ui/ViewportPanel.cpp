#include "ui/ViewportPanel.h"
#include "ui/UIManager.h"
#include "app/OutputZone.h"
#include "app/MappingProfile.h"
#include "app/ProjectorOutput.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"
#include "warp/ObjMeshWarp.h"
#include "compositing/MaskPath.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Zone color palette (shared with LayerPanel)
struct ZoneRGB { int r, g, b; };
static const ZoneRGB kZoneColors[] = {
    {0, 200, 255},    // cyan
    {255, 140, 50},   // orange
    {80, 220, 120},   // green
    {220, 80, 200},   // magenta
    {255, 220, 60},   // yellow
    {100, 140, 255},  // blue
    {255, 100, 100},  // red
    {180, 180, 180},  // grey
};
static ZoneRGB zoneRGB(int idx) { return kZoneColors[idx % 8]; }

// Zone-colored accent helpers
static ImU32 zAccent(int zi)      { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 255); }
static ImU32 zAccentDim(int zi)   { auto c = zoneRGB(zi); return IM_COL32(c.r*7/10, c.g*7/10, c.b*7/10, 255); }
static ImU32 zAccentSoft(int zi)  { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 80); }
static ImU32 zAccentGlow(int zi)  { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 30); }
static ImU32 zMaskFill(int zi)    { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 30); }
static ImU32 zMaskCurve(int zi)   { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 200); }
static ImU32 zMaskGlow(int zi)    { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 50); }
static ImU32 zHandleRing(int zi)  { auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 200); }
static ImU32 zSelectedFill(int zi){ auto c = zoneRGB(zi); return IM_COL32(c.r, c.g, c.b, 255); }
static ImU32 zPointFill(int zi)   { auto c = zoneRGB(zi); return IM_COL32(c.r*9/10, c.g*9/10, c.b*9/10, 255); }

// Fallback static colors (zone-independent)
static const ImU32 kAccent        = IM_COL32(255, 255, 255, 255);
static const ImU32 kAccentDim     = IM_COL32(0, 140, 180, 255);
static const ImU32 kAccentSoft    = IM_COL32(255, 255, 255, 80);
static const ImU32 kAccentGlow    = IM_COL32(255, 255, 255, 30);
static const ImU32 kWhiteSoft     = IM_COL32(255, 255, 255, 140);
static const ImU32 kHandleOuter   = IM_COL32(255, 255, 255, 220);
static const ImU32 kMaskFill      = IM_COL32(255, 255, 255, 30);
static const ImU32 kMaskCurve     = IM_COL32(255, 255, 255, 200);
static const ImU32 kMaskCurveGlow = IM_COL32(255, 255, 255, 50);
static const ImU32 kHandleLine    = IM_COL32(255, 255, 255, 70);
static const ImU32 kHandleDot     = IM_COL32(255, 255, 255, 200);
static const ImU32 kHandleRing    = IM_COL32(255, 255, 255, 200);
static const ImU32 kSelectedFill  = IM_COL32(0, 220, 255, 255);
static const ImU32 kSelectedRing  = IM_COL32(255, 255, 255, 255);
static const ImU32 kPointFill     = IM_COL32(0, 180, 230, 255);
static const ImU32 kPointRing     = IM_COL32(255, 255, 255, 180);
static const ImU32 kBorderColor   = IM_COL32(255, 255, 255, 20);
static const ImU32 kBorderGlow    = IM_COL32(0, 0, 0, 0);
static const ImU32 kBBoxLine      = IM_COL32(50, 130, 255, 200);
static const ImU32 kBBoxGlow      = IM_COL32(50, 130, 255, 40);
static const ImU32 kBBoxDim       = IM_COL32(255, 255, 255, 30);
static const ImU32 kLHandleFill   = IM_COL32(255, 255, 255, 240);
static const ImU32 kLHandleStroke = IM_COL32(255, 255, 255, 255);
static const ImU32 kLHandleActive = IM_COL32(0, 220, 255, 255);

glm::vec2 ViewportPanel::screenToUV(glm::vec2 screen) const {
    return glm::vec2(
        (screen.x - m_imageOrigin.x) / m_imageSize.x,
        1.0f - (screen.y - m_imageOrigin.y) / m_imageSize.y);
}

glm::vec2 ViewportPanel::uvToScreenVec(glm::vec2 uv) const {
    return glm::vec2(
        m_imageOrigin.x + uv.x * m_imageSize.x,
        m_imageOrigin.y + (1.0f - uv.y) * m_imageSize.y);
}

glm::vec2 ViewportPanel::screenToNDC(glm::vec2 screen) const {
    return glm::vec2(
        ((screen.x - m_imageOrigin.x) / m_imageSize.x) * 2.0f - 1.0f,
        1.0f - ((screen.y - m_imageOrigin.y) / m_imageSize.y) * 2.0f);
}

glm::vec2 ViewportPanel::ndcToScreen(glm::vec2 ndc) const {
    return glm::vec2(
        m_imageOrigin.x + (ndc.x * 0.5f + 0.5f) * m_imageSize.x,
        m_imageOrigin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_imageSize.y);
}

static ImVec2 toImVec2(glm::vec2 v) { return ImVec2(v.x, v.y); }

#include <functional>

void ViewportPanel::render(GLuint texture, MappingProfile* mapping,
                           float projectorAspect,
                           std::vector<std::unique_ptr<OutputZone>>* zones,
                           int* activeZone,
                           const std::vector<MonitorInfo>* monitors,
                           bool ndiAvailable,
                           int editorMonitor,
                           const std::vector<std::unique_ptr<MappingProfile>>* allMappings,
                           std::function<void()> inlineSetupSection) {
    // Unpack mapping for warp overlay
    WarpMode warpMode = mapping ? mapping->warpMode : WarpMode::CornerPin;
    CornerPinWarp* cornerPinPtr = mapping ? &mapping->cornerPin : nullptr;
    MeshWarp* meshWarpPtr = mapping ? &mapping->meshWarp : nullptr;
    ObjMeshWarp* objMeshWarp = mapping ? &mapping->objMeshWarp : nullptr;
    // Skip the whole Canvas render when the workspace is flipped to
    // Stage. Only one of the two windows submits per frame, so the
    // dock never shows a tab bar.
    if (UIManager::sShowStage) { return; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Canvas");
    ImGui::PopStyleVar();

    // Track panel visibility and bounds for overlay clipping
    // Check if this window is actually the visible/selected tab in its dock node
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    bool isDockTabVisible = true;
    if (win->DockNode && win->DockNode->VisibleWindow != win)
        isDockTabVisible = false;
    m_panelVisible = isDockTabVisible && !ImGui::IsWindowCollapsed();
    ImVec2 wMin = ImGui::GetWindowContentRegionMin();
    ImVec2 wMax = ImGui::GetWindowContentRegionMax();
    ImVec2 wPos = ImGui::GetWindowPos();
    m_panelMin = {wPos.x + wMin.x, wPos.y + wMin.y};
    m_panelMax = {wPos.x + wMax.x, wPos.y + wMax.y};

    // Zone tab bar + output routing — always visible. Delegated to the
    // shared renderNavBar so the Stage panel can render an identical row
    // and switching workspaces doesn't shift element positions.
    if (zones && activeZone) {
        renderNavBar(false, zones, activeZone, monitors, ndiAvailable, editorMonitor);
        ImDrawList* tabDraw = ImGui::GetWindowDrawList();
        // Subtle separator line before preview (kept here because the
        // viewport texture draws immediately below).
        ImGui::Dummy(ImVec2(0, 2));
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            tabDraw->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
                             IM_COL32(255, 255, 255, 25));
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (inlineSetupSection) {
            ImGui::Indent(6);
            inlineSetupSection();
            ImGui::Unindent(6);
        }
    }

    // --- legacy inline nav (kept disabled) ---
#if 0
    if (zones && activeZone) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::Indent(6);

        ImDrawList* tabDraw = ImGui::GetWindowDrawList();

        // --- Canvas / Stage pill — reads/writes UIManager::sShowStage
        //     to swap between workspaces. Only one of Canvas/Stage has
        //     its Begin() submitted per frame (guarded in Application),
        //     so the dock never shows a tab bar and the button is
        //     guaranteed to work.
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 7));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
            auto pill = [&](const char* label, bool active) {
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.96f, 0.97f, 1.00f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.98f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.05f, 0.07f, 0.10f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.96f, 0.97f, 1.00f, 1.00f));
                } else {
                    // Inactive = grey outline, no fill.
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.10f));
                    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.80f, 0.82f, 0.88f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.45f, 0.48f, 0.54f, 0.85f));
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                bool clicked = ImGui::Button(label);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(5);
                return clicked;
            };
            // We're inside Canvas::render, so Canvas is the active pill.
            // Clicking Stage flips the workspace flag for next frame.
            pill("CANVAS", true);
            ImGui::SameLine(0, 6);
            if (pill("STAGE", false)) UIManager::sShowStage = true;
            ImGui::PopStyleVar(3);
        }

        // --- Right-align the rest of the nav row (zones → + → OUTPUT → combo
        //     → composition chip). Compute the cluster's total width and jump
        //     the cursor over the gap so only CANVAS/STAGE stay on the left.
        {
            const float kPadX        = 14.0f * 2.0f;  // matches FramePadding
            const float kZoneSpacing = 4.0f;
            const float kSectionGap  = 14.0f;
            const float kInnerGap    = 8.0f;
            const float kComboW      = 150.0f;
            const float kDotW        = 12.0f;
            float rightW = 0.0f;
            if (zones) {
                for (auto& zp : *zones)
                    rightW += ImGui::CalcTextSize(zp->name.c_str()).x + kPadX + kZoneSpacing;
            }
            rightW += ImGui::CalcTextSize("+").x + kPadX;
            rightW += kSectionGap + kDotW + kInnerGap;
            rightW += ImGui::CalcTextSize("OUTPUT").x + kInnerGap;
            rightW += kComboW + kInnerGap;
            int aiLookup = *activeZone;
            if (aiLookup >= 0 && aiLookup < (int)zones->size()) {
                char compLabel[48];
                snprintf(compLabel, sizeof(compLabel), "%d x %d",
                         (*zones)[aiLookup]->width, (*zones)[aiLookup]->height);
                rightW += ImGui::CalcTextSize(compLabel).x + kPadX;
            } else {
                rightW += 120.0f;
            }
            rightW += kInnerGap;
            // Fullscreen button width — worst case is "Fullscreen"
            rightW += ImGui::CalcTextSize("Fullscreen").x + kPadX;
            // Right-edge breathing room — mirrors the ~12px CANVAS pill has
            // on the left, plus slack for scrollbar / sub-pixel / combo arrow.
            rightW += 90.0f;

            ImGui::SameLine();
            float targetX = ImGui::GetContentRegionMax().x - rightW;
            float curX    = ImGui::GetCursorPosX();
            if (targetX > curX) ImGui::SetCursorPosX(targetX);
        }

        // --- Zone tabs ---
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
        for (int i = 0; i < (int)zones->size(); i++) {
            ImGui::PushID(9000 + i);
            bool isActive = (i == *activeZone);
            auto& z = *(*zones)[i];

            // Per-zone color from shared palette (matches layer panel dots)
            static const float zoneColors[][3] = {
                // Monochrome zone tabs — lightness ramp only
                {0.96f, 0.96f, 0.96f}, {0.86f, 0.86f, 0.86f}, {0.76f, 0.76f, 0.76f}, {0.66f, 0.66f, 0.66f},
                {0.56f, 0.56f, 0.56f}, {0.80f, 0.80f, 0.80f}, {0.70f, 0.70f, 0.70f}, {0.60f, 0.60f, 0.60f},
            };
            const float* zc = zoneColors[i % 8];
            if (isActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0], zc[1], zc[2], 0.25f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0], zc[1], zc[2], 0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0] * 0.15f, zc[1] * 0.15f, zc[2] * 0.15f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0] * 0.25f, zc[1] * 0.25f, zc[2] * 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0] * 0.6f, zc[1] * 0.6f, zc[2] * 0.6f, 1.0f));
            }
            if (ImGui::Button(z.name.c_str())) {
                *activeZone = i;
            }
            ImGui::PopStyleColor(3);

            // Double-click to rename
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                m_renaming = true;
                m_renameIndex = i;
                strncpy(m_renameBuf, z.name.c_str(), sizeof(m_renameBuf) - 1);
                m_renameBuf[sizeof(m_renameBuf) - 1] = '\0';
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem("ZoneTabCtx")) {
                if (ImGui::MenuItem("Rename")) {
                    m_renaming = true;
                    m_renameIndex = i;
                    strncpy(m_renameBuf, z.name.c_str(), sizeof(m_renameBuf) - 1);
                    m_renameBuf[sizeof(m_renameBuf) - 1] = '\0';
                }
                if (ImGui::MenuItem("Duplicate")) {
                    *activeZone = -(200 + i); // signal: duplicate zone i
                }
                if ((int)zones->size() > 1) {
                    if (ImGui::MenuItem("Remove")) {
                        *activeZone = -(300 + i); // signal: remove zone i
                    }
                }
                ImGui::EndPopup();
            }

            // Output type dot on tab (top-left corner) — uses zone color
            ImVec2 btnMin = ImGui::GetItemRectMin();
            if (z.outputDest == OutputDest::Fullscreen || z.outputDest == OutputDest::NDI) {
                ImU32 dotCol = IM_COL32((int)(zc[0]*255), (int)(zc[1]*255), (int)(zc[2]*255), 255);
                tabDraw->AddCircleFilled(ImVec2(btnMin.x + 5, btnMin.y + 5), 3.0f, dotCol);
            }
            ImGui::SameLine();
            ImGui::PopID();
        }

        // Rename popup
        if (m_renaming) {
            ImGui::OpenPopup("##RenameZone");
        }
        if (ImGui::BeginPopup("##RenameZone")) {
            ImGui::Text("Rename Zone");
            ImGui::SetNextItemWidth(200);
            bool enter = ImGui::InputText("##RenameInput", m_renameBuf, sizeof(m_renameBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (m_renaming) {
                ImGui::SetKeyboardFocusHere(-1);
                m_renaming = false; // only set focus once
            }
            if (enter || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (enter && m_renameIndex >= 0 && m_renameIndex < (int)zones->size() && m_renameBuf[0]) {
                    (*zones)[m_renameIndex]->name = m_renameBuf;
                }
                m_renameIndex = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // "+" button to add zone
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
        if (ImGui::Button("+")) {
            *activeZone = -(100 + (int)zones->size()); // signal: want add
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add output zone");
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        // --- Output + Mapping routing (same row as zone tabs) ---
        int ai = *activeZone;
        if (ai >= 0 && ai < (int)zones->size()) {
            auto& az = *(*zones)[ai];

            // Match the pill / zone-tab vertical metrics so the whole
            // nav row baselines cleanly instead of stair-stepping.
            // Right-alignment of the whole cluster (zones + OUTPUT + chip)
            // is handled earlier — see the SameLine(pushX) jump above.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));
            ImGui::SameLine(0, 14);

            // Build display label
            static char destBuf[128] = {};
            const char* destLabel = "Preview Only";
            if (az.outputDest == OutputDest::Fullscreen && monitors) {
                int mi = az.outputMonitor;
                if (mi >= 0 && mi < (int)monitors->size()) {
                    snprintf(destBuf, sizeof(destBuf), "Fullscreen: %s", (*monitors)[mi].name.c_str());
                    destLabel = destBuf;
                }
            } else if (az.outputDest == OutputDest::NDI) {
                snprintf(destBuf, sizeof(destBuf), "NDI: \"%s\"",
                         az.ndiStreamName.empty() ? az.name.c_str() : az.ndiStreamName.c_str());
                destLabel = destBuf;
            }

            // "Output" label with status dot. Dot Y aligns to the framed
            // widget centre (not text baseline) so it sits on the same line
            // as the combo and zone pills.
            bool live = (az.outputDest != OutputDest::None);
            ImVec2 dotPos = ImGui::GetCursorScreenPos();
            float frameH = ImGui::GetFrameHeight();
            float dotCY = dotPos.y + frameH * 0.5f;
            if (live) {
                tabDraw->AddCircleFilled(ImVec2(dotPos.x + 4, dotCY),
                                         3.5f, IM_COL32(34, 210, 130, 255));
                tabDraw->AddCircle(ImVec2(dotPos.x + 4, dotCY),
                                   5.5f, IM_COL32(34, 210, 130, 40));
            } else {
                tabDraw->AddCircle(ImVec2(dotPos.x + 4, dotCY),
                                   3.0f, IM_COL32(100, 110, 130, 140), 0, 1.2f);
            }
            ImGui::Dummy(ImVec2(12, frameH));
            ImGui::SameLine();

            ImGui::AlignTextToFramePadding();
            if (live) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            }
            ImGui::TextUnformatted("OUTPUT");
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Combo inherits accent color when live
            if (live) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.73f, 0.78f, 1.0f));
            }

            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##ZoneOutput", destLabel, ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Preview Only", az.outputDest == OutputDest::None)) {
                    az.outputDest = OutputDest::None;
                    az.outputMonitor = -1;
                }

                if (monitors && !monitors->empty()) {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                    ImGui::Text("  Fullscreen");
                    ImGui::PopStyleColor();
                    for (int mi = 0; mi < (int)monitors->size(); mi++) {
                        ImGui::PushID(mi);

                        // Skip the editor's own monitor
                        bool isEditorMonitor = (mi == editorMonitor);
                        if (isEditorMonitor) {
                            ImGui::PopID();
                            continue;
                        }

                        // Check if another zone claims this monitor
                        std::string claimedBy;
                        for (int zi = 0; zi < (int)zones->size(); zi++) {
                            if (zi == ai) continue;
                            auto& oz = *(*zones)[zi];
                            if (oz.outputDest == OutputDest::Fullscreen && oz.outputMonitor == mi) {
                                claimedBy = oz.name;
                                break;
                            }
                        }

                        char label[256];
                        if (!claimedBy.empty()) {
                            snprintf(label, sizeof(label), "%s  %dx%d  (-> %s)",
                                     (*monitors)[mi].name.c_str(),
                                     (*monitors)[mi].width, (*monitors)[mi].height,
                                     claimedBy.c_str());
                        } else {
                            snprintf(label, sizeof(label), "%s  %dx%d",
                                     (*monitors)[mi].name.c_str(),
                                     (*monitors)[mi].width, (*monitors)[mi].height);
                        }

                        // Dim text for claimed monitors
                        if (!claimedBy.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.6f));
                        }
                        bool sel = (az.outputDest == OutputDest::Fullscreen && az.outputMonitor == mi);
                        if (ImGui::Selectable(label, sel)) {
                            // Steal: set the other zone to None
                            if (!claimedBy.empty()) {
                                for (int zi = 0; zi < (int)zones->size(); zi++) {
                                    if (zi == ai) continue;
                                    auto& oz = *(*zones)[zi];
                                    if (oz.outputDest == OutputDest::Fullscreen && oz.outputMonitor == mi) {
                                        oz.outputDest = OutputDest::None;
                                        oz.outputMonitor = -1;
                                        break;
                                    }
                                }
                            }
                            az.outputDest = OutputDest::Fullscreen;
                            az.outputMonitor = mi;
                        }
                        if (!claimedBy.empty()) {
                            ImGui::PopStyleColor();
                        }
                        ImGui::PopID();
                    }
                }

                if (ndiAvailable) {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                    ImGui::Text("  NDI");
                    ImGui::PopStyleColor();
                    char ndiLabel[256];
                    std::string streamName = az.ndiStreamName.empty() ? az.name : az.ndiStreamName;
                    snprintf(ndiLabel, sizeof(ndiLabel), "Easel - %s", streamName.c_str());
                    bool sel = (az.outputDest == OutputDest::NDI);
                    if (ImGui::Selectable(ndiLabel, sel)) {
                        az.outputDest = OutputDest::NDI;
                        if (az.ndiStreamName.empty()) az.ndiStreamName = az.name;
                    }
                }

                ImGui::EndCombo();
            }
            ImGui::PopStyleColor(); // combo text color

            // MAPPING combo removed — the Mapping inspector tab has its
            // own profile picker; duplicating it here was noise.
            // COMPOSITION moved to a popover on the zone tab (see below).

            // --- Composition size as a clickable chip, right-anchored on
            //     the same row so the size of the active zone is always
            //     visible. Clicking opens a preset picker.
            {
                static const char* presetLabels[] = {
                    "1920x1080 (1080p)", "3840x2160 (4K)", "1280x720 (720p)",
                    "2560x1440 (1440p)", "8000x2000 (Ultra-wide)", "1024x768", "Custom"
                };
                static const int presetW[] = { 1920, 3840, 1280, 2560, 8000, 1024, 0 };
                static const int presetH[] = { 1080, 2160, 720, 1440, 2000, 768, 0 };
                const int presetCount = 7;

                ImGui::SameLine(0, 12);
                char compLabel[48];
                snprintf(compLabel, sizeof(compLabel), "%d x %d", az.width, az.height);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.65f, 0.68f, 0.74f, 1.0f));
                if (ImGui::Button(compLabel)) ImGui::OpenPopup("##CompPreset");
                ImGui::PopStyleColor(3);
                // Fullscreen toggle — sits inline with the composition chip
                // so "display setup" actions (pick output size, go fullscreen)
                // cluster together instead of living in a separate menu row.
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.75f, 0.78f, 0.84f, 1.0f));
                const char* fsLbl = m_editorFullscreenHint ? "Exit FS" : "Fullscreen";
                if (ImGui::Button(fsLbl)) m_wantsFullscreenToggle = true;
                ImGui::PopStyleColor(3);

                if (ImGui::BeginPopup("##CompPreset")) {
                    for (int p = 0; p < presetCount - 1; p++) {
                        bool sel = (az.width == presetW[p] && az.height == presetH[p]);
                        if (ImGui::Selectable(presetLabels[p], sel)) {
                            az.resize(presetW[p], presetH[p]);
                            az.compPreset = p;
                        }
                    }
                    ImGui::Separator();
                    // Custom... — inline W x H inputs with Apply so arbitrary
                    // composition sizes are actually reachable (previously
                    // the popup just set a preset index and closed).
                    static int s_customCompW = 1920, s_customCompH = 1080;
                    static int s_customCompZone = -1;
                    if (s_customCompZone != ai) {
                        s_customCompW = az.width > 0 ? az.width : 1920;
                        s_customCompH = az.height > 0 ? az.height : 1080;
                        s_customCompZone = ai;
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                    ImGui::TextUnformatted("Custom");
                    ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt("##compCW", &s_customCompW, 0);
                    ImGui::SameLine();
                    ImGui::TextDisabled("x");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt("##compCH", &s_customCompH, 0);
                    ImGui::SameLine();
                    bool valid = s_customCompW >= 64 && s_customCompH >= 64
                              && s_customCompW <= 16384 && s_customCompH <= 16384;
                    if (!valid) ImGui::BeginDisabled();
                    if (ImGui::SmallButton("Apply")) {
                        az.resize(s_customCompW, s_customCompH);
                        az.compPreset = presetCount - 1;
                        ImGui::CloseCurrentPopup();
                    }
                    if (!valid) ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
            }

            ImGui::PopStyleVar(2);
        }

        ImGui::Unindent(6);
    }
#endif

    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_size = {avail.x, avail.y};

    if (texture && avail.x > 1 && avail.y > 1) {
        // Base image size (fit to panel)
        float panelAspect = avail.x / avail.y;
        float baseW, baseH;
        if (projectorAspect > panelAspect) {
            baseW = avail.x; baseH = avail.x / projectorAspect;
        } else {
            baseH = avail.y; baseW = avail.y * projectorAspect;
        }

        // Apply zoom
        float imgW = baseW * m_zoom;
        float imgH = baseH * m_zoom;
        float offsetX = (avail.x - imgW) * 0.5f + m_pan.x;
        float offsetY = (avail.y - imgH) * 0.5f + m_pan.y;

        ImVec2 cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursor.x + offsetX, cursor.y + offsetY));
        ImGui::Image((ImTextureID)(intptr_t)texture, ImVec2(imgW, imgH),
                     ImVec2(0, 1), ImVec2(1, 0));

        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        m_imageOrigin = {imgMin.x, imgMin.y};
        m_imageSize = {imgMax.x - imgMin.x, imgMax.y - imgMin.y};

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect(ImVec2(imgMin.x - 2, imgMin.y - 2),
                      ImVec2(imgMax.x + 2, imgMax.y + 2), kBorderGlow, 2.0f, 0, 4.0f);
        draw->AddRect(ImVec2(imgMin.x - 1, imgMin.y - 1),
                      ImVec2(imgMax.x + 1, imgMax.y + 1), kBorderColor, 1.0f, 0, 1.0f);
    }

    m_hovered = ImGui::IsWindowHovered();

    // Safety net: if NO mouse buttons are down and we still think we're mid-drag,
    // clear the flags. This prevents the canvas from getting stuck unresponsive
    // after the user releases the mouse over a different window (e.g. clicking a
    // zone tab mid-drag, or dragging off the viewport onto another panel).
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (m_panDragging)   m_panDragging = false;
        if (m_layerDragging) { m_layerDragging = false; m_handleDrag = HandleType::None; }
        if (m_warpDragging)  { m_warpDragging = false;  m_warpDragIndex = -1; }
        if (m_orbitDragging) m_orbitDragging = false;
        if (m_maskDragType > 0) { m_maskDragType = 0; m_maskDragIndex = -1; }
    }

    // --- Canvas zoom (scroll wheel) and pan (middle-mouse drag) ---
    if (m_hovered && warpMode != WarpMode::ObjMesh) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float oldZoom = m_zoom;
            m_zoom = std::max(0.25f, std::min(8.0f, m_zoom + scroll * 0.15f * m_zoom));
            // Scale pan to keep view centered at same point
            if (oldZoom > 0.0f) {
                float ratio = m_zoom / oldZoom;
                m_pan.x *= ratio;
                m_pan.y *= ratio;
            }
        }
    }

    // Middle-mouse pan
    if (m_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_panDragging = true;
        ImVec2 mp = ImGui::GetMousePos();
        m_panDragStart = {mp.x, mp.y};
        m_panStart = m_pan;
    }
    if (m_panDragging && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        ImVec2 mp = ImGui::GetMousePos();
        m_pan.x = m_panStart.x + (mp.x - m_panDragStart.x);
        m_pan.y = m_panStart.y + (mp.y - m_panDragStart.y);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
        m_panDragging = false;
    }

    // Double-click middle mouse to reset zoom/pan
    if (m_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
        m_zoom = 1.0f;
        m_pan = {0, 0};
    }

    // --- Draw warp handles (always visible, not in mask mode) ---
    if (m_editMode != EditMode::Mask && m_imageSize.x > 0 && m_imageSize.y > 0) {
        if (warpMode == WarpMode::ObjMesh && objMeshWarp) {
            // --- Orbit camera interaction ---
            auto& cam = objMeshWarp->camera();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered && !m_layerDragging) {
                m_orbitDragging = true;
                ImVec2 mp = ImGui::GetMousePos();
                m_orbitDragStart = {mp.x, mp.y};
                m_orbitStartAzimuth = cam.azimuth;
                m_orbitStartElevation = cam.elevation;
            }

            if (m_orbitDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mp = ImGui::GetMousePos();
                float dx = mp.x - m_orbitDragStart.x;
                float dy = mp.y - m_orbitDragStart.y;
                cam.azimuth = m_orbitStartAzimuth - dx * 0.005f;
                cam.elevation = std::max(-1.5f, std::min(1.5f,
                    m_orbitStartElevation + dy * 0.005f));
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                m_orbitDragging = false;
            }

            // Scroll zoom
            if (m_hovered) {
                float scroll = ImGui::GetIO().MouseWheel;
                if (scroll != 0.0f) {
                    cam.distance = std::max(0.5f, std::min(20.0f,
                        cam.distance - scroll * 0.3f));
                }
            }

            // Overlay label
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 labelPos(m_imageOrigin.x + 8, m_imageOrigin.y + m_imageSize.y - 20);
            draw->AddText(labelPos, kWhiteSoft, "Orbit | drag to rotate | scroll to zoom");
        } else {
            // --- CornerPin / MeshWarp handle interaction ---
            ImVec2 mousePos = ImGui::GetMousePos();
            glm::vec2 mouseNDC = screenToNDC({mousePos.x, mousePos.y});

            // Warp handles get priority over layer handles (smaller hit targets)
            if (cornerPinPtr && meshWarpPtr && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered) {
                int hit = -1;
                if (warpMode == WarpMode::CornerPin) {
                    hit = cornerPinPtr->hitTest(mouseNDC);
                } else {
                    hit = meshWarpPtr->hitTest(mouseNDC);
                }
                if (hit >= 0) {
                    m_warpDragIndex = hit;
                    m_warpDragging = true;
                }
            }

            if (m_warpDragging && cornerPinPtr && meshWarpPtr && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                glm::vec2 clamped(std::max(-1.5f, std::min(1.5f, mouseNDC.x)),
                                  std::max(-1.5f, std::min(1.5f, mouseNDC.y)));
                if (warpMode == WarpMode::CornerPin) {
                    cornerPinPtr->corners()[m_warpDragIndex] = clamped;
                } else {
                    meshWarpPtr->points()[m_warpDragIndex] = clamped;
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                m_warpDragging = false;
                m_warpDragIndex = -1;
            }

            // Draw warp handles (zone-colored)
            ImDrawList* draw = ImGui::GetWindowDrawList();
            auto ndc2scr = [&](glm::vec2 ndc) -> ImVec2 { return toImVec2(ndcToScreen(ndc)); };
            int zi = activeZone ? *activeZone : 0;

            if (warpMode == WarpMode::CornerPin && cornerPinPtr) {
                const auto& corners = cornerPinPtr->corners();

                for (int i = 0; i < 4; i++) {
                    draw->AddLine(ndc2scr(corners[i]), ndc2scr(corners[(i + 1) % 4]), zAccentGlow(zi), 6.0f);
                    draw->AddLine(ndc2scr(corners[i]), ndc2scr(corners[(i + 1) % 4]), zAccent(zi), 1.5f);
                }
                for (int i = 0; i < 4; i++) {
                    ImVec2 p = ndc2scr(corners[i]);
                    bool active = (m_warpDragging && m_warpDragIndex == i);
                    draw->AddCircleFilled(p, active ? 14.0f : 10.0f, zAccentGlow(zi));
                    draw->AddCircleFilled(p, active ? 8.0f : 6.0f, active ? zAccent(zi) : zAccentDim(zi));
                    draw->AddCircle(p, active ? 8.0f : 6.0f, kHandleOuter, 0, 1.5f);
                }
            } else if (meshWarpPtr) {
                const auto& points = meshWarpPtr->points();
                int cols = meshWarpPtr->cols(), rows = meshWarpPtr->rows();
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols - 1; c++)
                        draw->AddLine(ndc2scr(points[r*cols+c]), ndc2scr(points[r*cols+c+1]), zAccentSoft(zi), 1.0f);
                for (int c = 0; c < cols; c++)
                    for (int r = 0; r < rows - 1; r++)
                        draw->AddLine(ndc2scr(points[r*cols+c]), ndc2scr(points[(r+1)*cols+c]), zAccentSoft(zi), 1.0f);
                for (int i = 0; i < (int)points.size(); i++) {
                    ImVec2 p = ndc2scr(points[i]);
                    bool active = (m_warpDragging && m_warpDragIndex == i);
                    draw->AddCircleFilled(p, active ? 6.0f : 4.0f, active ? zAccent(zi) : zPointFill(zi));
                    draw->AddCircle(p, active ? 6.0f : 4.0f, kPointRing, 0, 1.2f);
                }
            }
        }
    }

    ImGui::End();
}

// ======== LAYER TRANSFORM OVERLAY ========

// Distance from point to line segment
static float pointToSegmentDist(ImVec2 p, ImVec2 a, ImVec2 b) {
    float abx = b.x-a.x, aby = b.y-a.y;
    float apx = p.x-a.x, apy = p.y-a.y;
    float t = (abx*apx + aby*apy) / (abx*abx + aby*aby + 1e-8f);
    t = std::max(0.0f, std::min(1.0f, t));
    float cx = a.x + t*abx, cy = a.y + t*aby;
    float dx = p.x-cx, dy = p.y-cy;
    return sqrtf(dx*dx + dy*dy);
}

void ViewportPanel::renderLayerOverlay(LayerStack& stack, int& selectedLayer, int canvasW, int canvasH) {
    if (m_imageSize.x <= 0 || m_imageSize.y <= 0) return;
    if (!m_panelVisible) return;
    if (m_editMode != EditMode::Normal) return;
    if (stack.count() == 0) {
        m_layerDragging = false;
        m_handleDrag = HandleType::None;
        selectedLayer = -1;
        return;
    }
    if (selectedLayer >= stack.count()) {
        selectedLayer = stack.count() - 1;
        m_layerDragging = false;
        m_handleDrag = HandleType::None;
    }

    bool warpBusy = m_warpDragging;
    // BackgroundDrawList so gizmo overlays sit ABOVE the canvas but BELOW
    // floating panels — previously used ForegroundDrawList which punched
    // through any panel docked over the viewport.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // Clip overlay drawing to viewport panel bounds
    draw->PushClipRect(ImVec2(m_panelMin.x, m_panelMin.y),
                       ImVec2(m_panelMax.x, m_panelMax.y), true);
    ImVec2 mouse = ImGui::GetMousePos();
    glm::vec2 mouseNDC = screenToNDC({mouse.x, mouse.y});
    const float handleR = 5.0f;
    const float hitR = 10.0f;
    const float edgeHitDist = 6.0f;

    // --- Helpers ---
    auto getCorners = [&](const std::shared_ptr<Layer>& layer, ImVec2 out[4]) {
        float sx = layer->scale.x * (layer->flipH ? -1.0f : 1.0f);
        float sy = layer->scale.y * (layer->flipV ? -1.0f : 1.0f);
        // Apply same aspect ratio correction as CompositeEngine::nativeScale
        // so bounding box matches rendered content
        bool mosaicFill = (layer->tileX > 1.0f || layer->tileY > 1.0f ||
                           layer->mosaicMode != MosaicMode::Mirror);
        if (!mosaicFill && layer->source && canvasW > 0 && canvasH > 0) {
            int lw = layer->width(), lh = layer->height();
            if (lw > 0 && lh > 0) {
                float srcAspect = (float)lw / lh;
                float canvasAspect = (float)canvasW / canvasH;
                sx *= srcAspect / canvasAspect;
            }
        }
        float rad = glm::radians(layer->rotation);
        float c = cosf(rad), s = sinf(rad);
        float px = layer->position.x, py = layer->position.y;
        glm::vec2 local[4] = {{-1,-1},{1,-1},{1,1},{-1,1}};
        for (int i = 0; i < 4; i++) {
            float lx = local[i].x * sx, ly = local[i].y * sy;
            float rx = lx*c - ly*s, ry = lx*s + ly*c;
            out[i] = toImVec2(ndcToScreen({rx + px, ry + py}));
        }
    };
    auto midPt = [](ImVec2 a, ImVec2 b) -> ImVec2 { return ImVec2((a.x+b.x)*0.5f, (a.y+b.y)*0.5f); };
    auto distPt = [](ImVec2 a, ImVec2 b) -> float { float dx=a.x-b.x, dy=a.y-b.y; return sqrtf(dx*dx+dy*dy); };
    auto cross2d = [](ImVec2 o, ImVec2 a, ImVec2 b) -> float { return (a.x-o.x)*(b.y-o.y)-(a.y-o.y)*(b.x-o.x); };
    auto inQuad = [&](ImVec2 c[4], ImVec2 pt) -> bool {
        bool pos=true, neg=true;
        for (int j=0; j<4; j++) { float cp=cross2d(c[j],c[(j+1)%4],pt); if(cp<0)pos=false; if(cp>0)neg=false; }
        return pos||neg;
    };
    HandleType handleTypes[8] = {
        HandleType::TopLeft, HandleType::Top, HandleType::TopRight, HandleType::Right,
        HandleType::BottomRight, HandleType::Bottom, HandleType::BottomLeft, HandleType::Left
    };
    auto getHandles = [&](ImVec2 c[4], ImVec2 out[8]) {
        out[0]=c[0]; out[1]=midPt(c[0],c[1]); out[2]=c[1]; out[3]=midPt(c[1],c[2]);
        out[4]=c[2]; out[5]=midPt(c[2],c[3]); out[6]=c[3]; out[7]=midPt(c[3],c[0]);
    };

    // --- Draw dim bboxes for non-selected layers ---
    for (int i = 0; i < stack.count(); i++) {
        if (!stack[i]->visible || !stack[i]->source || i == selectedLayer) continue;
        ImVec2 c[4]; getCorners(stack[i], c);
        // Subtle hover highlight when mouse is near
        bool hovering = inQuad(c, mouse) && m_hovered;
        ImU32 col = hovering ? IM_COL32(255, 255, 255, 80) : kBBoxDim;
        for (int j = 0; j < 4; j++)
            draw->AddLine(c[j], c[(j+1)%4], col, hovering ? 1.5f : 1.0f);
    }

    // --- Handle mouse-down for selection ---
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered && !warpBusy && !m_layerDragging) {
        bool hitSomething = false;

        // If there's a selected layer, check its handles first
        if (selectedLayer >= 0 && selectedLayer < stack.count() && stack[selectedLayer]->source) {
            ImVec2 selCorners[4], selHandles[8];
            getCorners(stack[selectedLayer], selCorners);
            getHandles(selCorners, selHandles);

            for (int h = 0; h < 8; h++) {
                if (distPt(mouse, selHandles[h]) < hitR) {
                    m_handleDrag = handleTypes[h];
                    m_layerDragging = true;
                    m_dragStartMouse = mouseNDC;
                    m_dragStartPos = stack[selectedLayer]->position;
                    m_dragStartScale = stack[selectedLayer]->scale;
                    m_dragStartRotation = stack[selectedLayer]->rotation;
                    m_dragStartRatio = stack[selectedLayer]->scale.x / std::max(0.001f, stack[selectedLayer]->scale.y);
                    hitSomething = true;
                    break;
                }
            }

            // Check body for move
            if (!hitSomething && inQuad(selCorners, mouse)) {
                m_handleDrag = HandleType::Move;
                m_layerDragging = true;
                m_dragStartMouse = mouseNDC;
                m_dragStartPos = stack[selectedLayer]->position;
                m_dragStartScale = stack[selectedLayer]->scale;
                m_dragStartRotation = stack[selectedLayer]->rotation;
                hitSomething = true;
            }
        }

        // Try selecting a different layer (top-to-bottom)
        if (!hitSomething) {
            bool found = false;
            for (int idx = stack.count()-1; idx >= 0; idx--) {
                if (!stack[idx]->visible || !stack[idx]->source) continue;
                ImVec2 c2[4]; getCorners(stack[idx], c2);
                if (inQuad(c2, mouse)) {
                    selectedLayer = idx;
                    m_handleDrag = HandleType::Move;
                    m_layerDragging = true;
                    m_dragStartMouse = mouseNDC;
                    m_dragStartPos = stack[idx]->position;
                    m_dragStartScale = stack[idx]->scale;
                    m_dragStartRotation = stack[idx]->rotation;
                    found = true;
                    break;
                }
            }
            // Clicked empty space: deselect
            if (!found) {
                selectedLayer = -1;
                m_layerDragging = false;
                m_handleDrag = HandleType::None;
            }
        }
    }

    // --- Double-click corner: enter mask edit mode ---
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_hovered && !warpBusy) {
        if (selectedLayer >= 0 && selectedLayer < stack.count() && stack[selectedLayer]->source) {
            ImVec2 selCorners[4];
            getCorners(stack[selectedLayer], selCorners);
            for (int j = 0; j < 4; j++) {
                if (distPt(mouse, selCorners[j]) < hitR * 1.5f) {
                    m_wantsMaskEdit = true;
                    m_layerDragging = false;
                    m_handleDrag = HandleType::None;
                    break;
                }
            }
            // Double-click on edge: enter mask and add point there
            if (!m_wantsMaskEdit) {
                for (int j = 0; j < 4; j++) {
                    float d = pointToSegmentDist(mouse, selCorners[j], selCorners[(j+1)%4]);
                    if (d < edgeHitDist * 2.0f) {
                        m_wantsMaskEdit = true;
                        // Convert click position to UV for mask point
                        m_maskEditClickUV = screenToUV({mouse.x, mouse.y});
                        m_layerDragging = false;
                        m_handleDrag = HandleType::None;
                        break;
                    }
                }
            }
        }
    }

    // --- Dragging ---
    if (m_layerDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        selectedLayer >= 0 && selectedLayer < stack.count()) {
        auto& dl = stack[selectedLayer];
        glm::vec2 delta = mouseNDC - m_dragStartMouse;
        bool shift = ImGui::GetIO().KeyShift;

        if (m_handleDrag == HandleType::Move) {
            dl->position = m_dragStartPos + delta;
        } else {
            // Rotate mouse delta into the layer's local coordinate space
            // so handle dragging works correctly at any rotation angle.
            // Use drag-start rotation so projection is stable if rotation
            // changes during the drag (e.g. audio bindings).
            float rad = glm::radians(m_dragStartRotation);
            float c = cosf(rad), s = sinf(rad);
            float dx = delta.x * c + delta.y * s;   // local X (along layer's right)
            float dy = -delta.x * s + delta.y * c;   // local Y (along layer's up)
            glm::vec2 ns = m_dragStartScale;

            switch (m_handleDrag) {
            case HandleType::TopLeft:     ns.x = std::max(0.05f, ns.x - dx*0.5f); ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
            case HandleType::TopRight:    ns.x = std::max(0.05f, ns.x + dx*0.5f); ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
            case HandleType::BottomLeft:  ns.x = std::max(0.05f, ns.x - dx*0.5f); ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
            case HandleType::BottomRight: ns.x = std::max(0.05f, ns.x + dx*0.5f); ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
            case HandleType::Top:    ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
            case HandleType::Bottom: ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
            case HandleType::Left:   ns.x = std::max(0.05f, ns.x - dx*0.5f); break;
            case HandleType::Right:  ns.x = std::max(0.05f, ns.x + dx*0.5f); break;
            default: break;
            }

            // Shift: constrain aspect ratio (lock to drag-start ratio)
            if (shift && m_dragStartRatio > 0.001f) {
                bool isCornerHandle = (m_handleDrag == HandleType::TopLeft || m_handleDrag == HandleType::TopRight ||
                                       m_handleDrag == HandleType::BottomLeft || m_handleDrag == HandleType::BottomRight);
                if (isCornerHandle) {
                    // Use the axis with the larger delta to drive both
                    float adx = fabsf(ns.x - m_dragStartScale.x);
                    float ady = fabsf(ns.y - m_dragStartScale.y);
                    if (adx > ady) {
                        ns.y = std::max(0.05f, ns.x / m_dragStartRatio);
                    } else {
                        ns.x = std::max(0.05f, ns.y * m_dragStartRatio);
                    }
                }
            }

            dl->scale = ns;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_layerDragging = false;
        m_handleDrag = HandleType::None;
    }

    // --- Draw selected layer ---
    if (selectedLayer >= 0 && selectedLayer < stack.count()) {
        auto& layer = stack[selectedLayer];
        if (layer->source) {
            ImVec2 corners[4];
            getCorners(layer, corners);

            // Bbox glow + line
            for (int j = 0; j < 4; j++)
                draw->AddLine(corners[j], corners[(j+1)%4], kBBoxGlow, 4.0f);
            for (int j = 0; j < 4; j++)
                draw->AddLine(corners[j], corners[(j+1)%4], kBBoxLine, 1.5f);

            // Edge hover highlight
            if (!m_layerDragging && m_hovered) {
                for (int j = 0; j < 4; j++) {
                    float d = pointToSegmentDist(mouse, corners[j], corners[(j+1)%4]);
                    if (d < edgeHitDist) {
                        draw->AddLine(corners[j], corners[(j+1)%4], IM_COL32(0, 220, 255, 160), 3.0f);
                    }
                }
            }

            // Handles
            ImVec2 handles[8];
            getHandles(corners, handles);

            for (int h = 0; h < 8; h++) {
                bool isCorner = (h % 2 == 0);
                float r = isCorner ? handleR : (handleR - 1.5f);
                bool active = (m_layerDragging && m_handleDrag == handleTypes[h]);
                bool hovered = (!m_layerDragging && distPt(mouse, handles[h]) < hitR && m_hovered);

                ImU32 fillCol = (active || hovered) ? IM_COL32(0, 230, 255, 255) : kLHandleFill;
                ImU32 strokeCol = active ? kLHandleActive : (hovered ? IM_COL32(0, 230, 255, 255) : kLHandleStroke);
                float drawR = (active || hovered) ? r + 1.5f : r;

                if (isCorner) {
                    ImVec2 hMin(handles[h].x-drawR, handles[h].y-drawR);
                    ImVec2 hMax(handles[h].x+drawR, handles[h].y+drawR);
                    draw->AddRectFilled(hMin, hMax, fillCol, 1.5f);
                    draw->AddRect(hMin, hMax, strokeCol, 1.5f, 0, 1.5f);
                } else {
                    draw->AddCircleFilled(handles[h], drawR, fillCol);
                    draw->AddCircle(handles[h], drawR, strokeCol, 0, 1.5f);
                }
            }

            // Cursor hints
            if (!m_layerDragging && !warpBusy && m_hovered) {
                bool setCursor = false;
                for (int h = 0; h < 8; h++) {
                    if (distPt(mouse, handles[h]) < hitR) {
                        if (h==0||h==4) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                        else if (h==2||h==6) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                        else if (h==1||h==5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                        else if (h==3||h==7) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        setCursor = true;
                        break;
                    }
                }
                // Edge hover cursor (crosshair for "add mask point")
                if (!setCursor) {
                    for (int j = 0; j < 4; j++) {
                        if (pointToSegmentDist(mouse, corners[j], corners[(j+1)%4]) < edgeHitDist) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            break;
                        }
                    }
                }
            }

            // Arrow key nudge (1 canvas pixel)
            if (!ImGui::GetIO().WantTextInput) {
                float nudgeX = 2.0f / (float)canvasW;
                float nudgeY = 2.0f / (float)canvasH;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  layer->position.x -= nudgeX;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) layer->position.x += nudgeX;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    layer->position.y += nudgeY;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  layer->position.y -= nudgeY;
            }
        }
    }

    draw->PopClipRect();
}

// ======== MASK OVERLAY ========

void ViewportPanel::renderMaskOverlay(MaskPath& mask, const glm::mat3& layerTransform, int zoneIndex) {
    if (m_imageSize.x <= 0 || m_imageSize.y <= 0) return;
    if (!m_panelVisible) return;
    if (m_editMode != EditMode::Mask) return;

    // Convert between canvas UV (what's displayed) and layer UV (what the mask stores).
    // Mask points are in layer UV; the viewport shows canvas UV.
    glm::mat3 invXform = glm::inverse(layerTransform);
    auto canvasToLayerUV = [&](glm::vec2 cuv) -> glm::vec2 {
        glm::vec2 ndc = cuv * 2.0f - 1.0f;
        glm::vec3 lndc = invXform * glm::vec3(ndc, 1.0f);
        return glm::vec2(lndc.x, lndc.y) * 0.5f + 0.5f;
    };
    auto layerToCanvasUV = [&](glm::vec2 luv) -> glm::vec2 {
        glm::vec2 ndc = luv * 2.0f - 1.0f;
        glm::vec3 cndc = layerTransform * glm::vec3(ndc, 1.0f);
        return glm::vec2(cndc.x, cndc.y) * 0.5f + 0.5f;
    };
    // Screen-to-layer and layer-to-screen convenience
    auto screenToLayerUV = [&](glm::vec2 screen) -> glm::vec2 {
        return canvasToLayerUV(screenToUV(screen));
    };
    auto layerUVToScreen = [&](glm::vec2 luv) -> glm::vec2 {
        return uvToScreenVec(layerToCanvasUV(luv));
    };


    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // Clip mask overlay drawing to viewport panel bounds
    draw->PushClipRect(ImVec2(m_panelMin.x, m_panelMin.y),
                       ImVec2(m_panelMax.x, m_panelMax.y), true);
    ImVec2 mousePos = ImGui::GetMousePos();
    glm::vec2 mouseUV = screenToLayerUV({mousePos.x, mousePos.y});

    // --- Compute banner rect early so clicks on the banner/Save button
    // don't leak through to the mask-point-add handler below. The banner
    // sits at the top-right of the viewport away from common drawing areas.
    ImVec2 bannerMin, bannerMax, saveBtnMin, saveBtnMax;
    {
        const char* banner = mask.closed()
            ? "MASK EDIT  |  Shift+Click: multi-select  •  Drag: move  •  R-click: del"
            : "MASK EDIT  |  Click: add points  •  Click first point to close  •  R-click: del";
        ImVec2 ts = ImGui::CalcTextSize(banner);
        const char* saveLbl = "Save";
        ImVec2 sts = ImGui::CalcTextSize(saveLbl);
        float pad = 8.0f;
        float saveBtnW = sts.x + 18.0f;
        float totalW = ts.x + pad * 2 + saveBtnW + 6.0f;
        float totalH = ts.y + pad;
        bannerMin = ImVec2(m_panelMax.x - totalW - 12, m_panelMin.y + 8);
        bannerMax = ImVec2(bannerMin.x + totalW, bannerMin.y + totalH);
        saveBtnMin = ImVec2(bannerMin.x + pad + ts.x + pad, bannerMin.y);
        saveBtnMax = ImVec2(saveBtnMin.x + saveBtnW, bannerMin.y + totalH);
    }
    auto ptInRect = [](ImVec2 p, ImVec2 a, ImVec2 b) {
        return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y;
    };
    bool mouseOverBanner = ptInRect(mousePos, bannerMin, bannerMax);

    bool shiftHeld = ImGui::GetIO().KeyShift;
    auto isSelected = [&](int idx) -> bool {
        for (int s : m_maskSelectedPoints) if (s == idx) return true;
        return false;
    };

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered && !mouseOverBanner) {
        int hi = mask.hitTestHandleIn(mouseUV, 0.025f);
        int ho = mask.hitTestHandleOut(mouseUV, 0.025f);
        int pt = mask.hitTestPoint(mouseUV, 0.03f);

        if (hi >= 0) {
            m_maskDragIndex = hi; m_maskDragType = 2; m_maskSelectedPoint = hi;
        } else if (ho >= 0) {
            m_maskDragIndex = ho; m_maskDragType = 3; m_maskSelectedPoint = ho;
        } else if (pt >= 0) {
            // Click on point 0 with 3+ points and open path → close the mask
            if (pt == 0 && !mask.closed() && mask.count() >= 3) {
                mask.setClosed(true);
                mask.markDirty();
                m_maskSelectedPoint = 0;
            } else {
                // Shift-click: toggle selection
                if (shiftHeld) {
                    if (isSelected(pt)) {
                        m_maskSelectedPoints.erase(
                            std::remove(m_maskSelectedPoints.begin(), m_maskSelectedPoints.end(), pt),
                            m_maskSelectedPoints.end());
                    } else {
                        m_maskSelectedPoints.push_back(pt);
                    }
                } else {
                    if (!isSelected(pt)) {
                        m_maskSelectedPoints.clear();
                        m_maskSelectedPoints.push_back(pt);
                    }
                }
                m_maskSelectedPoint = pt;
                m_maskDragIndex = pt; m_maskDragType = 1;
            }
        } else {
            float t; int edge = mask.hitTestEdge(mouseUV, 0.02f, t);
            if (edge >= 0 && mask.count() >= 2 && mask.closed()) {
                mask.insertPoint(edge, t);
                m_maskSelectedPoints.clear();
                m_maskSelectedPoints.push_back(edge + 1);
                m_maskSelectedPoint = edge + 1;
                m_maskDragIndex = m_maskSelectedPoint; m_maskDragType = 1;
            } else {
                // Add new point at end (open path building)
                mask.addPoint(mouseUV);
                m_maskSelectedPoints.clear();
                m_maskSelectedPoints.push_back(mask.count() - 1);
                m_maskSelectedPoint = mask.count() - 1;
                m_maskDragIndex = m_maskSelectedPoint; m_maskDragType = 1;
            }
        }
    }

    if (m_maskDragType > 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto& pts = mask.points();
        if (m_maskDragIndex >= 0 && m_maskDragIndex < (int)pts.size()) {
            glm::vec2 clamped = glm::clamp(mouseUV, glm::vec2(0.0f), glm::vec2(1.0f));
            if (m_maskDragType == 1) {
                // Move all selected points together
                glm::vec2 delta = clamped - pts[m_maskDragIndex].position;
                if (m_maskSelectedPoints.size() > 1) {
                    for (int si : m_maskSelectedPoints) {
                        if (si >= 0 && si < (int)pts.size()) {
                            pts[si].position = glm::clamp(pts[si].position + delta, glm::vec2(0.0f), glm::vec2(1.0f));
                        }
                    }
                } else {
                    pts[m_maskDragIndex].position = clamped;
                }
            } else if (m_maskDragType == 2) {
                auto& pt = pts[m_maskDragIndex];
                pt.handleIn = clamped - pt.position;
                if (pt.smooth) { float l = glm::length(pt.handleOut); if (l<0.001f) l=glm::length(pt.handleIn); pt.handleOut = glm::normalize(-pt.handleIn)*l; }
            } else if (m_maskDragType == 3) {
                auto& pt = pts[m_maskDragIndex];
                pt.handleOut = clamped - pt.position;
                if (pt.smooth) { float l = glm::length(pt.handleIn); if (l<0.001f) l=glm::length(pt.handleOut); pt.handleIn = glm::normalize(-pt.handleOut)*l; }
            }
            mask.markDirty();
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { m_maskDragType = 0; m_maskDragIndex = -1; }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_hovered) {
        int pt = mask.hitTestPoint(mouseUV, 0.03f); if (pt >= 0) { mask.removePoint(pt); m_maskSelectedPoint = -1; }
    }
    if (m_maskSelectedPoint >= 0 && m_maskSelectedPoint < mask.count()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) { mask.removePoint(m_maskSelectedPoint); m_maskSelectedPoint = -1; }

        // Arrow key nudge selected mask point (1 pixel in layer UV space)
        if (!ImGui::GetIO().WantTextInput && m_maskSelectedPoint >= 0 && m_maskSelectedPoint < mask.count()) {
            auto& pts = mask.points();
            // 1 pixel nudge: approximate as 1/canvas_size in UV, use image pixel size
            float nudgeX = 1.0f / std::max(1.0f, m_imageSize.x);
            float nudgeY = 1.0f / std::max(1.0f, m_imageSize.y);
            bool nudged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  { pts[m_maskSelectedPoint].position.x -= nudgeX; nudged = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { pts[m_maskSelectedPoint].position.x += nudgeX; nudged = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    { pts[m_maskSelectedPoint].position.y += nudgeY; nudged = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  { pts[m_maskSelectedPoint].position.y -= nudgeY; nudged = true; }
            if (nudged) {
                pts[m_maskSelectedPoint].position = glm::clamp(pts[m_maskSelectedPoint].position, glm::vec2(0.0f), glm::vec2(1.0f));
                mask.markDirty();
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive()) {
        if (m_maskSelectedPoint >= 0) {
            // First Esc: clear mask-point selection
            m_maskSelectedPoint = -1;
        } else {
            // Second Esc (or no point selected): request exit from mask mode
            m_wantsExitMaskMode = true;
        }
    }

    const auto& pts = mask.points();

    // Mask edit mode visual vocabulary: amber/yellow distinct from cyan transform handles
    // Mask overlay colors: zone-colored for canvas masks, gold for layer masks
    ImU32 kMFill, kMCurveGlow, kMCurve, kMHandleLine, kMHandleDot, kMHandleRing;
    ImU32 kMPointFill, kMPointRing, kMSelFill, kMSelRing, kMSelGlow;
    if (zoneIndex >= 0) {
        auto c = kZoneColors[zoneIndex % 8];
        kMFill       = IM_COL32(c.r, c.g, c.b, 25);
        kMCurveGlow  = IM_COL32(c.r, c.g, c.b, 55);
        kMCurve      = IM_COL32(c.r, c.g, c.b, 230);
        kMHandleLine = IM_COL32(c.r, c.g, c.b, 110);
        kMHandleDot  = IM_COL32(std::min(c.r+40,255), std::min(c.g+40,255), std::min(c.b+40,255), 220);
        kMHandleRing = IM_COL32(std::min(c.r+60,255), std::min(c.g+60,255), std::min(c.b+60,255), 255);
        kMPointFill  = IM_COL32(c.r, c.g, c.b, 255);
        kMPointRing  = IM_COL32(std::min(c.r+80,255), std::min(c.g+80,255), std::min(c.b+80,255), 220);
        kMSelFill    = IM_COL32(std::min(c.r+40,255), std::min(c.g+40,255), std::min(c.b+40,255), 255);
        kMSelRing    = IM_COL32(255, 255, 255, 255);
        kMSelGlow    = IM_COL32(c.r, c.g, c.b, 60);
    } else {
        kMFill       = IM_COL32(255, 200, 60, 25);
        kMCurveGlow  = IM_COL32(255, 200, 60, 55);
        kMCurve      = IM_COL32(255, 210, 90, 230);
        kMHandleLine = IM_COL32(255, 200, 60, 110);
        kMHandleDot  = IM_COL32(255, 220, 120, 220);
        kMHandleRing = IM_COL32(255, 230, 160, 255);
        kMPointFill  = IM_COL32(255, 200, 60, 255);
        kMPointRing  = IM_COL32(255, 240, 200, 220);
        kMSelFill    = IM_COL32(255, 240, 120, 255);
        kMSelRing    = IM_COL32(255, 255, 255, 255);
        kMSelGlow    = IM_COL32(255, 200, 60, 60);
    }

    // Edit mode banner (top-right of viewport) + Save button.
    {
        const char* banner = mask.closed()
            ? "MASK EDIT  |  Shift+Click: multi-select  •  Drag: move  •  R-click: del"
            : "MASK EDIT  |  Click: add points  •  Click first point to close  •  R-click: del";
        const char* saveLbl = "Save";
        ImVec2 ts = ImGui::CalcTextSize(banner);
        ImVec2 sts = ImGui::CalcTextSize(saveLbl);
        float pad = 8.0f;
        draw->AddRectFilled(bannerMin, bannerMax, IM_COL32(40, 30, 10, 200), 3.0f);
        draw->AddRect(bannerMin, bannerMax, IM_COL32(255, 200, 60, 180), 3.0f, 0, 1.5f);
        draw->AddText(ImVec2(bannerMin.x + pad, bannerMin.y + pad * 0.5f),
                      IM_COL32(255, 220, 120, 255), banner);

        bool saveHov = ptInRect(mousePos, saveBtnMin, saveBtnMax);
        ImU32 btnBg = saveHov ? IM_COL32(255, 210, 110, 230) : IM_COL32(255, 200, 60, 180);
        ImU32 btnTx = saveHov ? IM_COL32(40, 25, 0, 255)      : IM_COL32(60, 40, 10, 255);
        draw->AddRectFilled(saveBtnMin, saveBtnMax, btnBg, 3.0f);
        float saveBtnW = saveBtnMax.x - saveBtnMin.x;
        float saveBtnH = saveBtnMax.y - saveBtnMin.y;
        draw->AddText(ImVec2(saveBtnMin.x + (saveBtnW - sts.x) * 0.5f,
                             saveBtnMin.y + (saveBtnH - sts.y) * 0.5f), btnTx, saveLbl);
        if (saveHov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_wantsExitMaskMode = true;
        }
    }

    if (pts.empty()) { draw->PopClipRect(); return; }

    // Closed mask: subtle fill inside to show masked region
    if (pts.size() >= 3 && mask.closed()) {
        auto tv = mask.tessellate(24);
        if (tv.size() >= 3) {
            ImVec2 cs = toImVec2(layerUVToScreen(mask.centroid()));
            for (int i = 0; i < (int)tv.size(); i++) {
                int j = (i+1) % (int)tv.size();
                draw->AddTriangleFilled(cs, toImVec2(layerUVToScreen(tv[i])),
                                        toImVec2(layerUVToScreen(tv[j])), kMFill);
            }
        }
    }

    // Open path with 1+ points: show "close" hint on first point
    if (!mask.closed() && pts.size() >= 3) {
        ImVec2 firstPt = toImVec2(layerUVToScreen(pts[0].position));
        draw->AddCircle(firstPt, 12.0f, IM_COL32(255, 200, 60, 180), 0, 2.0f);
        draw->AddCircleFilled(firstPt, 12.0f, IM_COL32(255, 200, 60, 40));
        // "Close" label
        draw->AddText(ImVec2(firstPt.x + 14, firstPt.y - 8), IM_COL32(255, 220, 120, 255), "Close");
    }
    if (pts.size() >= 2) {
        int n = (int)pts.size(), edges = mask.closed() ? n : (n-1);
        for (int i = 0; i < edges; i++) {
            int j = (i+1)%n;
            ImVec2 p0=toImVec2(layerUVToScreen(pts[i].position)), c0=toImVec2(layerUVToScreen(pts[i].position+pts[i].handleOut));
            ImVec2 c1=toImVec2(layerUVToScreen(pts[j].position+pts[j].handleIn)), p1=toImVec2(layerUVToScreen(pts[j].position));
            draw->AddBezierCubic(p0,c0,c1,p1,kMCurveGlow,5.0f,32);
            draw->AddBezierCubic(p0,c0,c1,p1,kMCurve,1.8f,32);
        }

        // Hover ghost "+" on edge midpoints for discoverable point insertion
        float tParam; int hoverEdge = mask.hitTestEdge(mouseUV, 0.04f, tParam);
        if (hoverEdge >= 0 && m_maskDragType == 0) {
            // Find the midpoint on the edge at t=0.5 for visual hint
            int i = hoverEdge, j = (hoverEdge + 1) % n;
            glm::vec2 mid = MaskPath::evalBezier(
                pts[i].position,
                pts[i].position + pts[i].handleOut,
                pts[j].position + pts[j].handleIn,
                pts[j].position,
                0.5f);
            ImVec2 mp = toImVec2(layerUVToScreen(mid));
            draw->AddCircleFilled(mp, 8.0f, IM_COL32(255, 220, 120, 80));
            draw->AddCircle(mp, 8.0f, IM_COL32(255, 230, 160, 200), 0, 1.5f);
            // Plus icon
            draw->AddLine(ImVec2(mp.x - 4, mp.y), ImVec2(mp.x + 4, mp.y), IM_COL32(255, 250, 200, 255), 1.8f);
            draw->AddLine(ImVec2(mp.x, mp.y - 4), ImVec2(mp.x, mp.y + 4), IM_COL32(255, 250, 200, 255), 1.8f);
        }
    }
    for (int i = 0; i < (int)pts.size(); i++) {
        ImVec2 anch = toImVec2(layerUVToScreen(pts[i].position));
        bool isSel = (i == m_maskSelectedPoint) || isSelected(i);
        if (glm::length(pts[i].handleIn) > 0.001f) {
            ImVec2 h = toImVec2(layerUVToScreen(pts[i].position+pts[i].handleIn));
            draw->AddLine(anch,h,kMHandleLine,1.0f); draw->AddCircleFilled(h,3.5f,kMHandleDot); draw->AddCircle(h,3.5f,kMHandleRing,0,1.2f);
        }
        if (glm::length(pts[i].handleOut) > 0.001f) {
            ImVec2 h = toImVec2(layerUVToScreen(pts[i].position+pts[i].handleOut));
            draw->AddLine(anch,h,kMHandleLine,1.0f); draw->AddCircleFilled(h,3.5f,kMHandleDot); draw->AddCircle(h,3.5f,kMHandleRing,0,1.2f);
        }
        if (isSel) {
            float s=5.5f;
            draw->AddQuadFilled(ImVec2(anch.x,anch.y-s-3),ImVec2(anch.x+s+3,anch.y),ImVec2(anch.x,anch.y+s+3),ImVec2(anch.x-s-3,anch.y),kMSelGlow);
            draw->AddQuadFilled(ImVec2(anch.x,anch.y-s),ImVec2(anch.x+s,anch.y),ImVec2(anch.x,anch.y+s),ImVec2(anch.x-s,anch.y),kMSelFill);
            draw->AddQuad(ImVec2(anch.x,anch.y-s),ImVec2(anch.x+s,anch.y),ImVec2(anch.x,anch.y+s),ImVec2(anch.x-s,anch.y),kMSelRing,1.5f);
        } else {
            draw->AddCircleFilled(anch,4.5f,kMPointFill); draw->AddCircle(anch,4.5f,kMPointRing,0,1.2f);
        }
    }

    draw->PopClipRect();
}

// --- Shared secondary nav bar — CANVAS/STAGE pills + zones + OUTPUT + comp
//     chip + Fullscreen. Called from both Canvas (stageActive=false) and
//     Stage (stageActive=true) so switching workspaces never shifts the
//     element geometry.
void ViewportPanel::renderNavBar(bool stageActive,
                                 std::vector<std::unique_ptr<OutputZone>>* zones,
                                 int* activeZone,
                                 const std::vector<MonitorInfo>* monitors,
                                 bool ndiAvailable,
                                 int editorMonitor) {
    if (!zones || !activeZone) return;
    ImGui::Dummy(ImVec2(0, 2));
    // Match timeline/header edge padding (~12px from window edge).
    ImGui::Indent(12);
    ImDrawList* tabDraw = ImGui::GetWindowDrawList();

    // CANVAS/STAGE pills
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 7));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
        auto pill = [&](const char* label, bool active) {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.96f, 0.97f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.98f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.05f, 0.07f, 0.10f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.96f, 0.97f, 1.00f, 1.00f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.80f, 0.82f, 0.88f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.45f, 0.48f, 0.54f, 0.85f));
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            return clicked;
        };
        if (pill("CANVAS", !stageActive)) UIManager::sShowStage = false;
        ImGui::SameLine(0, 6);
        if (pill("STAGE", stageActive))  UIManager::sShowStage = true;
        ImGui::PopStyleVar(3);
    }

    // Right-align the cluster
    {
        const float kPadX        = 14.0f * 2.0f;
        const float kZoneSpacing = 4.0f;
        const float kSectionGap  = 14.0f;
        const float kInnerGap    = 8.0f;
        const float kComboW      = 150.0f;
        const float kDotW        = 12.0f;
        float rightW = 0.0f;
        for (auto& zp : *zones)
            rightW += ImGui::CalcTextSize(zp->name.c_str()).x + kPadX + kZoneSpacing;
        rightW += ImGui::CalcTextSize("+").x + kPadX;
        rightW += kSectionGap + kDotW + kInnerGap;
        rightW += ImGui::CalcTextSize("OUTPUT").x + kInnerGap;
        rightW += kComboW + kInnerGap;
        int aiLookup = *activeZone;
        if (aiLookup >= 0 && aiLookup < (int)zones->size()) {
            char compLabel[48];
            snprintf(compLabel, sizeof(compLabel), "%d x %d",
                     (*zones)[aiLookup]->width, (*zones)[aiLookup]->height);
            rightW += ImGui::CalcTextSize(compLabel).x + kPadX;
        } else {
            rightW += 120.0f;
        }
        rightW += kInnerGap;
        rightW += ImGui::CalcTextSize("Fullscreen").x + kPadX;
        // ~12px right margin to mirror the left inset, plus combo-arrow slack.
        rightW += 28.0f;

        ImGui::SameLine();
        float targetX = ImGui::GetContentRegionMax().x - rightW;
        float curX    = ImGui::GetCursorPosX();
        if (targetX > curX) ImGui::SetCursorPosX(targetX);
    }

    // Zone tabs
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
    for (int i = 0; i < (int)zones->size(); i++) {
        ImGui::PushID(9000 + i);
        bool isActive = (i == *activeZone);
        auto& z = *(*zones)[i];
        static const float zoneColors[][3] = {
            {0.96f, 0.96f, 0.96f}, {0.86f, 0.86f, 0.86f}, {0.76f, 0.76f, 0.76f}, {0.66f, 0.66f, 0.66f},
            {0.56f, 0.56f, 0.56f}, {0.80f, 0.80f, 0.80f}, {0.70f, 0.70f, 0.70f}, {0.60f, 0.60f, 0.60f},
        };
        const float* zc = zoneColors[i % 8];
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0], zc[1], zc[2], 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0], zc[1], zc[2], 0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0], zc[1], zc[2], 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(zc[0] * 0.15f, zc[1] * 0.15f, zc[2] * 0.15f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(zc[0] * 0.25f, zc[1] * 0.25f, zc[2] * 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(zc[0] * 0.6f, zc[1] * 0.6f, zc[2] * 0.6f, 1.0f));
        }
        if (ImGui::Button(z.name.c_str())) *activeZone = i;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_renaming = true;
            m_renameIndex = i;
            strncpy(m_renameBuf, z.name.c_str(), sizeof(m_renameBuf) - 1);
            m_renameBuf[sizeof(m_renameBuf) - 1] = '\0';
        }
        if (ImGui::BeginPopupContextItem("ZoneTabCtx")) {
            if (ImGui::MenuItem("Rename")) {
                m_renaming = true;
                m_renameIndex = i;
                strncpy(m_renameBuf, z.name.c_str(), sizeof(m_renameBuf) - 1);
                m_renameBuf[sizeof(m_renameBuf) - 1] = '\0';
            }
            if (ImGui::MenuItem("Duplicate")) *activeZone = -(200 + i);
            if ((int)zones->size() > 1) {
                if (ImGui::MenuItem("Remove")) *activeZone = -(300 + i);
            }
            ImGui::EndPopup();
        }
        ImVec2 btnMin = ImGui::GetItemRectMin();
        if (z.outputDest == OutputDest::Fullscreen || z.outputDest == OutputDest::NDI) {
            ImU32 dotCol = IM_COL32((int)(zc[0]*255), (int)(zc[1]*255), (int)(zc[2]*255), 255);
            tabDraw->AddCircleFilled(ImVec2(btnMin.x + 5, btnMin.y + 5), 3.0f, dotCol);
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    if (m_renaming) ImGui::OpenPopup("##RenameZone");
    if (ImGui::BeginPopup("##RenameZone")) {
        ImGui::Text("Rename Zone");
        ImGui::SetNextItemWidth(200);
        bool enter = ImGui::InputText("##RenameInput", m_renameBuf, sizeof(m_renameBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (m_renaming) { ImGui::SetKeyboardFocusHere(-1); m_renaming = false; }
        if (enter || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (enter && m_renameIndex >= 0 && m_renameIndex < (int)zones->size() && m_renameBuf[0]) {
                (*zones)[m_renameIndex]->name = m_renameBuf;
            }
            m_renameIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
    if (ImGui::Button("+")) *activeZone = -(100 + (int)zones->size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add output zone");
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    // OUTPUT + composition + Fullscreen
    int ai = *activeZone;
    if (ai >= 0 && ai < (int)zones->size()) {
        auto& az = *(*zones)[ai];
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));
        ImGui::SameLine(0, 14);

        static char destBuf[128] = {};
        const char* destLabel = "Preview Only";
        if (az.outputDest == OutputDest::Fullscreen && monitors) {
            int mi = az.outputMonitor;
            if (mi >= 0 && mi < (int)monitors->size()) {
                snprintf(destBuf, sizeof(destBuf), "Fullscreen: %s", (*monitors)[mi].name.c_str());
                destLabel = destBuf;
            }
        } else if (az.outputDest == OutputDest::NDI) {
            snprintf(destBuf, sizeof(destBuf), "NDI: \"%s\"",
                     az.ndiStreamName.empty() ? az.name.c_str() : az.ndiStreamName.c_str());
            destLabel = destBuf;
        }

        bool live = (az.outputDest != OutputDest::None);
        ImVec2 dotPos = ImGui::GetCursorScreenPos();
        float frameH = ImGui::GetFrameHeight();
        float dotCY = dotPos.y + frameH * 0.5f;
        if (live) {
            tabDraw->AddCircleFilled(ImVec2(dotPos.x + 4, dotCY), 3.5f, IM_COL32(34, 210, 130, 255));
            tabDraw->AddCircle(ImVec2(dotPos.x + 4, dotCY), 5.5f, IM_COL32(34, 210, 130, 40));
        } else {
            tabDraw->AddCircle(ImVec2(dotPos.x + 4, dotCY), 3.0f, IM_COL32(100, 110, 130, 140), 0, 1.2f);
        }
        ImGui::Dummy(ImVec2(12, frameH));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (live) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
        else      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::TextUnformatted("OUTPUT");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        if (live) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
        else      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.73f, 0.78f, 1.0f));
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##ZoneOutput", destLabel, ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("Preview Only", az.outputDest == OutputDest::None)) {
                az.outputDest = OutputDest::None;
                az.outputMonitor = -1;
            }
            if (monitors && !monitors->empty()) {
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                ImGui::Text("  Fullscreen");
                ImGui::PopStyleColor();
                for (int mi = 0; mi < (int)monitors->size(); mi++) {
                    ImGui::PushID(mi);
                    if (mi == editorMonitor) { ImGui::PopID(); continue; }
                    std::string claimedBy;
                    for (int zi = 0; zi < (int)zones->size(); zi++) {
                        if (zi == ai) continue;
                        auto& oz = *(*zones)[zi];
                        if (oz.outputDest == OutputDest::Fullscreen && oz.outputMonitor == mi) {
                            claimedBy = oz.name; break;
                        }
                    }
                    char label[256];
                    if (!claimedBy.empty()) {
                        snprintf(label, sizeof(label), "%s  %dx%d  (-> %s)",
                                 (*monitors)[mi].name.c_str(),
                                 (*monitors)[mi].width, (*monitors)[mi].height,
                                 claimedBy.c_str());
                    } else {
                        snprintf(label, sizeof(label), "%s  %dx%d",
                                 (*monitors)[mi].name.c_str(),
                                 (*monitors)[mi].width, (*monitors)[mi].height);
                    }
                    if (!claimedBy.empty())
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.6f));
                    bool sel = (az.outputDest == OutputDest::Fullscreen && az.outputMonitor == mi);
                    if (ImGui::Selectable(label, sel)) {
                        if (!claimedBy.empty()) {
                            for (int zi = 0; zi < (int)zones->size(); zi++) {
                                if (zi == ai) continue;
                                auto& oz = *(*zones)[zi];
                                if (oz.outputDest == OutputDest::Fullscreen && oz.outputMonitor == mi) {
                                    oz.outputDest = OutputDest::None;
                                    oz.outputMonitor = -1;
                                    break;
                                }
                            }
                        }
                        az.outputDest = OutputDest::Fullscreen;
                        az.outputMonitor = mi;
                    }
                    if (!claimedBy.empty()) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            if (ndiAvailable) {
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
                ImGui::Text("  NDI");
                ImGui::PopStyleColor();
                char ndiLabel[256];
                std::string streamName = az.ndiStreamName.empty() ? az.name : az.ndiStreamName;
                snprintf(ndiLabel, sizeof(ndiLabel), "Easel - %s", streamName.c_str());
                bool sel = (az.outputDest == OutputDest::NDI);
                if (ImGui::Selectable(ndiLabel, sel)) {
                    az.outputDest = OutputDest::NDI;
                    if (az.ndiStreamName.empty()) az.ndiStreamName = az.name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();

        // Composition chip + Fullscreen
        {
            static const char* presetLabels[] = {
                "1920x1080 (1080p)", "3840x2160 (4K)", "1280x720 (720p)",
                "2560x1440 (1440p)", "8000x2000 (Ultra-wide)", "1024x768", "Custom"
            };
            static const int presetW[] = { 1920, 3840, 1280, 2560, 8000, 1024, 0 };
            static const int presetH[] = { 1080, 2160, 720, 1440, 2000, 768, 0 };
            const int presetCount = 7;

            ImGui::SameLine(0, 12);
            char compLabel[48];
            snprintf(compLabel, sizeof(compLabel), "%d x %d", az.width, az.height);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.65f, 0.68f, 0.74f, 1.0f));
            if (ImGui::Button(compLabel)) ImGui::OpenPopup("##CompPreset");
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.75f, 0.78f, 0.84f, 1.0f));
            const char* fsLbl = m_editorFullscreenHint ? "Exit FS" : "Fullscreen";
            if (ImGui::Button(fsLbl)) m_wantsFullscreenToggle = true;
            ImGui::PopStyleColor(3);

            if (ImGui::BeginPopup("##CompPreset")) {
                for (int p = 0; p < presetCount - 1; p++) {
                    bool sel = (az.width == presetW[p] && az.height == presetH[p]);
                    if (ImGui::Selectable(presetLabels[p], sel)) {
                        az.resize(presetW[p], presetH[p]);
                        az.compPreset = p;
                    }
                }
                ImGui::Separator();
                static int s_customCompW = 1920, s_customCompH = 1080;
                static int s_customCompZone = -1;
                if (s_customCompZone != ai) {
                    s_customCompW = az.width > 0 ? az.width : 1920;
                    s_customCompH = az.height > 0 ? az.height : 1080;
                    s_customCompZone = ai;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
                ImGui::TextUnformatted("Custom");
                ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("##compCW", &s_customCompW, 0);
                ImGui::SameLine();
                ImGui::TextDisabled("x");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("##compCH", &s_customCompH, 0);
                ImGui::SameLine();
                bool valid = s_customCompW >= 64 && s_customCompH >= 64
                          && s_customCompW <= 16384 && s_customCompH <= 16384;
                if (!valid) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Apply")) {
                    az.resize(s_customCompW, s_customCompH);
                    az.compPreset = presetCount - 1;
                    ImGui::CloseCurrentPopup();
                }
                if (!valid) ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        ImGui::PopStyleVar(2);
    }

    ImGui::Unindent(12);
}
