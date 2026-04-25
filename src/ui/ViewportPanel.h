#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "compositing/LayerStack.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>

class CornerPinWarp;
class MeshWarp;
class ObjMeshWarp;
class MaskPath;
struct OutputZone;
struct MonitorInfo;
struct MappingProfile;

class ViewportPanel {
public:
    enum class WarpMode { CornerPin, MeshWarp, ObjMesh };
    enum class EditMode { Normal, Mask };

    void render(GLuint texture, MappingProfile* mapping,
                float projectorAspect = 16.0f / 9.0f,
                std::vector<std::unique_ptr<OutputZone>>* zones = nullptr,
                int* activeZone = nullptr,
                const std::vector<MonitorInfo>* monitors = nullptr,
                bool ndiAvailable = false,
                int editorMonitor = -1,
                const std::vector<std::unique_ptr<MappingProfile>>* allMappings = nullptr,
                std::function<void()> inlineSetupSection = nullptr);

    // Shared secondary-nav row (CANVAS/STAGE pills on the left, zone tabs +
    // OUTPUT combo + composition chip + Fullscreen on the right). Rendered
    // by both the Canvas viewport and the Stage panel so switching
    // workspaces doesn't shift the nav geometry.
    void renderNavBar(bool stageActive,
                      std::vector<std::unique_ptr<OutputZone>>* zones,
                      int* activeZone,
                      const std::vector<MonitorInfo>* monitors,
                      bool ndiAvailable,
                      int editorMonitor);

    // Layer transform overlay — drag to move, handles to resize
    void renderLayerOverlay(LayerStack& stack, int& selectedLayer, int canvasW = 1920, int canvasH = 1080);

    // Mask editing overlay
    // layerTransform: the full layer transform (getTransformMatrix() * nativeScale)
    // used to convert between canvas UV and layer UV for correct mask placement.
    // zoneIndex: used for zone-colored mask overlays (-1 = layer mask, uses white)
    void renderMaskOverlay(MaskPath& mask, const glm::mat3& layerTransform = glm::mat3(1.0f), int zoneIndex = -1);

    void setEditMode(EditMode m) { m_editMode = m; }
    EditMode editMode() const { return m_editMode; }

    // Signals from overlay to application
    bool wantsMaskEdit() const { return m_wantsMaskEdit; }
    void clearMaskEditSignal() { m_wantsMaskEdit = false; }
    glm::vec2 maskEditClickUV() const { return m_maskEditClickUV; }  // where the edge click happened

    // Signal: user clicked Save in the mask edit banner (or pressed Esc while editing).
    // Application should respond by exiting mask edit mode.
    bool wantsExitMaskMode() const { return m_wantsExitMaskMode; }
    void clearExitMaskSignal() { m_wantsExitMaskMode = false; }

    // Signal: user clicked the Fullscreen button in the viewport toolbar.
    // Application should respond by toggling editor fullscreen.
    bool wantsFullscreenToggle() const { return m_wantsFullscreenToggle; }
    void clearFullscreenSignal() { m_wantsFullscreenToggle = false; }
    void setEditorFullscreen(bool on) { m_editorFullscreenHint = on; }

    void setLayerSelected(bool sel) { m_layerSelected = sel; }
    bool isHovered() const { return m_hovered; }
    glm::vec2 size() const { return m_size; }
    glm::vec2 imageOrigin() const { return m_imageOrigin; }
    glm::vec2 imageSize() const { return m_imageSize; }

    // Canvas zoom
    float zoom() const { return m_zoom; }
    void resetZoom() { m_zoom = 1.0f; m_pan = {0, 0}; }

    // Reset ALL drag/interaction state (called on zone switch). Covers every
    // stateful flag so the canvas never gets stuck thinking a drag is in progress
    // after the user clicks away mid-drag (e.g. to switch zones or focus a tab).
    void resetDragState() {
        m_warpDragIndex = -1;
        m_warpDragging = false;
        m_maskDragIndex = -1;
        m_maskDragType = 0;
        m_maskSelectedPoint = -1;
        m_maskSelectedPoints.clear();
        m_maskBoxSelecting = false;
        m_panDragging = false;
        m_layerDragging = false;
        m_handleDrag = HandleType::None;
        m_orbitDragging = false;
    }

private:
    glm::vec2 m_size = {800, 600};
    bool m_hovered = false;
    EditMode m_editMode = EditMode::Normal;
    bool m_layerSelected = false;

    // Warp dragging state
    int m_warpDragIndex = -1;
    bool m_warpDragging = false;

    // Mask editing state
    int m_maskDragIndex = -1;
    int m_maskDragType = 0;
    int m_maskSelectedPoint = -1;
    std::vector<int> m_maskSelectedPoints; // multi-select (indices into MaskPath)
    bool m_maskBoxSelecting = false;
    glm::vec2 m_maskBoxStart = {0, 0};
public:
    std::vector<int>& maskSelectedPoints() { return m_maskSelectedPoints; }
private:

    // Layer transform state
    enum class HandleType { None, Move, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    HandleType m_handleDrag = HandleType::None;
    bool m_layerDragging = false;
    glm::vec2 m_dragStartMouse = {0, 0};
    glm::vec2 m_dragStartPos = {0, 0};
    glm::vec2 m_dragStartScale = {1, 1};
    float m_dragStartRotation = 0.0f; // rotation at drag start (for local-space projection)
    float m_dragStartRatio = 1.0f;  // aspect ratio at drag start (for shift-constrain)

    // Signal: double-click corner or edge-click to enter mask edit
    bool m_wantsMaskEdit = false;
    bool m_wantsExitMaskMode = false;
    glm::vec2 m_maskEditClickUV = {0, 0};

    // Fullscreen toggle — set by the inline toolbar button, read by Application.
    bool m_wantsFullscreenToggle = false;
    bool m_editorFullscreenHint = false;  // read-only display state

    // Orbit camera drag state (ObjMesh mode)
    bool m_orbitDragging = false;
    glm::vec2 m_orbitDragStart = {0, 0};
    float m_orbitStartAzimuth = 0;
    float m_orbitStartElevation = 0;

    // Zone tab rename state
    bool m_renaming = false;
    int m_renameIndex = -1;
    char m_renameBuf[128] = {};

    // Canvas zoom & pan
    float m_zoom = 1.0f;
    glm::vec2 m_pan = {0, 0};
    bool m_panDragging = false;
    glm::vec2 m_panDragStart = {0, 0};
    glm::vec2 m_panStart = {0, 0};

    // The image area within the panel
    glm::vec2 m_imageOrigin = {0, 0};
    glm::vec2 m_imageSize = {0, 0};

    // Viewport panel clip rect (screen-space) for clipping overlays
    glm::vec2 m_panelMin = {0, 0};
    glm::vec2 m_panelMax = {0, 0};
    bool m_panelVisible = false;

    // Coordinate conversion helpers
    glm::vec2 screenToUV(glm::vec2 screen) const;
    glm::vec2 uvToScreenVec(glm::vec2 uv) const;
    glm::vec2 screenToNDC(glm::vec2 screen) const;
    glm::vec2 ndcToScreen(glm::vec2 ndc) const;
};
