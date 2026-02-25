#include "ui/ViewportPanel.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"
#include "compositing/MaskPath.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// Theme colors
static const ImU32 kAccent        = IM_COL32(0, 200, 255, 255);
static const ImU32 kAccentDim     = IM_COL32(0, 140, 180, 255);
static const ImU32 kAccentSoft    = IM_COL32(0, 200, 255, 80);
static const ImU32 kAccentGlow    = IM_COL32(0, 200, 255, 30);
static const ImU32 kWhiteSoft     = IM_COL32(255, 255, 255, 140);
static const ImU32 kHandleOuter   = IM_COL32(255, 255, 255, 220);
static const ImU32 kMaskFill      = IM_COL32(0, 0, 0, 80);
static const ImU32 kMaskCurve     = IM_COL32(0, 200, 255, 200);
static const ImU32 kMaskCurveGlow = IM_COL32(0, 200, 255, 50);
static const ImU32 kHandleLine    = IM_COL32(255, 255, 255, 70);
static const ImU32 kHandleDot     = IM_COL32(255, 255, 255, 200);
static const ImU32 kHandleRing    = IM_COL32(0, 200, 255, 200);
static const ImU32 kSelectedFill  = IM_COL32(0, 220, 255, 255);
static const ImU32 kSelectedRing  = IM_COL32(255, 255, 255, 255);
static const ImU32 kPointFill     = IM_COL32(0, 180, 230, 255);
static const ImU32 kPointRing     = IM_COL32(255, 255, 255, 180);
static const ImU32 kBorderColor   = IM_COL32(0, 160, 220, 60);
static const ImU32 kBorderGlow    = IM_COL32(0, 160, 220, 15);
static const ImU32 kBBoxLine      = IM_COL32(0, 200, 255, 180);
static const ImU32 kBBoxGlow      = IM_COL32(0, 200, 255, 40);
static const ImU32 kBBoxDim       = IM_COL32(255, 255, 255, 50);
static const ImU32 kLHandleFill   = IM_COL32(255, 255, 255, 240);
static const ImU32 kLHandleStroke = IM_COL32(0, 200, 255, 255);
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

