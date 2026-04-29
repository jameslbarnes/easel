#include "ui/LayerPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <set>

// Theme
static const ImU32 kAccent       = IM_COL32(255, 255, 255, 255);
static const ImU32 kTextPrimary  = IM_COL32(230, 235, 245, 255);
static const ImU32 kTextDim      = IM_COL32(140, 153, 173, 255);
static const ImU32 kTextMuted    = IM_COL32(90, 100, 120, 255);
static const ImU32 kRowBg        = IM_COL32(22, 26, 36, 255);
static const ImU32 kRowBgAlt     = IM_COL32(25, 30, 42, 255);
static const ImU32 kRowHover     = IM_COL32(32, 40, 58, 255);
static const ImU32 kRowSelected  = IM_COL32(255, 255, 255, 22);
static const ImU32 kEyeOn        = IM_COL32(255, 255, 255, 220);
static const ImU32 kEyeOff       = IM_COL32(70, 75, 90, 140);
static const ImU32 kInsertLine   = IM_COL32(255, 255, 255, 200);
static const ImU32 kInsertGlow   = IM_COL32(255, 255, 255, 50);
static const ImU32 kThumbBorder  = IM_COL32(50, 58, 80, 200);
static const ImU32 kThumbBg      = IM_COL32(15, 18, 25, 255);
static const ImU32 kOpacityBg    = IM_COL32(30, 35, 50, 200);
static const ImU32 kOpacityFill  = IM_COL32(0, 160, 210, 120);
static const ImU32 kBadgeImage   = IM_COL32(80, 180, 100, 200);
static const ImU32 kBadgeVideo   = IM_COL32(200, 120, 60, 200);
static const ImU32 kBadgeShader  = IM_COL32(160, 80, 220, 200);
static const ImU32 kBadgeCapture = IM_COL32(220, 180, 50, 200);
static const ImU32 kBadgeText    = IM_COL32(255, 255, 255, 220);
static const ImU32 kDragShadow   = IM_COL32(0, 0, 0, 120);

static ImU32 getBadgeColor(const std::string& type) {
    if (type == "Image") return kBadgeImage;
    if (type == "Video") return kBadgeVideo;
    if (type == "Shader") return kBadgeShader;
    if (type == "Capture" || type == "WindowCapture") return kBadgeCapture;
    return kTextMuted;
}

static const char* getBadgeLabel(const std::string& type) {
    if (type == "Image") return "IMG";
    if (type == "Video") return "VID";
    if (type == "Shader") return "SHD";
    if (type == "Capture") return "SCR";
    if (type == "WindowCapture") return "WIN";
    return "?";
}

// ─── iPad-style layer cards ────────────────────────────────────────────
//
//   Design brief: "feel like an iPad app" — Procreate / LumaFusion /
//   Pixelmator territory. The ingredients are well-known:
//
//   - Each row is its OWN CARD with a subtle rounded bg, margin between
//     cards, and a soft selected-state that uses an accent-tinted fill
//     (not a hairline border).
//   - Generous padding everywhere. Touch targets ≥ 32pt.
//   - Rounded thumbnail (12pt radius) — instantly "iOS native".
//   - Typography: primary name ~15pt, secondary ALL-CAPS type label
//     ~10pt with letter-spacing (faked via tracked padding) for a
//     quiet subtitle rhythm.
//   - Controls are CIRCULAR icon buttons (~28pt) with a soft fill
//     backdrop, appearing only on row hover or when active. When
//     active the buttons pick up a coloured fill. No pills, no text
//     glyphs.
//   - Opacity shown as a soft fill strip just below the card's bottom,
//     never a sharp blue bar.
//   - No Unicode rendered anywhere — all icons drawn with primitives.
//
namespace LP {
    static constexpr float kCardMarginX = 8.0f;   // outer margin — gives cards space
    static constexpr float kCardMarginY = 4.0f;
    static constexpr float kCardRadius  = 12.0f;  // iOS-native rounding
    static constexpr float kCardPadX    = 10.0f;  // inner horizontal padding — matches the "+ Add Layer" button's visual inset above
    static constexpr float kThumbW      = 48.0f;  // 48×48 thumbnail — iOS-sized
    static constexpr float kThumbToText = 14.0f;
    static constexpr float kBtnR        = 14.0f;  // circular button radius
    static constexpr float kBtnGap      = 6.0f;
    static constexpr float kTextToPct   = 12.0f;
    static constexpr float kPctToBtns   = 10.0f;
}

// Type colour — used for the tiny type-dot next to the name and the
// thumbnail's faint tint.
static ImU32 typeColor(const std::shared_ptr<Layer>& layer) {
    if (!layer->source) return IM_COL32(120, 128, 145, 200);
    return getBadgeColor(layer->source->typeName());
}

// ─── Primitive icon drawers ─────────────────────────────────────────
// Each icon fits in an LP::kIconW × LP::kIconW box centred on (cx, cy).
// `alpha` lets callers fade the whole icon without rebuilding colours.

