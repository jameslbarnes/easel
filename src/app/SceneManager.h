#pragma once
#include "compositing/LayerStack.h"
#include "compositing/LayerEffect.h"
#include "compositing/BlendMode.h"
#include <string>
#include <vector>

// Snapshot of a single layer's state
struct SceneLayerState {
    uint32_t layerId = 0;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;
    glm::vec2 position = {0, 0};
    glm::vec2 scale = {1, 1};
    float rotation = 0;
    std::vector<LayerEffect> effects;
};

// A named scene = collection of layer snapshots
struct Scene {
    std::string name = "Scene";
    std::vector<SceneLayerState> layers;
};

class SceneManager {
public:
    // Save current state as a new scene
    void saveScene(const std::string& name, const LayerStack& stack);

    // Recall a scene (instantly sets layer states)
    void recallScene(int index, LayerStack& stack);

    // Scene list
    int count() const { return (int)m_scenes.size(); }
    Scene& operator[](int i) { return m_scenes[i]; }
    const Scene& operator[](int i) const { return m_scenes[i]; }

    // Remove
    void removeScene(int index);

    // Rename
    void renameScene(int index, const std::string& name);

    // Access for serialization
    std::vector<Scene>& scenes() { return m_scenes; }
    const std::vector<Scene>& scenes() const { return m_scenes; }

private:
    std::vector<Scene> m_scenes;
};
