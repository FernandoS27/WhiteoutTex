// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/menu_bar.h"

#include <filesystem>

#include <imgui.h>

namespace whiteout::gui {

std::vector<AppCommand> drawMenuBar(bool has_texture,
                                    const std::vector<std::string>& recent_paths,
                                    bool has_upscaler) {
    std::vector<AppCommand> commands;

    if (!ImGui::BeginMainMenuBar())
        return commands;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open")) {
            commands.push_back(ShowOpenDialogCmd{});
        }
        if (ImGui::BeginMenu("Open Recent", !recent_paths.empty())) {
            for (const auto& recent_path : recent_paths) {
                namespace fs = std::filesystem;
                std::string label = fs::path(recent_path).filename().string();
                if (ImGui::MenuItem(label.c_str())) {
                    commands.push_back(OpenFileCmd{recent_path});
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", recent_path.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent")) {
                commands.push_back(ClearRecentFilesCmd{});
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save As...", nullptr, false, has_texture)) {
            commands.push_back(ShowSaveDialogCmd{});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Batch convert...")) {
            commands.push_back(OpenBatchConvertCmd{});
        }
        if (ImGui::MenuItem("CASC Browser...")) {
            commands.push_back(OpenCascBrowserCmd{});
        }
        if (has_upscaler) {
            ImGui::Separator();
            if (ImGui::MenuItem("Upscale (AI)...", nullptr, false, has_texture)) {
                commands.push_back(ShowUpscaleDialogCmd{});
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About WhiteoutTex")) {
            commands.push_back(ShowAboutCmd{});
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();

    return commands;
}

} // namespace whiteout::gui