static void drawEyeIcon(ImDrawList* dl, float cx, float cy, bool open,
                         ImU32 col) {
    if (open) {
        // Almond outline + pupil.
        const int segs = 14;
        const float rX = 6.0f, rY = 3.2f;
        ImVec2 pts[segs * 2];
        for (int i = 0; i < segs; i++) {
            float t = (float)i / (segs - 1);
            float a = -1.5707963f + t * 3.1415927f; // top arc
            pts[i] = ImVec2(cx + std::cos(a) * rX, cy - std::abs(std::sin(a)) * rY);
        }
        for (int i = 0; i < segs; i++) {
            float t = (float)i / (segs - 1);
            float a = -1.5707963f + t * 3.1415927f;
            pts[segs + i] = ImVec2(cx - std::cos(a) * rX, cy + std::abs(std::sin(a)) * rY);
        }
        dl->AddPolyline(pts, segs * 2, col, ImDrawFlags_Closed, 1.2f);
        dl->AddCircleFilled(ImVec2(cx, cy), 1.6f, col);
    } else {
        // Eye closed — single horizontal arc (cupped downward).
        const int segs = 10;
        ImVec2 pts[segs];
        for (int i = 0; i < segs; i++) {
            float t = (float)i / (segs - 1);
            float x = cx + (t - 0.5f) * 12.0f;
            float y = cy + std::sin(t * 3.1415927f) * 2.2f;
            pts[i] = ImVec2(x, y);
        }
        dl->AddPolyline(pts, segs, col, ImDrawFlags_None, 1.3f);
    }
}

static void drawMuteIcon(ImDrawList* dl, float cx, float cy, bool on, ImU32 col) {
    // Speaker: trapezoid + triangular cone. When "on" (muted), draw an X
    // through it to indicate the layer is silenced from the composite.
    float x0 = cx - 6.0f, y0 = cy;
    dl->AddRectFilled(ImVec2(x0, y0 - 2.0f),
                       ImVec2(x0 + 3.0f, y0 + 2.0f), col, 0.5f);
    dl->AddTriangleFilled(ImVec2(x0 + 3.0f, y0 - 4.0f),
                           ImVec2(x0 + 8.0f, y0 - 4.5f),
                           ImVec2(x0 + 8.0f, y0 + 4.5f), col);
    dl->AddTriangleFilled(ImVec2(x0 + 3.0f, y0 + 4.0f),
                           ImVec2(x0 + 8.0f, y0 + 4.5f),
                           ImVec2(x0 + 8.0f, y0 - 4.5f), col);
    if (on) {
        dl->AddLine(ImVec2(cx + 1.0f, cy - 5.5f),
                     ImVec2(cx + 7.0f, cy + 5.5f), col, 1.5f);
        dl->AddLine(ImVec2(cx + 7.0f, cy - 5.5f),
                     ImVec2(cx + 1.0f, cy + 5.5f), col, 1.5f);
    }
}

static void drawSoloIcon(ImDrawList* dl, float cx, float cy, bool on, ImU32 col) {
    // Headphone silhouette: top arc + two earcups. Simple 2D symbol that
    // reads as "solo / monitor" without any text.
    const int segs = 10;
    ImVec2 arcPts[segs];
    for (int i = 0; i < segs; i++) {
        float t = (float)i / (segs - 1);
        float a = 3.1415927f + t * 3.1415927f;
        arcPts[i] = ImVec2(cx + std::cos(a) * 6.0f, cy - 2.0f + std::sin(a) * 5.0f);
    }
    dl->AddPolyline(arcPts, segs, col, ImDrawFlags_None, 1.3f);
    if (on) {
        dl->AddRectFilled(ImVec2(cx - 6.5f, cy - 2.0f),
                           ImVec2(cx - 3.5f, cy + 4.0f), col, 1.5f);
        dl->AddRectFilled(ImVec2(cx + 3.5f, cy - 2.0f),
                           ImVec2(cx + 6.5f, cy + 4.0f), col, 1.5f);
    } else {
        dl->AddRect(ImVec2(cx - 6.5f, cy - 2.0f),
                     ImVec2(cx - 3.5f, cy + 4.0f), col, 1.5f, 0, 1.0f);
        dl->AddRect(ImVec2(cx + 3.5f, cy - 2.0f),
                     ImVec2(cx + 6.5f, cy + 4.0f), col, 1.5f, 0, 1.0f);
    }
}

