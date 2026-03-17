#pragma once
#include "ui/ViewportPanel.h"
#include "warp/CornerPinWarp.h"
#include "warp/MeshWarp.h"
#include "warp/ObjMeshWarp.h"

class WarpEditor {
public:
    void render(CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                ObjMeshWarp& objMeshWarp, ViewportPanel::WarpMode& mode);

    bool wantsLoadOBJ() const { return m_wantsLoadOBJ; }
    void clearLoadOBJ() { m_wantsLoadOBJ = false; }

private:
    bool m_wantsLoadOBJ = false;
};
