#include "compositing/LayerStack.h"
#include <algorithm>

void LayerStack::addLayer(LayerPtr layer) {
    m_layers.push_back(std::move(layer));
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