// Draw a single layer row. The row IS the container — rounded corners,
// subtle surface fill, no nested card inside. Hover/select lift the fill.
// The row's bottom edge has no tray line. Opacity % sits on the right and
// is draggable (the mouse handler in render() detects the drag zone).
//
// Zone dots (when multiple zones exist) live in the right cluster above
// the opacity %, right-aligned to the same inner edge so the pair reads
// as one object.
static void drawLayerRow(ImDrawList* draw, const std::shared_ptr<Layer>& layer,
                         float x, float y, float width, float rowHeight,
                         bool selected, bool hovered, bool dimmed,
                         float /*thumbSize*/,
                         // Right-side cluster width reserved for zone dots —
                         // the caller knows how many zones exist and passes
                         // the computed width so the name column can avoid
                         // overlapping them.
                         float zoneClusterW = 0.0f) {
    // Inset the row slightly so rounded corners stay clear of the panel
    // edges without creating a visible "card inside panel" look.
    // Row container spans the full panel width so its left/right edges
    // align with the "+ Add Layer" button above. Only vertical margin
    // remains — just enough spacing between stacked rows.
    const float kInsetX = 0.0f;
    const float kInsetY = 3.0f;
    ImVec2 rowMin(x + kInsetX, y + kInsetY);
    ImVec2 rowMax(x + width - kInsetX, y + rowHeight - kInsetY);

    const float cy  = (rowMin.y + rowMax.y) * 0.5f;
    const float cx0 = rowMin.x;
    const float cx1 = rowMax.x;
    const float kRowR = 12.0f;
    bool effectivelyVisible = layer->visible && !layer->userHidden;
    ImU32 tCol = typeColor(layer);

    // ── Always-on container fill (rounded) — the row is a surface, not a
    //    blank strip. Hover/select layer ON TOP of the base fill so it
    //    still feels like one object, not two stacked shapes.
    if (!dimmed) {
        draw->AddRectFilled(rowMin, rowMax, IM_COL32(30, 35, 46, 255), kRowR);
        if (selected) {
            draw->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 16), kRowR);
        } else if (hovered) {
            draw->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 8), kRowR);
        }
    }

    // ── Thumbnail
    float thumbX = cx0 + LP::kCardPadX;
    float thumbH = LP::kThumbW;
    float thumbY = cy - thumbH * 0.5f;
    ImVec2 tMin(thumbX, thumbY), tMax(thumbX + thumbH, thumbY + thumbH);

    // Thumbnail rounding — subtle curve, roughly 6pt so the corners soften
    // without reading as a pill. Image clipped via AddImageRounded so the
    // content curves with the frame.
    const float kThumbR = 6.0f;
    ImU32 thumbTint = IM_COL32(
        (tCol >>  0) & 0xFF,
        (tCol >>  8) & 0xFF,
        (tCol >> 16) & 0xFF,
        40);
    draw->AddRectFilled(tMin, tMax, kThumbBg,  kThumbR);
    draw->AddRectFilled(tMin, tMax, thumbTint, kThumbR);

    GLuint texId = layer->source ? layer->source->textureId() : 0;
    if (texId != 0) {
        draw->AddImageRounded((ImTextureID)(intptr_t)texId, tMin, tMax,
                               ImVec2(0, 1), ImVec2(1, 0),
                               IM_COL32(255, 255, 255, effectivelyVisible ? 255 : 90),
                               kThumbR);
    }
    draw->AddRect(tMin, tMax, IM_COL32(255, 255, 255, 16), kThumbR, 0, 1.0f);

    if (!effectivelyVisible) {
        draw->AddLine(ImVec2(tMin.x + 6, tMax.y - 6),
                       ImVec2(tMax.x - 6, tMin.y + 6),
                       IM_COL32(255, 255, 255, 170), 2.0f);
    }

    // ── Name + TYPE caption
    float nameX = tMax.x + LP::kThumbToText;

    const float nameSize = 15.0f;
    const float typeSize = 10.0f;   // SHADER caption size — the % matches this
    const float gapY     = 4.0f;
    const float pairH    = nameSize + gapY + typeSize;
    const float nameY    = cy - pairH * 0.5f;
    const float typeY    = nameY + nameSize + gapY;

    char opBuf[8];
    snprintf(opBuf, sizeof(opBuf), "%d%%",
             (int)(layer->opacity * 100.0f + 0.5f));
    // Compute % text metrics at the SMALLER SHADER-caption size so both
    // read as a matched pair rhythmically.
    ImVec2 opSz = ImGui::GetFont()->CalcTextSizeA(typeSize, FLT_MAX, 0.0f, opBuf);
    float rightInnerEdge = cx1 - LP::kCardPadX;
    float rightClusterW  = std::max(opSz.x, zoneClusterW);
    float pctX           = rightInnerEdge - opSz.x;
    float nameWMax       = std::max(40.0f,
                                     rightInnerEdge - rightClusterW
                                     - LP::kTextToPct - nameX);

    ImU32 nameCol = effectivelyVisible ? kTextPrimary : kTextDim;
    if (dimmed) nameCol = IM_COL32(180, 190, 210, 150);

    draw->PushClipRect(ImVec2(nameX, rowMin.y),
                        ImVec2(nameX + nameWMax, rowMax.y), true);
    draw->AddText(ImGui::GetFont(), nameSize,
                   ImVec2(nameX, nameY), nameCol, layer->name.c_str());
    if (layer->source) {
        std::string type = layer->source->typeName();
        for (auto& ch : type) ch = (char)toupper((unsigned char)ch);
        if (layer->soloed)     type += "  SOLO";
        else if (layer->muted) type += "  MUTED";
        draw->AddText(ImGui::GetFont(), typeSize,
                       ImVec2(nameX, typeY), kTextMuted, type.c_str());
    }
    draw->PopClipRect();

    // ── Opacity % — small (matches SHADER caption size), right-aligned,
    //    sits on the TYPE baseline so name/% share a visual line below
    //    and name/(zone dots) share one above. Draggable via the handler
    //    in render().
    draw->AddText(ImGui::GetFont(), typeSize,
                   ImVec2(pctX, typeY), kTextDim, opBuf);
}

