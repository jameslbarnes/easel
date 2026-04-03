#include "app/SceneManager.h"

void SceneManager::saveScene(const std::string& name, const LayerStack& stack) {
    Scene scene;
    scene.name = name;
    for (int i = 0; i < stack.count(); i++) {
        const auto& layer = stack[i];
        SceneLayerState snap;
        snap.layerId = layer->id;
        snap.visible = layer->visible;
        snap.opacity = layer->opacity;
        snap.blendMode = layer->blendMode;
        snap.position = layer->position;
        snap.scale = layer->scale;
        snap.rotation = layer->rotation;
        snap.effects = layer->effects;
        scene.layers.push_back(snap);
    }
    m_scenes.push_back(scene);
}

void SceneManager::recallScene(int index, LayerStack& stack) {
    if (index < 0 || index >= (int)m_scenes.size()) return;
    const auto& scene = m_scenes[index];

    for (const auto& snap : scene.layers) {
        // Find layer by ID
        for (int i = 0; i < stack.count(); i++) {
            if (stack[i]->id == snap.layerId) {
                auto& layer = stack[i];
                layer->visible = snap.visible;
                layer->opacity = snap.opacity;
                layer->blendMode = snap.blendMode;
                layer->position = snap.position;
                layer->scale = snap.scale;
                layer->rotation = snap.rotation;
                layer->effects = snap.effects;
                break;
            }
        }
    }
}

void SceneManager::removeScene(int index) {
    if (index >= 0 && index < (int)m_scenes.size()) {
        m_scenes.erase(m_scenes.begin() + index);
    }
}

void SceneManager::renameScene(int index, const std::string& name) {
    if (index >= 0 && index < (int)m_scenes.size()) {
        m_scenes[index].name = name;
    }
}
