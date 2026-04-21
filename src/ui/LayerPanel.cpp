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

// Draw a single layer row at a given screen Y position
static void drawLayerRow(ImDrawList* draw, const std::shared_ptr<Layer>& layer,
                         float x, float y, float width, float rowHeight,
                         bool selected, bool hovered, bool dimmed, float thumbSize) {
    ImVec2 rowMin(x, y);
    ImVec2 rowMax(x + width, y + rowHeight);

    // Background
    if (!dimmed) {
        if (selected) {
            draw->AddRectFilled(rowMin, rowMax, kRowSelected, 3.0f);
            draw->AddRectFilled(rowMin, ImVec2(x + 3, y + rowHeight), kAccent, 2.0f);
        }
        if (hovered && !selected) {
            draw->AddRectFilled(rowMin, rowMax, kRowHover, 3.0f);
        }
    }

    // Eye
    float eyeX = x + 14, eyeY = y + rowHeight * 0.5f;
    if (layer->visible) {
        draw->AddCircleFilled(ImVec2(eyeX, eyeY), 4.0f, kEyeOn);
        draw->AddCircle(ImVec2(eyeX, eyeY), 7.0f, kEyeOn, 0, 1.5f);
    } else {
        draw->AddCircle(ImVec2(eyeX, eyeY), 4.0f, kEyeOff, 0, 1.5f);
        draw->AddLine(ImVec2(eyeX - 5, eyeY + 4), ImVec2(eyeX + 5, eyeY - 4), kEyeOff, 1.5f);
    }

    // Thumbnail
    float thumbX = x + 28;
    float thumbY = y + (rowHeight - thumbSize) * 0.5f;
    ImVec2 tMin(thumbX, thumbY), tMax(thumbX + thumbSize, thumbY + thumbSize);
    draw->AddRectFilled(tMin, tMax, kThumbBg, 3.0f);
    GLuint texId = layer->source ? layer->source->textureId() : 0;
    if (texId != 0) {
        draw->PushClipRect(ImVec2(tMin.x + 1, tMin.y + 1), ImVec2(tMax.x - 1, tMax.y - 1), true);
        draw->AddImage((ImTextureID)(intptr_t)texId, tMin, tMax,
                       ImVec2(0, 1), ImVec2(1, 0),
                       IM_COL32(255, 255, 255, layer->visible ? 255 : 80));
        draw->PopClipRect();
    }
    draw->AddRect(tMin, tMax, kThumbBorder, 3.0f);

    // Badge
    if (layer->source) {
        const char* badge = getBadgeLabel(layer->source->typeName());
        ImU32 badgeCol = getBadgeColor(layer->source->typeName());
        ImVec2 bs = ImGui::CalcTextSize(badge);
        ImVec2 bMin(tMax.x - bs.x - 6, tMax.y - bs.y - 2);
        draw->AddRectFilled(bMin, tMax, badgeCol, 2.0f);
        draw->AddText(ImVec2(bMin.x + 3, bMin.y + 1), kBadgeText, badge);
    }

    // Name + dims
    float nameX = thumbX + thumbSize + 8;
    float nameY = y + 4;
    ImU32 nameCol = layer->visible ? kTextPrimary : kTextDim;
    if (dimmed) nameCol = IM_COL32(180, 190, 210, 140);
    draw->AddText(ImVec2(nameX, nameY), nameCol, layer->name.c_str());
    if (layer->source) {
        char dim[64];
        snprintf(dim, sizeof(dim), "%dx%d", layer->source->width(), layer->source->height());
        draw->AddText(ImVec2(nameX, nameY + 16), kTextMuted, dim);
    }

    // Opacity bar
    if (width > 140) {
        float barW = 46.0f, barH = 3.0f;
        float barX = rowMax.x - barW - 8;
        float barY = y + rowHeight * 0.5f + 6;
        draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), kOpacityBg, 2.0f);
        draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * layer->opacity, barY + barH), kOpacityFill, 2.0f);
        char opBuf[16];
        snprintf(opBuf, sizeof(opBuf), "%d%%", (int)(layer->opacity * 100.0f + 0.5f));
        ImVec2 opSz = ImGui::CalcTextSize(opBuf);
        draw->AddText(ImVec2(barX + barW - opSz.x, barY - 13), kTextDim, opBuf);
    }
}

