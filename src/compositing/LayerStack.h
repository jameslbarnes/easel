#pragma once
#include "compositing/Layer.h"
#include <vector>
#include <memory>

class LayerStack {
public:
    using LayerPtr = std::shared_ptr<Layer>;

    void addLayer(LayerPtr layer);
    void insertLayer(int index, LayerPtr layer);
    void removeLayer(int index);
    void moveLayer(int from, int to);

    LayerPtr& operator[](int index) { return m_layers[index]; }
    const LayerPtr& operator[](int index) const { return m_layers[index]; }
    int count() const { return (int)m_layers.size(); }
    bool empty() const { return m_layers.empty(); }

    std::vector<LayerPtr>& layers() { return m_layers; }
    const std::vector<LayerPtr>& layers() const { return m_layers; }

private:
    std::vector<LayerPtr> m_layers;
};
