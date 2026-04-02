#pragma once
#include "compositing/Layer.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>

struct LayerGroup {
    std::string name = "Group";
    bool collapsed = false;
    bool visible = true;
};

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

    // Group management
    uint32_t createGroup(const std::string& name = "Group");
    void removeGroup(uint32_t groupId);  // ungroups layers, doesn't delete them
    LayerGroup* group(uint32_t groupId);
    const std::unordered_map<uint32_t, LayerGroup>& groups() const { return m_groups; }
    std::unordered_map<uint32_t, LayerGroup>& groups() { return m_groups; }

private:
    std::vector<LayerPtr> m_layers;
    std::unordered_map<uint32_t, LayerGroup> m_groups;
    uint32_t m_nextGroupId = 1;
};
