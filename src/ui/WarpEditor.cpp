#include "ui/WarpEditor.h"
#include "ui/UIManager.h"
#include "ui/PropertyPanel.h"
#include "app/MappingProfile.h"
#include <imgui.h>
#include <filesystem>

static const ImU32  kSectionLine = IM_COL32(255, 255, 255, 40);

static void sectionSep() {
    ImGui::Dummy(ImVec2(0, 2));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    draw->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + width, pos.y), kSectionLine);
    ImGui::Dummy(ImVec2(0, 3));
}

static bool accentButton(const char* label, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(4);
    return clicked;
}

static void modeButton(const char* label, int thisMode, int& currentMode, float width) {
    bool active = (currentMode == thisMode);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.125f, 0.165f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
    }
    if (ImGui::Button(label, ImVec2(width, 0))) currentMode = thisMode;
    ImGui::PopStyleColor(2);
}

void WarpEditor::render(MappingProfile& mapping, bool& maskEditMode,
                        std::vector<std::unique_ptr<MappingProfile>>* allMappings,
                        int activeMappingIndex,
                        const std::vector<std::string>* zoneNames,
                        int activeZoneIndex) {
    m_wantsLoadOBJ = false;

    // Match the Properties panel's airy padding EXACTLY so the two
    // right-column panels read as one rhythm. Properties uses (28, 22)
    // window pad / (10, 6) item / (12, 9) frame — was (20, 18) / (8, 12)
    // here, which gave Mapping a tighter, denser feel than Properties.
    PropertyPanel::PushPanelStyle();   // same inset/rhythm as every other panel
    ImGui::Begin("        ###Mapping");
    PropertyPanel::PopPanelStyle();

    // 6-pill nav is rendered at the right-dock host level (one bar total).

    // Consistent top title — same uppercase H2 + spacing as every other
    // right-dock parameter panel (Properties / Shaders / Camera / Audio).
    PropertyPanel::PanelSectionHeader("Mapping", /*firstSection=*/true);

    // Test-pattern selector — in MAPPING workspace the output is replaced by a
    // black/white calibration pattern so you align geometry, not live content.
    // Index order MUST match Application::m_mapPatterns.
    if (UIManager::sMode == UIManager::WorkspaceMode::Mapping) {
        static const char* kPatternNames[] = {
            "Grid", "Checkerboard", "Crosshair", "Concentric Circles",
            "Dots", "Solid White"
        };
        PropertyPanel::PanelLabel("TEST PATTERN");
        ImGui::Combo("##mapTestPattern", &m_testPattern,
                     kPatternNames, IM_ARRAYSIZE(kPatternNames));
    }

    // --- Mapping profile header ---
    if (allMappings && !allMappings->empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("Profile");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // Profile name (editable inline)
        if (m_renaming) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30);
            if (ImGui::InputText("##MapRename", m_renameBuf, sizeof(m_renameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                mapping.name = m_renameBuf;
                m_renaming = false;
            }
            if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) m_renaming = false;
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::Text("%s", mapping.name.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) {
                m_renaming = true;
                snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", mapping.name.c_str());
            }
        }

        ImGui::Dummy(ImVec2(0, 6));
    }

    auto& cornerPin = mapping.cornerPin;
    auto& meshWarp = mapping.meshWarp;
    auto& objMeshWarp = mapping.objMeshWarp;

    // Mode selector — compact dropdown. Replaces the old 3-button segmented
    // row to reclaim vertical space; selecting an item switches mode
    // identically to the buttons. Full-width control matches the other
    // dropdowns/inputs in this panel (SetNextItemWidth(-1) idiom).
    int modeInt = (int)mapping.warpMode;
    static const char* kModeNames[] = { "Corner Pin", "Mesh Warp", "OBJ Mesh" };

    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##WarpMode", &modeInt, kModeNames, IM_ARRAYSIZE(kModeNames));

    mapping.warpMode = (ViewportPanel::WarpMode)modeInt;

    ImGui::Dummy(ImVec2(0, 10));

    if (mapping.warpMode == ViewportPanel::WarpMode::CornerPin) {
        auto& corners = cornerPin.corners();
        const char* labels[] = {"BL", "BR", "TR", "TL"};
        // Label gutter width. NOTE: ImGui::SameLine(offset) measures the
        // offset from window->Pos.x, NOT from the padded content start.
        // The label Text() is drawn at CursorStartPos.x (= window pos +
        // WindowPadding.x, which is 28 here). A bare SameLine(32) therefore
        // landed the drag field at pos+32 — only 4px past the label's start
        // — so the full-width DragFloat2 painted on top of the BL/BR/TR/TL
        // labels, clipping them to a sliver at the panel's left edge.
        // Anchor the gutter to the real content start instead.
        const float kLabelColW    = 56.0f;  // label (~16px) + clear gap
        const float kLabelGutterX = ImGui::GetCursorStartPos().x + kLabelColW;
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
            ImGui::Text("%s", labels[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(kLabelGutterX);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat2("##corner", &corners[i][0], 0.01f, -1.5f, 1.5f, "%.2f");
            ImGui::PopID();
        }
        // Reset spans the same -1 width as the drag rows so the
        // bottom of the corner block aligns flush with the rows above.
        if (accentButton("Reset", -1)) {
            corners = {{
                {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}
            }};
        }
    } else if (mapping.warpMode == ViewportPanel::WarpMode::MeshWarp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::Text("%d x %d", meshWarp.cols(), meshWarp.rows());
        ImGui::PopStyleColor();

        float qw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) * 0.25f;
        if (accentButton("+C", qw)) meshWarp.addColumn();
        ImGui::SameLine();
        if (accentButton("-C", qw)) meshWarp.removeColumn();
        ImGui::SameLine();
        if (accentButton("+R", qw)) meshWarp.addRow();
        ImGui::SameLine();
        if (accentButton("-R", qw)) meshWarp.removeRow();

        if (accentButton("Reset", -1)) {
            meshWarp.resetGrid();
        }

        // Curve / straight points. Each control point is either smooth (a
        // Catmull-Rom curve runs through it) or a corner (segments touching it
        // go straight). Right-click a point in the viewport to toggle one;
        // these set them all at once. Smooth = circle, straight = square.
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        ImGui::TextWrapped("Right-click a point to toggle curve / straight");
        ImGui::PopStyleColor();
        float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (accentButton("All Smooth", hw))   meshWarp.setAllCorners(false);
        ImGui::SameLine();
        if (accentButton("All Straight", hw)) meshWarp.setAllCorners(true);
    } else if (mapping.warpMode == ViewportPanel::WarpMode::ObjMesh) {
        // Mesh file display
        if (objMeshWarp.isLoaded()) {
            std::string filename = std::filesystem::path(objMeshWarp.meshPath()).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
            ImGui::Text("%s", filename.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.7f));
            ImGui::Text("No mesh loaded");
            ImGui::PopStyleColor();
        }

        if (accentButton("Load Mesh...", -1)) {
            m_wantsLoadOBJ = true;
        }

        if (objMeshWarp.isLoaded()) {
            sectionSep();

            // Camera controls
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Camera");
            ImGui::PopStyleColor();

            auto& cam = objMeshWarp.camera();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("FOV", &cam.fovDeg, 10.0f, 120.0f, "%.0f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("Distance", &cam.distance, 0.5f, 20.0f, "%.1f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("Azimuth", &cam.azimuth, -3.14159f, 3.14159f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("Elevation", &cam.elevation, -1.5f, 1.5f, "%.2f");

            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Target", &cam.target[0], 0.01f, -10.0f, 10.0f, "%.2f");

            sectionSep();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("Model");
            ImGui::PopStyleColor();

            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("Scale", &objMeshWarp.modelScale(), 0.01f, 10.0f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("Position", &objMeshWarp.modelPosition()[0], 0.01f, -10.0f, 10.0f, "%.2f");

            if (accentButton("Reset Camera", -1)) {
                cam = OrbitCamera{};
                objMeshWarp.modelScale() = 1.0f;
                objMeshWarp.modelPosition() = {0.0f, 0.0f, 0.0f};
            }

            // Material toggles
            auto& mats = objMeshWarp.materials();
            if (!mats.empty()) {
                sectionSep();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Materials");
                ImGui::PopStyleColor();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.73f, 0.78f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                for (int mi = 0; mi < (int)mats.size(); mi++) {
                    ImGui::PushID(5000 + mi);
                    ImGui::Checkbox("##enabled", &mats[mi].textured);
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", mats[mi].name.c_str());

                    if (zoneNames && !zoneNames->empty()) {
                        const char* sourceLabel = "Current zone";
                        if (mats[mi].sourceZoneIndex >= 0 &&
                            mats[mi].sourceZoneIndex < (int)zoneNames->size()) {
                            sourceLabel = (*zoneNames)[mats[mi].sourceZoneIndex].c_str();
                        } else if (mats[mi].sourceZoneIndex >= 0) {
                            sourceLabel = "Missing zone";
                        }
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##sourceZone", sourceLabel)) {
                            std::string currentLabel = "Current zone";
                            if (activeZoneIndex >= 0 && activeZoneIndex < (int)zoneNames->size()) {
                                currentLabel += " (" + (*zoneNames)[activeZoneIndex] + ")";
                            }
                            if (ImGui::Selectable(currentLabel.c_str(), mats[mi].sourceZoneIndex < 0)) {
                                mats[mi].sourceZoneIndex = -1;
                            }
                            for (int zi = 0; zi < (int)zoneNames->size(); zi++) {
                                bool selected = mats[mi].sourceZoneIndex == zi;
                                if (ImGui::Selectable((*zoneNames)[zi].c_str(), selected)) {
                                    // Treat an explicit self-route as Current zone so it
                                    // can never sample its previous output recursively.
                                    mats[mi].sourceZoneIndex = zi == activeZoneIndex ? -1 : zi;
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Live zone feed rendered on this mesh face");
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 3));
                    ImGui::PopID();
                }
                ImGui::PopStyleColor(2);

                float halfBtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (accentButton("All On", halfBtnW)) {
                    for (auto& m : mats) m.textured = true;
                }
                ImGui::SameLine();
                if (accentButton("All Off", halfBtnW)) {
                    for (auto& m : mats) m.textured = false;
                }
            }
        }
    }

    ImGui::End();
}
