#include "compositing/LayerStack.h"
#include <algorithm>

void LayerStack::addLayer(LayerPtr layer) {
    m_layers.push_back(std::move(layer));
}

void LayerStack::insertLayer(int index, LayerPtr layer) {
    if (index < 0) index = 0;
    if (index > (int)m_layers.size()) index = (int)m_layers.size();
    m_layers.insert(m_layers.begin() + index, std::move(layer));
}

void LayerStack::removeLayer(int index) {
    if (index >= 0 && index < (int)m_layers.size()) {
        m_layers.erase(m_layers.begin() + index);
    }
}

void LayerStack::moveLayer(int from, int to) {
    if (from < 0 || from >= (int)m_layers.size()) return;
    if (to < 0 || to >= (int)m_layers.size()) return;
    if (from == to) return;

    auto layer = m_layers[from];
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, layer);
}

uint32_t LayerStack::createGroup(const std::string& name) {
    uint32_t id = m_nextGroupId++;
    m_groups[id] = {name, false, true};
    return id;
}

void LayerStack::removeGroup(uint32_t groupId) {
    for (auto& layer : m_layers) {
        if (layer->groupId == groupId) {
            layer->groupId = 0;
        }
    }
    m_groups.erase(groupId);
}

LayerGroup* LayerStack::group(uint32_t groupId) {
    auto it = m_groups.find(groupId);
    return (it != m_groups.end()) ? &it->second : nullptr;
}
