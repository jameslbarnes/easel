#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "compositing/LayerStack.h"
#include <vector>
#include <memory>
#include <string>

class CornerPinWarp;
class MeshWarp;
class ObjMeshWarp;
class MaskPath;
struct OutputZone;
struct MonitorInfo;

class ViewportPanel {
public:
    enum class WarpMode { CornerPin, MeshWarp, ObjMesh };
    enum class EditMode { Normal, Mask };

    void render(GLuint texture, CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                WarpMode warpMode, float projectorAspect = 16.0f / 9.0f,
                std::vector<std::unique_ptr<OutputZone>>* zones = nullptr,
                int* activeZone = nullptr,
                const std::vector<MonitorInfo>* monitors = nullptr,
                bool ndiAvailable = false,
                ObjMeshWarp* objMeshWarp = nullptr);

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

    // Orbit camera drag state (ObjMesh mode)
    bool m_orbitDragging = false;
    glm::vec2 m_orbitDragStart = {0, 0};
    float m_orbitStartAzimuth = 0;
    float m_orbitStartElevation = 0;

    // Zone tab rename state
    bool m_renaming = false;
    int m_renameIndex = -1;
    char m_renameBuf[128] = {};

    // The image area within the panel
    glm::vec2 m_imageOrigin = {0, 0};
    glm::vec2 m_imageSize = {0, 0};

    // Coordinate conversion helpers
    glm::vec2 screenToUV(glm::vec2 screen) const;
    glm::vec2 uvToScreenVec(glm::vec2 uv) const;
    glm::vec2 screenToNDC(glm::vec2 screen) const;
    glm::vec2 ndcToScreen(glm::vec2 ndc) const;
};
