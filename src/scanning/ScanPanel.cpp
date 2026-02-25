#ifdef HAS_OPENCV
#include "scanning/ScanPanel.h"
#include <imgui.h>
#include <string>

void ScanPanel::render(SceneScanner& scanner, WebcamSource& webcam) {
    ImGui::Begin("Scene Scanner");

    // --- Webcam section ---
    if (ImGui::CollapsingHeader("Webcam", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!webcam.isOpen()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::TextWrapped("Connect a USB webcam to enable structured light scanning.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));

            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Camera", &m_selectedCamera);
            if (m_selectedCamera < 0) m_selectedCamera = 0;

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("Open Webcam")) {
                webcam.open(m_selectedCamera);
            }
            ImGui::PopStyleColor(4);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            ImGui::Text("Connected");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("(%dx%d)", webcam.cameraWidth(), webcam.cameraHeight());
            ImGui::PopStyleColor();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            if (ImGui::SmallButton("Close")) {
                webcam.close();
            }
            ImGui::PopStyleColor(4);

            // Webcam preview
            if (webcam.textureId()) {
                float previewW = ImGui::GetContentRegionAvail().x;
                float aspect = (float)webcam.cameraHeight() / (float)webcam.cameraWidth();
                float previewH = previewW * aspect;
                ImGui::Image((ImTextureID)(intptr_t)webcam.textureId(),
                    ImVec2(previewW, previewH), ImVec2(0, 1), ImVec2(1, 0));
            }
        }
    }

    // Divider
    ImGui::Dummy(ImVec2(0, 4));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
    }
    ImGui::Dummy(ImVec2(0, 6));

    // --- Calibration section ---
    if (ImGui::CollapsingHeader("Calibration", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (scanner.isCalibrated()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.22f, 0.82f, 0.52f, 1.0f));
            ImGui::Text("Calibrated");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
            ImGui::Text("(err: %.3f px)", scanner.calibrationError());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.55f, 0.20f, 1.0f));
            ImGui::Text("Not calibrated");
            ImGui::PopStyleColor();
        }

        bool canCalibrate = webcam.isOpen() && scanner.state() == SceneScanner::State::Idle;
        if (!canCalibrate) ImGui::BeginDisabled();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Button("Calibrate", ImVec2(-1, 0))) {
            scanner.startCalibration(webcam);
        }
        ImGui::PopStyleColor(4);

        if (!canCalibrate) ImGui::EndDisabled();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 0.8f));
        ImGui::TextWrapped("Place a 10x7 printed checkerboard visible to both projector and camera.");
        ImGui::PopStyleColor();
    }

    // Divider
    ImGui::Dummy(ImVec2(0, 4));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(0, 200, 255, 40));
    }
    ImGui::Dummy(ImVec2(0, 6));

    // --- Scan section ---
    if (ImGui::CollapsingHeader("Scan", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto state = scanner.state();

        // Status display
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
        ImGui::Text("Status: %s", scanner.statusText().c_str());
        ImGui::PopStyleColor();

        // Progress bar during scan/decode
        if (state == SceneScanner::State::Scanning ||
            state == SceneScanner::State::Calibrating ||
            state == SceneScanner::State::Decoding) {

            float prog = scanner.progress();
            ImGui::ProgressBar(prog, ImVec2(-1, 0));

            if (state != SceneScanner::State::Decoding) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.25f, 0.25f, 0.60f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                    scanner.cancelScan();
                }
                ImGui::PopStyleColor(4);
            }
        } else {
            bool canScan = webcam.isOpen() && state == SceneScanner::State::Idle;
            if (!canScan && state != SceneScanner::State::Complete) ImGui::BeginDisabled();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.78f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.78f, 1.0f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.78f, 1.0f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 1.0f));
            if (ImGui::Button("Scan Scene", ImVec2(-1, 0))) {
                scanner.startScan(webcam);
            }
            ImGui::PopStyleColor(4);

            if (!canScan && state != SceneScanner::State::Complete) ImGui::EndDisabled();
        }

        // Results thumbnails
        if (scanner.isComplete() && scanner.result().valid) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
            ImGui::Text("Scan Results");
            ImGui::PopStyleColor();

            float thumbW = (ImGui::GetContentRegionAvail().x - 12) / 4.0f;
            float thumbH = thumbW * 0.5625f; // 16:9

            const auto& res = scanner.result();

            if (res.depthMap.id()) {
                ImGui::BeginGroup();
                ImGui::Image((ImTextureID)(intptr_t)res.depthMap.id(),
                    ImVec2(thumbW, thumbH), ImVec2(0, 1), ImVec2(1, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Depth");
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                ImGui::SameLine();
            }

            if (res.normalMap.id()) {
                ImGui::BeginGroup();
                ImGui::Image((ImTextureID)(intptr_t)res.normalMap.id(),
                    ImVec2(thumbW, thumbH), ImVec2(0, 1), ImVec2(1, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Normals");
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                ImGui::SameLine();
            }

            if (res.edgeMap.id()) {
                ImGui::BeginGroup();
                ImGui::Image((ImTextureID)(intptr_t)res.edgeMap.id(),
                    ImVec2(thumbW, thumbH), ImVec2(0, 1), ImVec2(1, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Edges");
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                ImGui::SameLine();
            }

            if (res.colorMap.id()) {
                ImGui::BeginGroup();
                ImGui::Image((ImTextureID)(intptr_t)res.colorMap.id(),
                    ImVec2(thumbW, thumbH), ImVec2(0, 1), ImVec2(1, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
                ImGui::Text("Color");
                ImGui::PopStyleColor();
                ImGui::EndGroup();
            }
        }
    }

    ImGui::End();
}

#endif // HAS_OPENCV
