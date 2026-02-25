#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "compositing/LayerStack.h"

class CornerPinWarp;
class MeshWarp;
class MaskPath;

class ViewportPanel {
public:
    enum class WarpMode { CornerPin, MeshWarp };
    enum class EditMode { Normal, Mask };

    void render(GLuint texture, CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                WarpMode warpMode, float projectorAspect = 16.0f / 9.0f);

    // Layer transform overlay — drag to move, handles to resize
    void renderLayerOverlay(LayerStack& stack, int& selectedLayer);

    // Mask editing overlay
    void renderMaskOverlay(MaskPath& mask);

    void setEditMode(EditMode m) { m_editMode = m; }
    EditMode editMode() const { return m_editMode; }

    bool isHovered() const { return m_hovered; }
    glm::vec2 size() const { return m_size; }
    glm::vec2 imageOrigin() const { return m_imageOrigin; }
    glm::vec2 imageSize() const { return m_imageSize; }

private:
    glm::vec2 m_size = {800, 600};
    bool m_hovered = false;
    EditMode m_editMode = EditMode::Normal;

    // Warp dragging state
    int m_warpDragIndex = -1;
    bool m_warpDragging = false;

    // Mask editing state
    int m_maskDragIndex = -1;
    int m_maskDragType = 0;
    int m_maskSelectedPoint = -1;

    // Layer transform state
    enum class HandleType { None, Move, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    HandleType m_handleDrag = HandleType::None;
    bool m_layerDragging = false;
    glm::vec2 m_dragStartMouse = {0, 0};
    glm::vec2 m_dragStartPos = {0, 0};
    glm::vec2 m_dragStartScale = {1, 1};

    // The image area within the panel
    glm::vec2 m_imageOrigin = {0, 0};
    glm::vec2 m_imageSize = {0, 0};

    // Coordinate conversion helpers
    glm::vec2 screenToUV(glm::vec2 screen) const;
    glm::vec2 uvToScreenVec(glm::vec2 uv) const;
    glm::vec2 screenToNDC(glm::vec2 screen) const;
    glm::vec2 ndcToScreen(glm::vec2 ndc) const;
};
