// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/menu_bar.h"

#include "localization.h"

#include <filesystem>

#include <imgui.h>

namespace whiteout::textool::views {

using namespace models;
using i18n::tr;

std::vector<AppCommand> drawMenuBar(bool has_texture, const std::vector<std::string>& recent_paths,
                                    bool has_upscaler, i18n::Language current_language,
                                    const std::vector<std::string>& pipelines) {
    std::vector<AppCommand> commands;

    if (!ImGui::BeginMainMenuBar())
        return commands;

    if (ImGui::BeginMenu(tr("menu.file"))) {
        if (ImGui::MenuItem(tr("menu.file.open"))) {
            commands.push_back(ShowOpenDialogCmd{});
        }
        if (ImGui::BeginMenu(tr("menu.file.open_recent"), !recent_paths.empty())) {
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
            if (ImGui::MenuItem(tr("menu.file.clear_recent"))) {
                commands.push_back(ClearRecentFilesCmd{});
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(tr("menu.file.save_as"), nullptr, false, has_texture)) {
            commands.push_back(ShowSaveDialogCmd{});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("menu.tools"))) {
        if (ImGui::MenuItem(tr("menu.tools.batch"))) {
            commands.push_back(OpenBatchConvertCmd{});
        }
        if (ImGui::MenuItem(tr("menu.tools.casc"))) {
            commands.push_back(OpenCascBrowserCmd{});
        }
        // Run a standard pipeline on the current image (output replaces it).
        if (ImGui::BeginMenu(tr("menu.tools.pipeline"), has_texture && !pipelines.empty())) {
            for (const auto& name : pipelines) {
                // Show the file stem; the command carries the full file name.
                std::string label = std::filesystem::path(name).stem().string();
                if (ImGui::MenuItem(label.c_str()))
                    commands.push_back(RunPipelineCmd{name});
            }
            ImGui::EndMenu();
        }
        if (has_upscaler) {
            ImGui::Separator();
            if (ImGui::MenuItem(tr("menu.tools.upscale"), nullptr, false, has_texture)) {
                commands.push_back(ShowUpscaleDialogCmd{});
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("menu.settings"))) {
        if (ImGui::BeginMenu(tr("menu.settings.language"))) {
            for (const auto& e : i18n::languages()) {
                const bool selected = e.lang == current_language;
                if (ImGui::MenuItem(e.endonym, nullptr, selected)) {
                    commands.push_back(SetLanguageCmd{e.lang});
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("menu.help"))) {
        if (ImGui::MenuItem(tr("menu.help.about"))) {
            commands.push_back(ShowAboutCmd{});
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();

    return commands;
}

} // namespace whiteout::textool::views
