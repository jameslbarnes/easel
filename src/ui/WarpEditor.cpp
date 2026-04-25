#include "ui/WarpEditor.h"
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
                        int activeMappingIndex) {
    m_wantsLoadOBJ = false;

    ImGui::Begin("        ###Mapping");

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

    // Mode selector — 3 buttons. No hairline dividers above/below; just
    // balanced vertical spacing so the row reads as a coherent segmented
    // control rather than two sections separated by lines.
    int modeInt = (int)mapping.warpMode;
    float thirdW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;

    modeButton("Corner Pin", 0, modeInt, thirdW);
    ImGui::SameLine();
    modeButton("Mesh Warp", 1, modeInt, thirdW);
    ImGui::SameLine();
    modeButton("OBJ Mesh", 2, modeInt, thirdW);

    mapping.warpMode = (ViewportPanel::WarpMode)modeInt;

    ImGui::Dummy(ImVec2(0, 10));

    if (mapping.warpMode == ViewportPanel::WarpMode::CornerPin) {
        auto& corners = cornerPin.corners();
        const char* labels[] = {"BL", "BR", "TR", "TL"};
        // Labels sit in a fixed-width column so the numeric fields line
        // up vertically and there's a clear gap between each label and
        // its value (previously 28px, now 56px).
        const float kLabelColW = 56.0f;
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
            ImGui::Text("%s", labels[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(kLabelColW);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat2("##corner", &corners[i][0], 0.01f, -1.5f, 1.5f, "%.2f");
            ImGui::PopID();
        }
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
                    ImGui::Checkbox(mats[mi].name.c_str(), &mats[mi].textured);
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