// Zone / layer-row accent colors — vibrant so layers assigned to different
// zones are easy to tell apart at a glance. Avoids cyan (project-wide ban)
// but uses a distinct rainbow set for clarity.
static ImU32 zoneColor(int idx) {
    static const ImU32 colors[] = {
        IM_COL32(255, 100, 180, 230),  // pink
        IM_COL32(255, 150, 60,  230),  // orange
        IM_COL32(110, 220, 130, 230),  // green
        IM_COL32(200, 120, 255, 230),  // purple
        IM_COL32(255, 220, 80,  230),  // yellow
        IM_COL32(255, 110, 110, 230),  // red
        IM_COL32(120, 200, 255, 230),  // soft sky (non-cyan)
        IM_COL32(190, 190, 190, 230),  // neutral
    };
    return colors[idx % 8];
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

    float thumbSize = 30.0f;
    float rowHeight = 42.0f;
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

    // Detect mouse-down on a row to start potential drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && windowHovered && !m_renaming) {
        float relY = mousePos.y - listStart.y;
        int displayIdx = (int)(relY / rowHeight);
        if (displayIdx >= 0 && displayIdx < layerCount && relY >= 0) {
            int stackIdx = layerCount - 1 - displayIdx;

            // Eye toggle zone
            if (mousePos.x < listStart.x + 26) {
                stack[stackIdx]->visible = !stack[stackIdx]->visible;
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

        // Alternate row bg
        ImVec2 rMin(listStart.x, rowY);
        ImVec2 rMax(listStart.x + panelWidth, rowY + rowHeight);
        draw->AddRectFilled(rMin, rMax, (displayIdx % 2 == 0) ? kRowBg : kRowBgAlt, 3.0f);

        // Hover (only when not dragging)
        bool rowHovered = !m_dragActive &&
                          mousePos.x >= rMin.x && mousePos.x < rMax.x &&
                          mousePos.y >= rMin.y && mousePos.y < rMax.y && windowHovered;

        drawLayerRow(draw, layer, listStart.x, rowY, panelWidth, rowHeight,
                     selected, rowHovered, false, thumbSize);

        // Zone visibility dots (only when multiple zones exist)
        if (zones && zones->size() > 1) {
            float dotX = listStart.x + panelWidth - 8;
            float dotY = rowY + 6;
            float dotR = 4.0f;
            float dotSpacing = 12.0f;

            for (int zi = (int)zones->size() - 1; zi >= 0; zi--) {
                auto& z = *(*zones)[zi];
                bool inZone = z.showAllLayers || z.visibleLayerIds.count(layer->id);

                ImVec2 center(dotX, dotY + dotR);
                ImU32 col = zoneColor(zi);
                ImU32 dimCol = IM_COL32((col & 0xFF) / 3, ((col >> 8) & 0xFF) / 3,
                                        ((col >> 16) & 0xFF) / 3, 100);

                if (inZone) {
                    draw->AddCircleFilled(center, dotR, col);
                } else {
                    draw->AddCircle(center, dotR, dimCol, 0, 1.2f);
                }

                // Click to toggle zone membership
                if (!m_dragActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && windowHovered) {
                    float dx = mousePos.x - center.x, dy = mousePos.y - center.y;
                    if (dx*dx + dy*dy < (dotR + 4) * (dotR + 4)) {
                        if (z.showAllLayers) {
                            // Transition to explicit visibility: add all current layers, then toggle this one
                            z.showAllLayers = false;
                            for (int li = 0; li < stack.count(); li++) {
                                z.visibleLayerIds.insert(stack[li]->id);
                            }
                            z.visibleLayerIds.erase(layer->id);
                        } else {
                            if (inZone) {
                                z.visibleLayerIds.erase(layer->id);
                            } else {
                                z.visibleLayerIds.insert(layer->id);
                            }
                        }
                    }
                }

                dotX -= dotSpacing;
            }
        }

        // Separator
        if (displayIdx < layerCount - 1) {
            draw->AddLine(ImVec2(rMin.x + 28, rMax.y), ImVec2(rMax.x - 8, rMax.y),
                          IM_COL32(255, 255, 255, 8));
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
        float nameX = listStart.x + 28 + thumbSize + 8;
        float inputW = panelWidth - (nameX - listStart.x) - 10;
        if (inputW < 40.0f) inputW = 40.0f;

        ImGui::SetCursorScreenPos(ImVec2(nameX, rY));
        ImGui::SetNextItemWidth(inputW);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.14f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.18f, 0.25f, 1.0f));
        if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            stack[m_renameIndex]->name = m_renameBuf;
            m_renaming = false;
            m_renameIndex = -1;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
