#include "ui/WarpEditor.h"
#include <imgui.h>

static const ImU32  kSectionLine = IM_COL32(0, 200, 255, 40);

static void sectionSep() {
    ImGui::Dummy(ImVec2(0, 2));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    draw->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + width, pos.y), kSectionLine);
    ImGui::Dummy(ImVec2(0, 3));
}

static bool accentButton(const char* label, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(4);
    return clicked;
}

void WarpEditor::render(CornerPinWarp& cornerPin, MeshWarp& meshWarp,
                        ViewportPanel::WarpMode& mode) {
    ImGui::Begin("Warp");

    // Mode selector as styled radio buttons
    int modeInt = (int)mode;

    float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // Corner Pin button
    bool cpActive = (modeInt == 0);
    if (cpActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.90f, 1.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.125f, 0.165f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
    }
    if (ImGui::Button("Corner Pin", ImVec2(halfW, 0))) modeInt = 0;
    ImGui::PopStyleColor(2);

    ImGui::SameLine();

    // Mesh Warp button
    bool mwActive = (modeInt == 1);
    if (mwActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.90f, 1.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.125f, 0.165f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
    }
    if (ImGui::Button("Mesh Warp", ImVec2(halfW, 0))) modeInt = 1;
    ImGui::PopStyleColor(2);

    mode = (ViewportPanel::WarpMode)modeInt;

    sectionSep();

    if (mode == ViewportPanel::WarpMode::CornerPin) {
        auto& corners = cornerPin.corners();
        const char* labels[] = {"BL", "BR", "TR", "TL"};
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.78f, 1.0f, 0.7f));
            ImGui::Text("%s", labels[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(28);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat2("##corner", &corners[i][0], 0.01f, -1.5f, 1.5f, "%.2f");
            ImGui::PopID();
        }
        if (accentButton("Reset", -1)) {
            corners = {{
                {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}
            }};
        }
    } else {
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
    }

    ImGui::End();
}