// Zone / layer-row accent colors — vibrant so layers assigned to different
// zones are easy to tell apart at a glance. Avoids cyan (project-wide ban)
// but uses a distinct rainbow set for clarity.
static ImU32 zoneColor(int idx) {
    // Currently monochrome — single near-white tint for every zone.
    // Re-introduce distinct hues here later when an accent palette is
    // chosen.
    (void)idx;
    return IM_COL32(235, 238, 244, 230);
}

void LayerPanel::render(LayerStack& stack, int& selectedLayer,
                        std::vector<std::unique_ptr<OutputZone>>* zones,
                        int activeZone) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(250, 150), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Layers");

    wantsAddImage = wantsAddVideo = wantsAddShader = false;
    removedLayerIds.clear();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float panelWidth = ImGui::GetContentRegionAvail().x;
    if (panelWidth < 10.0f) { ImGui::End(); return; }

    // "+" add layer button (full width)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
        if (ImGui::Button("+ Add Layer", ImVec2(-1, 0))) {
            ImGui::OpenPopup("##AddLayer");
        }
        ImGui::PopStyleColor(4);

        if (ImGui::BeginPopup("##AddLayer")) {
            if (ImGui::MenuItem("Image...")) wantsAddImage = true;
            if (ImGui::MenuItem("Video...")) wantsAddVideo = true;
            if (ImGui::MenuItem("Shader...")) wantsAddShader = true;
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(0, 2));
    }

    // Row rhythm — 72px cards (64 content + 8 outer margin) so each card
    // reads as an iOS-sized list cell with room to breathe.
    float thumbSize = LP::kThumbW;
    float rowHeight = 72.0f;
    int layerCount = stack.count();

    // Empty state
    if (layerCount == 0) {
        ImGui::Dummy(ImVec2(0, 30));
        float tw = ImGui::CalcTextSize("No layers").x;
        ImGui::SetCursorPosX((panelWidth - tw) * 0.5f);
        ImGui::TextDisabled("No layers");
        ImGui::Dummy(ImVec2(0, 6));
        float hw = ImGui::CalcTextSize("Drop files or click +").x;
        ImGui::SetCursorPosX((panelWidth - hw) * 0.5f);
        ImGui::TextDisabled("Drop files or click +");
        ImGui::End();
        return;
    }

    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 listStart = ImGui::GetCursorScreenPos();
    bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Reserve space for all rows so scrolling works
    ImGui::Dummy(ImVec2(panelWidth, rowHeight * layerCount));

    // --- MOUSE INTERACTION ---

    // Detect mouse-down on a row to start potential drag.
    // NOTE: the opacity-scrub drag is NOT handled here — it's driven by the
    // ImGui::InvisibleButton over each row's % text region, which claims
    // the mouse press itself so the window-drag gesture never triggers.
    // We only run this block if no opacity drag is in progress.
    if (m_opacityDragIdx < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && windowHovered && !m_renaming)
    {
        float relY = mousePos.y - listStart.y;
        int displayIdx = (int)(relY / rowHeight);
        if (displayIdx >= 0 && displayIdx < layerCount && relY >= 0) {
            int stackIdx = layerCount - 1 - displayIdx;

            // Hit zones: thumbnail toggles visibility, everything else =
            // select + potential reorder. Opacity scrub is handled by the
            // InvisibleButton below, not here.
            float relX = mousePos.x - listStart.x;
            float thumbLo = LP::kCardPadX - 4;
            float thumbHi = LP::kCardPadX + LP::kThumbW + 4;
            if (relX >= thumbLo && relX < thumbHi) {
                auto& L = stack[stackIdx];
                L->userHidden = !L->userHidden;
                L->visible = !L->userHidden;
            } else {
                selectedLayer = stackIdx;
                m_dragIndex = stackIdx;
                m_dragStartY = mousePos.y;
                m_dragOffsetY = mousePos.y - (listStart.y + displayIdx * rowHeight);
                m_dragActive = false; // not yet — need to move first
            }
        } else {
            // Clicked outside any layer row — deselect
            selectedLayer = -1;
        }
    }

    // Activate drag once mouse moves enough (3px threshold)
    if (m_dragIndex >= 0 && !m_dragActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (fabsf(mousePos.y - m_dragStartY) > 3.0f) {
            m_dragActive = true;
        }
    }

    // (Opacity scrub is driven by IsItemActivated / IsItemActive on the
    // per-row InvisibleButton below, not here. ImGui claims the mouse
    // press so the window never starts a drag.)

    // Track insertion position during drag
    if (m_dragActive) {
        float relY = mousePos.y - listStart.y;
        int hoverDisplay = (int)(relY / rowHeight);
        // Snap to nearest edge
        float frac = (relY / rowHeight) - hoverDisplay;
        if (frac > 0.5f) hoverDisplay++;
        m_insertIndex = std::max(0, std::min(hoverDisplay, layerCount));
    }

    // Drop on release
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_dragActive && m_dragIndex >= 0) {
            int dragDisplay = layerCount - 1 - m_dragIndex;
            int insertDisplay = m_insertIndex;

            // Convert display insert position to stack index
            // display 0 = stack top (layerCount-1), display N = stack bottom (0)
            // Insert at display position D means the layer goes to stack index (layerCount - D)
            // but we need to account for the removal of the dragged layer
            if (insertDisplay != dragDisplay && insertDisplay != dragDisplay + 1) {
                int toStack;
                if (insertDisplay < dragDisplay) {
                    // Moving up in display = moving to higher stack index
                    toStack = layerCount - 1 - insertDisplay;
                } else {
                    // Moving down in display = moving to lower stack index
                    toStack = layerCount - insertDisplay;
                }
                toStack = std::max(0, std::min(toStack, layerCount - 1));
                if (toStack != m_dragIndex) {
                    stack.moveLayer(m_dragIndex, toStack);
                    selectedLayer = toStack;
                }
            }
        }
        m_dragActive = false;
        m_dragIndex = -1;
        m_insertIndex = -1;
    }

    // Double-click to rename
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && windowHovered && !m_dragActive) {
        float relY = mousePos.y - listStart.y;
        int displayIdx = (int)(relY / rowHeight);
        if (displayIdx >= 0 && displayIdx < layerCount && mousePos.x >= listStart.x + 26) {
            int stackIdx = layerCount - 1 - displayIdx;
            m_renaming = true;
            m_renameJustStarted = true;
            m_renameIndex = stackIdx;
            strncpy(m_renameBuf, stack[stackIdx]->name.c_str(), sizeof(m_renameBuf) - 1);
            m_renameBuf[sizeof(m_renameBuf) - 1] = '\0';
        }
    }

    // Right-click context menu
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && windowHovered) {
        float relY = mousePos.y - listStart.y;
        int displayIdx = (int)(relY / rowHeight);
        if (displayIdx >= 0 && displayIdx < layerCount) {
            int stackIdx = layerCount - 1 - displayIdx;
            selectedLayer = stackIdx;
            ImGui::OpenPopup("LayerMenu");
        }
    }

    // --- DRAW ROWS ---

    int dragDisplay = (m_dragIndex >= 0) ? (layerCount - 1 - m_dragIndex) : -1;

    for (int displayIdx = 0; displayIdx < layerCount; displayIdx++) {
        int stackIdx = layerCount - 1 - displayIdx;
        auto& layer = stack[stackIdx];
        bool selected = (stackIdx == selectedLayer);

        float rowY = listStart.y + displayIdx * rowHeight;

        // Skip drawing the dragged row in-place (we draw it floating)
        if (m_dragActive && displayIdx == dragDisplay) {
            // Draw a dim placeholder
            ImVec2 rMin(listStart.x, rowY);
            ImVec2 rMax(listStart.x + panelWidth, rowY + rowHeight);
            draw->AddRectFilled(rMin, rMax, IM_COL32(20, 24, 34, 180), 3.0f);
            continue;
        }

        ImVec2 rMin(listStart.x, rowY);
        ImVec2 rMax(listStart.x + panelWidth, rowY + rowHeight);

        // Hover (only when not dragging)
        bool rowHovered = !m_dragActive &&
                          mousePos.x >= rMin.x && mousePos.x < rMax.x &&
                          mousePos.y >= rMin.y && mousePos.y < rMax.y && windowHovered;

        // Zone-dot geometry: dots stack above the opacity %, right-aligned
        // to the card's inner right edge. Compute the cluster width up
        // front so drawLayerRow can reserve horizontal room.
        int zoneCount = (zones && zones->size() > 1) ? (int)zones->size() : 0;
        const float kZoneDotR   = 3.5f;
        const float kZoneGap    = 14.0f;  // centre-to-centre — 7px of clear space between dots
        float zoneClusterW = zoneCount > 0
            ? (kZoneDotR * 2.0f + (zoneCount - 1) * kZoneGap)
            : 0.0f;

        drawLayerRow(draw, layer, listStart.x, rowY, panelWidth, rowHeight,
                     selected, rowHovered, false, thumbSize, zoneClusterW);

        // Opacity scrub — per-row InvisibleButton over the % text. Because
        // ImGui's item system owns the press, ImGui NEVER treats this
        // click as a "window drag" gesture on the Layers panel. We drive
        // the scrub off IsItemActivated / IsItemActive + GetMouseDragDelta.
        {
            const float kInsetX  = 0.0f;
            float pctRight       = listStart.x + panelWidth - kInsetX - LP::kCardPadX;
            char obuf[8];
            snprintf(obuf, sizeof(obuf), "%d%%",
                     (int)(layer->opacity * 100.0f + 0.5f));
            ImVec2 psz = ImGui::GetFont()->CalcTextSizeA(10.0f, FLT_MAX, 0.0f, obuf);
            float pctLeft = pctRight - psz.x - 6.0f;
            float hitH    = 18.0f;
            float hitY    = rowY + rowHeight * 0.5f - hitH * 0.5f;
            float hitW    = pctRight - pctLeft + 12.0f;
            ImGui::SetCursorScreenPos(ImVec2(pctLeft - 4.0f, hitY));
            ImGui::PushID((int)(layer->id + 0xA0000000));
            ImGui::InvisibleButton("##opDrag", ImVec2(hitW, hitH));
            if (ImGui::IsItemActivated()) {
                m_opacityDragIdx    = stackIdx;
                m_opacityDragStart  = layer->opacity;
            }
            if (ImGui::IsItemActive()) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                float newOp = m_opacityDragStart + d.x * 0.005f;
                if (newOp < 0.0f) newOp = 0.0f;
                if (newOp > 1.0f) newOp = 1.0f;
                layer->opacity = newOp;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                selectedLayer = stackIdx;
            } else if (ImGui::IsItemDeactivated()) {
                m_opacityDragIdx = -1;
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            ImGui::PopID();
        }

        // Zone visibility dots — right-aligned stack sitting ABOVE the
        // opacity % so both land in the same right cluster.
        if (zoneCount > 0) {
            float rowCenterY = rowY + rowHeight * 0.5f;
            // Upper row of the right cluster — mirrors the name baseline,
            // a few pixels above the TYPE caption / opacity % line.
            float dotCy = rowCenterY - 7.0f;
            // Right-aligned: start from rightInnerEdge and walk LEFTWARD.
            const float kInsetX      = 0.0f;
            float rightInnerEdge = listStart.x + panelWidth - kInsetX - LP::kCardPadX;
            float dotX = rightInnerEdge - kZoneDotR;
            for (int zi = (int)zones->size() - 1; zi >= 0; zi--) {
                auto& z = *(*zones)[zi];
                bool inZone = z.showAllLayers || z.visibleLayerIds.count(layer->id);

                ImVec2 center(dotX, dotCy);
                ImU32 col = zoneColor(zi);
                ImU32 dimCol = IM_COL32((col & 0xFF) / 3, ((col >> 8) & 0xFF) / 3,
                                        ((col >> 16) & 0xFF) / 3, 110);

                if (inZone) draw->AddCircleFilled(center, kZoneDotR, col);
                else        draw->AddCircle     (center, kZoneDotR, dimCol, 0, 1.2f);

                if (!m_dragActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && windowHovered) {
                    float dx = mousePos.x - center.x, dy = mousePos.y - center.y;
                    if (dx*dx + dy*dy < (kZoneDotR + 4) * (kZoneDotR + 4)) {
                        if (z.showAllLayers) {
                            z.showAllLayers = false;
                            for (int li = 0; li < stack.count(); li++) {
                                z.visibleLayerIds.insert(stack[li]->id);
                            }
                            z.visibleLayerIds.erase(layer->id);
                        } else {
                            if (inZone) z.visibleLayerIds.erase(layer->id);
                            else        z.visibleLayerIds.insert(layer->id);
                        }
                    }
                }

                dotX -= kZoneGap;
            }
        }
    }

    // --- DRAW INSERTION LINE ---
    if (m_dragActive && m_insertIndex >= 0) {
        float lineY = listStart.y + m_insertIndex * rowHeight;
        float lx0 = listStart.x + 4;
        float lx1 = listStart.x + panelWidth - 4;
        fg->AddLine(ImVec2(lx0, lineY), ImVec2(lx1, lineY), kInsertGlow, 6.0f);
        fg->AddLine(ImVec2(lx0, lineY), ImVec2(lx1, lineY), kInsertLine, 2.0f);
        fg->AddCircleFilled(ImVec2(lx0, lineY), 4.0f, kInsertLine);
        fg->AddCircleFilled(ImVec2(lx1, lineY), 4.0f, kInsertLine);
    }

    // --- DRAW FLOATING DRAG ROW ---
    if (m_dragActive && m_dragIndex >= 0 && m_dragIndex < layerCount) {
        float floatY = mousePos.y - m_dragOffsetY;
        auto& dragLayer = stack[m_dragIndex];

        // Shadow
        fg->AddRectFilled(ImVec2(listStart.x + 2, floatY + 2),
                          ImVec2(listStart.x + panelWidth + 2, floatY + rowHeight + 2),
                          kDragShadow, 4.0f);
        // Background
        fg->AddRectFilled(ImVec2(listStart.x, floatY),
                          ImVec2(listStart.x + panelWidth, floatY + rowHeight),
                          IM_COL32(30, 38, 55, 240), 4.0f);
        // Border glow
        fg->AddRect(ImVec2(listStart.x, floatY),
                    ImVec2(listStart.x + panelWidth, floatY + rowHeight),
                    IM_COL32(255, 255, 255, 80), 4.0f, 0, 1.5f);

        drawLayerRow(fg, dragLayer, listStart.x, floatY, panelWidth, rowHeight,
                     true, false, false, thumbSize);
    }

    // --- INLINE RENAME ---
    if (m_renaming && m_renameIndex >= 0 && m_renameIndex < layerCount) {
        int rDisplay = layerCount - 1 - m_renameIndex;
        float rY = listStart.y + rDisplay * rowHeight + 4;
        // Align the rename InputText with the name column from drawLayerRow
        // (card inset + card padding + thumbnail + gap).
        float nameX = listStart.x + LP::kCardMarginX + LP::kCardPadX
                     + thumbSize + LP::kThumbToText;
        float inputW = panelWidth - (nameX - listStart.x) - 10;
        if (inputW < 40.0f) inputW = 40.0f;

        ImGui::SetCursorScreenPos(ImVec2(nameX, rY));
        ImGui::SetNextItemWidth(inputW);
        // On the first frame of rename, auto-focus the input so typing works
        // immediately without clicking it first.
        if (m_renameJustStarted) {
            ImGui::SetKeyboardFocusHere();
            m_renameJustStarted = false;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.14f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.18f, 0.25f, 1.0f));
        bool committed = ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        // Commit on click-away: IsItemDeactivatedAfterEdit fires when the
        // input loses focus after the user typed something. Also close (without
        // changes) on plain deactivation so the field doesn't stick around.
        bool deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        bool deactivated          = ImGui::IsItemDeactivated();
        ImGui::PopStyleColor(2);
        if (committed || deactivatedAfterEdit) {
            stack[m_renameIndex]->name = m_renameBuf;
            m_renaming = false;
            m_renameIndex = -1;
        } else if (deactivated || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_renaming = false;
            m_renameIndex = -1;
        }
    }

    // --- GROUP HEADERS ---
    // Draw group headers above their first member (visual only, lightweight)
    {
        std::set<uint32_t> drawnGroups;
        for (int displayIdx = 0; displayIdx < layerCount; displayIdx++) {
            int stackIdx = layerCount - 1 - displayIdx;
            auto& layer = stack[stackIdx];
            if (layer->groupId == 0) continue;
            if (drawnGroups.count(layer->groupId)) continue;
            drawnGroups.insert(layer->groupId);

            auto* grp = stack.group(layer->groupId);
            if (!grp) continue;

            float rowY = listStart.y + displayIdx * rowHeight;
            // Draw a subtle group bar above the first grouped layer
            float barY = rowY - 2;
            ImU32 grpCol = IM_COL32(255, 255, 255, 100);
            draw->AddRectFilled(ImVec2(listStart.x, barY - 14),
                                ImVec2(listStart.x + panelWidth, barY),
                                IM_COL32(0, 180, 235, 20), 2.0f);
            draw->AddLine(ImVec2(listStart.x + 4, barY), ImVec2(listStart.x + panelWidth - 4, barY), grpCol, 1.0f);

            // Group name
            ImVec2 textPos(listStart.x + 8, barY - 13);
            draw->AddText(textPos, grpCol, grp->name.c_str());

            // Count members
            int memberCount = 0;
            for (int i = 0; i < layerCount; i++) {
                if (stack[i]->groupId == layer->groupId) memberCount++;
            }
            char countBuf[16];
            snprintf(countBuf, sizeof(countBuf), "(%d)", memberCount);
            ImVec2 countSize = ImGui::CalcTextSize(countBuf);
            ImVec2 nameSize = ImGui::CalcTextSize(grp->name.c_str());
            draw->AddText(ImVec2(textPos.x + nameSize.x + 6, barY - 13), kTextMuted, countBuf);
        }
    }

    // Draw group bracket on left edge for grouped layers
    {
        for (int displayIdx = 0; displayIdx < layerCount; displayIdx++) {
            int stackIdx = layerCount - 1 - displayIdx;
            auto& layer = stack[stackIdx];
            if (layer->groupId == 0) continue;

            float rowY = listStart.y + displayIdx * rowHeight;
            // Left accent line for group membership
            draw->AddRectFilled(ImVec2(listStart.x, rowY),
                                ImVec2(listStart.x + 2, rowY + rowHeight),
                                IM_COL32(255, 255, 255, 60));
        }
    }

    // --- CONTEXT MENU ---
    if (ImGui::BeginPopup("LayerMenu")) {
        int ci = selectedLayer;
        if (ci >= 0 && ci < layerCount) {
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                auto& orig = stack[ci];
                auto dupe = std::make_shared<Layer>();
                dupe->name = orig->name + " copy";
                dupe->visible = orig->visible;
                dupe->opacity = orig->opacity;
                dupe->blendMode = orig->blendMode;
                dupe->position = orig->position;
                dupe->scale = orig->scale;
                dupe->rotation = orig->rotation;
                dupe->flipH = orig->flipH;
                dupe->flipV = orig->flipV;
                dupe->tileX = orig->tileX;
                dupe->tileY = orig->tileY;
                dupe->cropTop = orig->cropTop;
                dupe->cropBottom = orig->cropBottom;
                dupe->cropLeft = orig->cropLeft;
                dupe->cropRight = orig->cropRight;
                dupe->source = orig->source;
                stack.insertLayer(ci + 1, dupe);
                selectedLayer = ci + 1;
            }
            if (ImGui::MenuItem("Rename")) {
                m_renaming = true;
                m_renameJustStarted = true;
                m_renameIndex = ci;
                strncpy(m_renameBuf, stack[ci]->name.c_str(), sizeof(m_renameBuf) - 1);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Move to Top", nullptr, false, ci < layerCount - 1)) {
                stack.moveLayer(ci, layerCount - 1);
                selectedLayer = layerCount - 1;
            }
            if (ImGui::MenuItem("Move Up", nullptr, false, ci < layerCount - 1)) {
                stack.moveLayer(ci, ci + 1); selectedLayer = ci + 1;
            }
            if (ImGui::MenuItem("Move Down", nullptr, false, ci > 0)) {
                stack.moveLayer(ci, ci - 1); selectedLayer = ci - 1;
            }
            if (ImGui::MenuItem("Move to Bottom", nullptr, false, ci > 0)) {
                stack.moveLayer(ci, 0); selectedLayer = 0;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(stack[ci]->visible ? "Hide" : "Show")) {
                stack[ci]->visible = !stack[ci]->visible;
            }
            if (ImGui::MenuItem(stack[ci]->muted ? "Unmute" : "Mute")) {
                stack[ci]->muted = !stack[ci]->muted;
            }
            if (ImGui::MenuItem(stack[ci]->soloed ? "Unsolo" : "Solo")) {
                stack[ci]->soloed = !stack[ci]->soloed;
            }
            ImGui::Separator();
            // Group operations
            if (stack[ci]->groupId == 0) {
                if (ImGui::MenuItem("Create Group", "Ctrl+G")) {
                    uint32_t gid = stack.createGroup("Group");
                    stack[ci]->groupId = gid;
                }
            } else {
                if (ImGui::MenuItem("Ungroup")) {
                    uint32_t gid = stack[ci]->groupId;
                    stack.removeGroup(gid);
                }
                if (ImGui::BeginMenu("Add to Group")) {
                    for (auto& [gid, grp] : stack.groups()) {
                        if (ImGui::MenuItem(grp.name.c_str())) {
                            stack[ci]->groupId = gid;
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            // Show available groups to join
            if (stack[ci]->groupId == 0 && !stack.groups().empty()) {
                if (ImGui::BeginMenu("Add to Group")) {
                    for (auto& [gid, grp] : stack.groups()) {
                        if (ImGui::MenuItem(grp.name.c_str())) {
                            stack[ci]->groupId = gid;
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            if (ImGui::MenuItem("Delete")) {
                if (ci >= 0 && ci < stack.count() && stack[ci])
                    removedLayerIds.push_back(stack[ci]->id);
                stack.removeLayer(ci);
                selectedLayer = std::min(selectedLayer, stack.count() - 1);
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    // Keyboard shortcuts
    if (!m_renaming && selectedLayer >= 0 && selectedLayer < stack.count() && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (selectedLayer >= 0 && selectedLayer < stack.count() && stack[selectedLayer])
                removedLayerIds.push_back(stack[selectedLayer]->id);
            stack.removeLayer(selectedLayer);
            selectedLayer = std::min(selectedLayer, stack.count() - 1);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            int ci = selectedLayer;
            auto& orig = stack[ci];
            auto dupe = std::make_shared<Layer>();
            dupe->name = orig->name + " copy";
            dupe->visible = orig->visible;
            dupe->opacity = orig->opacity;
            dupe->blendMode = orig->blendMode;
            dupe->position = orig->position;
            dupe->scale = orig->scale;
            dupe->rotation = orig->rotation;
            dupe->flipH = orig->flipH;
            dupe->flipV = orig->flipV;
            dupe->tileX = orig->tileX;
            dupe->tileY = orig->tileY;
            dupe->cropTop = orig->cropTop;
            dupe->cropBottom = orig->cropBottom;
            dupe->cropLeft = orig->cropLeft;
            dupe->cropRight = orig->cropRight;
            dupe->source = orig->source;
            stack.insertLayer(ci + 1, dupe);
            selectedLayer = ci + 1;
        }
    }

    ImGui::End();
}
