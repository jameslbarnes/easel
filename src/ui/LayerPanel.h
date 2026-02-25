#pragma once
#include "compositing/LayerStack.h"
#include <string>

class LayerPanel {
public:
    void render(LayerStack& stack, int& selectedLayer);

private:
    // Manual drag state
    bool m_dragActive = false;
    int m_dragIndex = -1;         // Stack index being dragged
    int m_insertIndex = -1;       // Where to insert on drop (display-space)
    float m_dragStartY = 0;       // Mouse Y when drag began
    float m_dragOffsetY = 0;      // Offset from row top to mouse

    // Inline rename
    bool m_renaming = false;
    int m_renameIndex = -1;
    char m_renameBuf[256] = {};
};
