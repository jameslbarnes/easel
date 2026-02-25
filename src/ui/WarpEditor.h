#pragma once
#include "ui/ViewportPanel.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"

class WarpEditor {
public:
    void render(CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                ViewportPanel::WarpMode& mode);
};
