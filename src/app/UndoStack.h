#pragma once
#include "compositing/LayerStack.h"
#include "compositing/MaskPath.h"
#include "sources/ShaderSource.h"
#include <vector>
#include <deque>

struct LayerSnapshot {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;
    glm::vec2 position = {0.0f, 0.0f};
    glm::vec2 scale = {1.0f, 1.0f};
    float rotation = 0.0f;
    bool flipH = false;
    bool flipV = false;
    int tileX = 1, tileY = 1;
    float cropTop = 0.0f, cropBottom = 0.0f;
    float cropLeft = 0.0f, cropRight = 0.0f;
    bool maskEnabled = false;
    std::vector<MaskPoint> maskPoints;
    bool maskClosed = true;

    // Shader param values (empty if not a shader source)
    std::vector<std::variant<float, glm::vec4, bool, glm::vec2, std::string>> shaderParamValues;

    // Content source pointer (shared — not deep-copied)
    std::shared_ptr<ContentSource> source;
};

struct SceneSnapshot {
    std::vector<LayerSnapshot> layers;
    int selectedLayer = -1;
};

class UndoStack {
public:
    void pushState(const LayerStack& stack, int selectedLayer) {
        SceneSnapshot snap = capture(stack, selectedLayer);
        m_undoStack.push_back(std::move(snap));
        if ((int)m_undoStack.size() > m_maxEntries) {
            m_undoStack.pop_front();
        }
        m_redoStack.clear();
    }

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    void undo(LayerStack& stack, int& selectedLayer) {
        if (m_undoStack.empty()) return;
        m_redoStack.push_back(capture(stack, selectedLayer));
        restore(m_undoStack.back(), stack, selectedLayer);
        m_undoStack.pop_back();
    }

    void redo(LayerStack& stack, int& selectedLayer) {
        if (m_redoStack.empty()) return;
        m_undoStack.push_back(capture(stack, selectedLayer));
        restore(m_redoStack.back(), stack, selectedLayer);
        m_redoStack.pop_back();
    }

private:
    static constexpr int m_maxEntries = 50;
    std::deque<SceneSnapshot> m_undoStack;
    std::deque<SceneSnapshot> m_redoStack;

    static SceneSnapshot capture(const LayerStack& stack, int selectedLayer) {
        SceneSnapshot snap;
        snap.selectedLayer = selectedLayer;
        for (int i = 0; i < stack.count(); i++) {
            const auto& layer = stack[i];
            LayerSnapshot ls;
            ls.name = layer->name;
            ls.visible = layer->visible;
            ls.opacity = layer->opacity;
            ls.blendMode = layer->blendMode;
            ls.position = layer->position;
            ls.scale = layer->scale;
            ls.rotation = layer->rotation;
            ls.flipH = layer->flipH;
            ls.flipV = layer->flipV;
            ls.tileX = layer->tileX;
            ls.tileY = layer->tileY;
            ls.cropTop = layer->cropTop;
            ls.cropBottom = layer->cropBottom;
            ls.cropLeft = layer->cropLeft;
            ls.cropRight = layer->cropRight;
            ls.maskEnabled = layer->maskEnabled;
            ls.maskPoints = layer->maskPath.points();
            ls.maskClosed = layer->maskPath.closed();
            ls.source = layer->source;

            // Snapshot shader param values
            if (layer->source && layer->source->isShader()) {
                auto* ss = static_cast<ShaderSource*>(layer->source.get());
                for (const auto& input : ss->inputs()) {
                    ls.shaderParamValues.push_back(input.value);
                }
            }

            snap.layers.push_back(std::move(ls));
        }
        return snap;
    }

    static void restore(const SceneSnapshot& snap, LayerStack& stack, int& selectedLayer) {
        // Rebuild layer stack from snapshot
        while (stack.count() > 0) {
            stack.removeLayer(0);
        }

        for (const auto& ls : snap.layers) {
            auto layer = std::make_shared<Layer>();
            layer->name = ls.name;
            layer->visible = ls.visible;
            layer->opacity = ls.opacity;
            layer->blendMode = ls.blendMode;
            layer->position = ls.position;
            layer->scale = ls.scale;
            layer->rotation = ls.rotation;
            layer->flipH = ls.flipH;
            layer->flipV = ls.flipV;
            layer->tileX = ls.tileX;
            layer->tileY = ls.tileY;
            layer->cropTop = ls.cropTop;
            layer->cropBottom = ls.cropBottom;
            layer->cropLeft = ls.cropLeft;
            layer->cropRight = ls.cropRight;
            layer->maskEnabled = ls.maskEnabled;
            layer->maskPath.points() = ls.maskPoints;
            layer->maskPath.setClosed(ls.maskClosed);
            layer->source = ls.source;

            // Restore shader param values
            if (layer->source && layer->source->isShader() && !ls.shaderParamValues.empty()) {
                auto* ss = static_cast<ShaderSource*>(layer->source.get());
                auto& inputs = ss->inputs();
                for (int i = 0; i < (int)ls.shaderParamValues.size() && i < (int)inputs.size(); i++) {
                    inputs[i].value = ls.shaderParamValues[i];
                }
            }

            stack.addLayer(layer);
        }

        selectedLayer = snap.selectedLayer;
    }
};