void ViewportPanel::render(GLuint texture, CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                           WarpMode warpMode, float projectorAspect) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Projector Preview");
    ImGui::PopStyleVar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_size = {avail.x, avail.y};

    if (texture && avail.x > 1 && avail.y > 1) {
        float panelAspect = avail.x / avail.y;
        float imgW, imgH;
        if (projectorAspect > panelAspect) {
            imgW = avail.x; imgH = avail.x / projectorAspect;
        } else {
            imgH = avail.y; imgW = avail.y * projectorAspect;
        }
        float offsetX = (avail.x - imgW) * 0.5f;
        float offsetY = (avail.y - imgH) * 0.5f;

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

    // --- Always draw and handle warp (unless in mask mode) ---
    if (m_editMode != EditMode::Mask && m_imageSize.x > 0 && m_imageSize.y > 0) {
        ImVec2 mousePos = ImGui::GetMousePos();
        glm::vec2 mouseNDC = screenToNDC({mousePos.x, mousePos.y});

        // Only start warp drag if not already dragging a layer
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered && !m_layerDragging) {
            int hit = -1;
            if (warpMode == WarpMode::CornerPin) {
                hit = cornerPin.hitTest(mouseNDC);
            } else {
                hit = meshWarp.hitTest(mouseNDC);
            }
            if (hit >= 0) {
                m_warpDragIndex = hit;
                m_warpDragging = true;
            }
        }

        if (m_warpDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            glm::vec2 clamped(std::max(-1.5f, std::min(1.5f, mouseNDC.x)),
                              std::max(-1.5f, std::min(1.5f, mouseNDC.y)));
            if (warpMode == WarpMode::CornerPin) {
                cornerPin.corners()[m_warpDragIndex] = clamped;
            } else {
                meshWarp.points()[m_warpDragIndex] = clamped;
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_warpDragging = false;
            m_warpDragIndex = -1;
        }

        // Draw warp handles
        ImDrawList* draw = ImGui::GetWindowDrawList();
        auto ndc2scr = [&](glm::vec2 ndc) -> ImVec2 { return toImVec2(ndcToScreen(ndc)); };

        if (warpMode == WarpMode::CornerPin) {
            const auto& corners = cornerPin.corners();
            const char* labels[] = {"BL", "BR", "TR", "TL"};

            for (int i = 0; i < 4; i++) {
                draw->AddLine(ndc2scr(corners[i]), ndc2scr(corners[(i + 1) % 4]), kAccentGlow, 6.0f);
                draw->AddLine(ndc2scr(corners[i]), ndc2scr(corners[(i + 1) % 4]), kAccent, 1.5f);
            }
            for (int i = 0; i < 4; i++) {
                ImVec2 p = ndc2scr(corners[i]);
                bool active = (m_warpDragging && m_warpDragIndex == i);
                draw->AddCircleFilled(p, active ? 14.0f : 10.0f, kAccentGlow);
                draw->AddCircleFilled(p, active ? 8.0f : 6.0f, active ? kAccent : kAccentDim);
                draw->AddCircle(p, active ? 8.0f : 6.0f, kHandleOuter, 0, 1.5f);
                draw->AddText(ImVec2(p.x + 12, p.y - 8), kWhiteSoft, labels[i]);
            }
        } else {
            const auto& points = meshWarp.points();
            int cols = meshWarp.cols(), rows = meshWarp.rows();
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols - 1; c++)
                    draw->AddLine(ndc2scr(points[r*cols+c]), ndc2scr(points[r*cols+c+1]), kAccentSoft, 1.0f);
            for (int c = 0; c < cols; c++)
                for (int r = 0; r < rows - 1; r++)
                    draw->AddLine(ndc2scr(points[r*cols+c]), ndc2scr(points[(r+1)*cols+c]), kAccentSoft, 1.0f);
            for (int i = 0; i < (int)points.size(); i++) {
                ImVec2 p = ndc2scr(points[i]);
                bool active = (m_warpDragging && m_warpDragIndex == i);
                draw->AddCircleFilled(p, active ? 6.0f : 4.0f, active ? kAccent : kPointFill);
                draw->AddCircle(p, active ? 6.0f : 4.0f, kPointRing, 0, 1.2f);
            }
        }
    }

    ImGui::End();
}

// ======== LAYER TRANSFORM OVERLAY ========

