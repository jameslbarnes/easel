#pragma once
#include "ui/ViewportPanel.h"
#include <vector>
#include <memory>
#include <string>

struct MappingProfile;

class WarpEditor {
public:
    void render(MappingProfile& mapping, bool& maskEditMode,
                std::vector<std::unique_ptr<MappingProfile>>* allMappings = nullptr,
                int activeMappingIndex = 0,
                const std::vector<std::string>* zoneNames = nullptr,
                int activeZoneIndex = -1);

    bool wantsLoadOBJ() const { return m_wantsLoadOBJ; }
    void clearLoadOBJ() { m_wantsLoadOBJ = false; }

    // Selected MAPPING-mode calibration pattern. Index into the pattern set
    // generated in Application (Grid / Checkerboard / Crosshair / Circles /
    // Dots / Solid White). Application reads this to pick the warp source.
    int  testPatternIndex() const { return m_testPattern; }

private:
    bool m_wantsLoadOBJ = false;
    bool m_renaming = false;
    char m_renameBuf[128] = {};
    int  m_testPattern = 0;   // default = Grid
};