void ViewportPanel::renderLayerOverlay(LayerStack& stack, int& selectedLayer) {
    if (m_imageSize.x <= 0 || m_imageSize.y <= 0) return;
    if (m_editMode != EditMode::Normal) return;
    if (stack.count() == 0) return;
    // Don't interact if warp is being dragged
    bool warpBusy = m_warpDragging;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 mouseImGui = ImGui::GetMousePos();
    glm::vec2 mouseNDC = screenToNDC({mouseImGui.x, mouseImGui.y});
    float handleRadius = 5.0f;
    float handleHitRadius = 10.0f;

    auto getLayerCorners = [&](const std::shared_ptr<Layer>& layer, ImVec2 out[4]) {
        float sx = layer->scale.x * (layer->flipH ? -1.0f : 1.0f);
        float sy = layer->scale.y * (layer->flipV ? -1.0f : 1.0f);
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

    auto mid = [](ImVec2 a, ImVec2 b) -> ImVec2 {
        return ImVec2((a.x+b.x)*0.5f, (a.y+b.y)*0.5f);
    };
    auto dist = [](ImVec2 a, ImVec2 b) -> float {
        float dx = a.x-b.x, dy = a.y-b.y; return sqrtf(dx*dx+dy*dy);
    };
    auto cross2d = [](ImVec2 o, ImVec2 a, ImVec2 b) -> float {
        return (a.x-o.x)*(b.y-o.y)-(a.y-o.y)*(b.x-o.x);
    };
    auto pointInQuad = [&](ImVec2 corners[4], ImVec2 pt) -> bool {
        bool pos = true, neg = true;
        for (int j = 0; j < 4; j++) {
            float cp = cross2d(corners[j], corners[(j+1)%4], pt);
            if (cp < 0) pos = false;
            if (cp > 0) neg = false;
        }
        return pos || neg;
    };

    // Draw dim bounding boxes for non-selected visible layers
    for (int i = 0; i < stack.count(); i++) {
        if (!stack[i]->visible || !stack[i]->source || i == selectedLayer) continue;
        ImVec2 c[4]; getLayerCorners(stack[i], c);
        for (int j = 0; j < 4; j++)
            draw->AddLine(c[j], c[(j+1)%4], kBBoxDim, 1.0f);
    }

    // Selected layer handles
    if (selectedLayer >= 0 && selectedLayer < stack.count()) {
        auto& layer = stack[selectedLayer];
        if (layer->source) {
            ImVec2 corners[4];
            getLayerCorners(layer, corners);

            HandleType handleTypes[8] = {
                HandleType::TopLeft, HandleType::Top, HandleType::TopRight, HandleType::Right,
                HandleType::BottomRight, HandleType::Bottom, HandleType::BottomLeft, HandleType::Left
            };

            auto getHandles = [&](ImVec2 c[4], ImVec2 out[8]) {
                out[0]=c[0]; out[1]=mid(c[0],c[1]); out[2]=c[1]; out[3]=mid(c[1],c[2]);
                out[4]=c[2]; out[5]=mid(c[2],c[3]); out[6]=c[3]; out[7]=mid(c[3],c[0]);
            };

            ImVec2 handles[8];
            getHandles(corners, handles);

            // Click to start interaction
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered && !warpBusy) {
                m_handleDrag = HandleType::None;
                m_layerDragging = false;

                // Check resize handles
                for (int h = 0; h < 8; h++) {
                    if (dist(mouseImGui, handles[h]) < handleHitRadius) {
                        m_handleDrag = handleTypes[h];
                        m_layerDragging = true;
                        m_dragStartMouse = mouseNDC;
                        m_dragStartPos = layer->position;
                        m_dragStartScale = layer->scale;
                        break;
                    }
                }

                // Check body move
                if (!m_layerDragging && pointInQuad(corners, mouseImGui)) {
                    m_handleDrag = HandleType::Move;
                    m_layerDragging = true;
                    m_dragStartMouse = mouseNDC;
                    m_dragStartPos = layer->position;
                    m_dragStartScale = layer->scale;
                }

                // Click outside: try selecting another layer
                if (!m_layerDragging) {
                    for (int idx = stack.count()-1; idx >= 0; idx--) {
                        if (!stack[idx]->visible || !stack[idx]->source) continue;
                        ImVec2 c2[4]; getLayerCorners(stack[idx], c2);
                        if (pointInQuad(c2, mouseImGui)) {
                            selectedLayer = idx;
                            m_handleDrag = HandleType::Move;
                            m_layerDragging = true;
                            m_dragStartMouse = mouseNDC;
                            m_dragStartPos = stack[idx]->position;
                            m_dragStartScale = stack[idx]->scale;
                            break;
                        }
                    }
                }
            }

            // Dragging
            if (m_layerDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                auto& dl = stack[selectedLayer];
                glm::vec2 delta = mouseNDC - m_dragStartMouse;

                if (m_handleDrag == HandleType::Move) {
                    dl->position = m_dragStartPos + delta;
                } else {
                    float dx = delta.x, dy = delta.y;
                    glm::vec2 ns = m_dragStartScale;

                    switch (m_handleDrag) {
                    case HandleType::TopLeft:     ns.x = std::max(0.05f, ns.x - dx*0.5f); ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
                    case HandleType::TopRight:    ns.x = std::max(0.05f, ns.x + dx*0.5f); ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
                    case HandleType::BottomLeft:  ns.x = std::max(0.05f, ns.x - dx*0.5f); ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
                    case HandleType::BottomRight: ns.x = std::max(0.05f, ns.x + dx*0.5f); ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
                    case HandleType::Top:    ns.y = std::max(0.05f, ns.y + dy*0.5f); break;
                    case HandleType::Bottom: ns.y = std::max(0.05f, ns.y - dy*0.5f); break;
                    case HandleType::Left:   ns.x = std::max(0.05f, ns.x - dx*0.5f); break;
                    case HandleType::Right:  ns.x = std::max(0.05f, ns.x + dx*0.5f); break;
                    default: break;
                    }
                    dl->scale = ns;
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                m_layerDragging = false;
                m_handleDrag = HandleType::None;
            }

            // Draw selected bbox (re-get after potential drag)
            getLayerCorners(stack[selectedLayer], corners);
            for (int j = 0; j < 4; j++)
                draw->AddLine(corners[j], corners[(j+1)%4], kBBoxGlow, 4.0f);
            for (int j = 0; j < 4; j++)
                draw->AddLine(corners[j], corners[(j+1)%4], kBBoxLine, 1.5f);

            getHandles(corners, handles);
            for (int h = 0; h < 8; h++) {
                bool isCorner = (h % 2 == 0);
                float r = isCorner ? handleRadius : (handleRadius - 1.5f);
                bool active = (m_layerDragging && m_handleDrag == handleTypes[h]);

                if (isCorner) {
                    ImVec2 hMin(handles[h].x-r, handles[h].y-r);
                    ImVec2 hMax(handles[h].x+r, handles[h].y+r);
                    draw->AddRectFilled(hMin, hMax, kLHandleFill, 1.5f);
                    draw->AddRect(hMin, hMax, active ? kLHandleActive : kLHandleStroke, 1.5f, 0, 1.5f);
                } else {
                    draw->AddCircleFilled(handles[h], r, kLHandleFill);
                    draw->AddCircle(handles[h], r, active ? kLHandleActive : kLHandleStroke, 0, 1.5f);
                }
            }

            // Cursor hints
            if (!m_layerDragging && !warpBusy) {
                for (int h = 0; h < 8; h++) {
                    if (dist(mouseImGui, handles[h]) < handleHitRadius) {
                        if (h==0||h==4) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                        else if (h==2||h==6) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                        else if (h==1||h==5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                        else if (h==3||h==7) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        break;
                    }
                }
            }
        }
    }
}

// ======== MASK OVERLAY ========

void ViewportPanel::renderMaskOverlay(MaskPath& mask) {
    if (m_imageSize.x <= 0 || m_imageSize.y <= 0) return;
    if (m_editMode != EditMode::Mask) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 mousePos = ImGui::GetMousePos();
    glm::vec2 mouseUV = screenToUV({mousePos.x, mousePos.y});

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hovered) {
        int hi = mask.hitTestHandleIn(mouseUV, 0.025f);
        int ho = mask.hitTestHandleOut(mouseUV, 0.025f);
        int pt = mask.hitTestPoint(mouseUV, 0.03f);

        if (hi >= 0) { m_maskDragIndex = hi; m_maskDragType = 2; m_maskSelectedPoint = hi; }
        else if (ho >= 0) { m_maskDragIndex = ho; m_maskDragType = 3; m_maskSelectedPoint = ho; }
        else if (pt >= 0) { m_maskDragIndex = pt; m_maskDragType = 1; m_maskSelectedPoint = pt; }
        else {
            float t; int edge = mask.hitTestEdge(mouseUV, 0.02f, t);
            if (edge >= 0 && mask.count() >= 2) {
                mask.insertPoint(edge, t);
                m_maskSelectedPoint = edge+1; m_maskDragIndex = m_maskSelectedPoint; m_maskDragType = 1;
            } else {
                mask.addPoint(mouseUV);
                m_maskSelectedPoint = mask.count()-1; m_maskDragIndex = m_maskSelectedPoint; m_maskDragType = 1;
            }
        }
    }

    if (m_maskDragType > 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto& pts = mask.points();
        if (m_maskDragIndex >= 0 && m_maskDragIndex < (int)pts.size()) {
            auto& pt = pts[m_maskDragIndex];
            glm::vec2 clamped = glm::clamp(mouseUV, glm::vec2(0.0f), glm::vec2(1.0f));
            if (m_maskDragType == 1) { pt.position = clamped; }
            else if (m_maskDragType == 2) {
                pt.handleIn = clamped - pt.position;
                if (pt.smooth) { float l = glm::length(pt.handleOut); if (l<0.001f) l=glm::length(pt.handleIn); pt.handleOut = glm::normalize(-pt.handleIn)*l; }
            } else if (m_maskDragType == 3) {
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
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_maskSelectedPoint = -1;

    const auto& pts = mask.points();
    if (pts.empty()) return;

    if (pts.size() >= 3) {
        auto tv = mask.tessellate(24);
        if (tv.size() >= 3) {
            ImVec2 cs = toImVec2(uvToScreenVec(mask.centroid()));
            for (int i = 0; i < (int)tv.size(); i++) {
                int j = (i+1) % (int)tv.size();
                draw->AddTriangleFilled(cs, toImVec2(uvToScreenVec(tv[i])), toImVec2(uvToScreenVec(tv[j])), kMaskFill);
            }
        }
    }
    if (pts.size() >= 2) {
        int n = (int)pts.size(), edges = mask.closed() ? n : (n-1);
        for (int i = 0; i < edges; i++) {
            int j = (i+1)%n;
            ImVec2 p0=toImVec2(uvToScreenVec(pts[i].position)), c0=toImVec2(uvToScreenVec(pts[i].position+pts[i].handleOut));
            ImVec2 c1=toImVec2(uvToScreenVec(pts[j].position+pts[j].handleIn)), p1=toImVec2(uvToScreenVec(pts[j].position));
            draw->AddBezierCubic(p0,c0,c1,p1,kMaskCurveGlow,5.0f,32);
            draw->AddBezierCubic(p0,c0,c1,p1,kMaskCurve,1.8f,32);
        }
    }
    for (int i = 0; i < (int)pts.size(); i++) {
        ImVec2 anchor = toImVec2(uvToScreenVec(pts[i].position));
        bool isSel = (i == m_maskSelectedPoint);
        if (glm::length(pts[i].handleIn) > 0.001f) {
            ImVec2 h = toImVec2(uvToScreenVec(pts[i].position+pts[i].handleIn));
            draw->AddLine(anchor,h,kHandleLine,1.0f); draw->AddCircleFilled(h,3.5f,kHandleDot); draw->AddCircle(h,3.5f,kHandleRing,0,1.2f);
        }
        if (glm::length(pts[i].handleOut) > 0.001f) {
            ImVec2 h = toImVec2(uvToScreenVec(pts[i].position+pts[i].handleOut));
            draw->AddLine(anchor,h,kHandleLine,1.0f); draw->AddCircleFilled(h,3.5f,kHandleDot); draw->AddCircle(h,3.5f,kHandleRing,0,1.2f);
        }
        if (isSel) {
            float s=5.5f;
            draw->AddQuadFilled(ImVec2(anchor.x,anchor.y-s-3),ImVec2(anchor.x+s+3,anchor.y),ImVec2(anchor.x,anchor.y+s+3),ImVec2(anchor.x-s-3,anchor.y),kAccentGlow);
            draw->AddQuadFilled(ImVec2(anchor.x,anchor.y-s),ImVec2(anchor.x+s,anchor.y),ImVec2(anchor.x,anchor.y+s),ImVec2(anchor.x-s,anchor.y),kSelectedFill);
            draw->AddQuad(ImVec2(anchor.x,anchor.y-s),ImVec2(anchor.x+s,anchor.y),ImVec2(anchor.x,anchor.y+s),ImVec2(anchor.x-s,anchor.y),kSelectedRing,1.5f);
        } else {
            draw->AddCircleFilled(anchor,4.5f,kPointFill); draw->AddCircle(anchor,4.5f,kPointRing,0,1.2f);
        }
    }
}
