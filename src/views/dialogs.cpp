// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "localization.h"
#include "save_dialog.h" // centerNextWindow()
#include "views/dialogs.h"

#include <filesystem>

#include <imgui.h>

namespace {

constexpr ImVec4 kSuccessColor{0.4f, 1.0f, 0.4f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};

constexpr const char* kLicenseText =
    "BSD 3-Clause License\n"
    "\n"
    "Copyright (c) 2026, Fernando Sahmkow\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are met:\n"
    "\n"
    "1. Redistributions of source code must retain the above copyright notice,\n"
    "   this list of conditions and the following disclaimer.\n"
    "\n"
    "2. Redistributions in binary form must reproduce the above copyright notice,\n"
    "   this list of conditions and the following disclaimer in the documentation\n"
    "   and/or other materials provided with the distribution.\n"
    "\n"
    "3. Neither the name of the copyright holder nor the names of its contributors\n"
    "   may be used to endorse or promote products derived from this software\n"
    "   without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
    "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
    "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n"
    "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
    "FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
    "DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
    "SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
    "CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,\n"
    "OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.";

} // namespace

namespace whiteout::textool::views {

using namespace models;
using i18n::tr;

// ============================================================================
// About dialog
// ============================================================================

void drawAboutDialog(bool& show) {
    if (show) {
        ImGui::OpenPopup(tr("dialog.about.title"));
        show = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal(tr("dialog.about.title"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SeparatorText("WhiteoutTex");
        ImGui::TextUnformatted(tr("dialog.about.description"));
        ImGui::Spacing();
        ImGui::SeparatorText(tr("dialog.about.license_header"));
        ImGui::TextUnformatted(kLicenseText);
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button(tr("dialog.about.close"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// First-run language picker
// ============================================================================

std::vector<AppCommand> drawLanguagePrompt(bool& show, i18n::Language current) {
    std::vector<AppCommand> commands;
    if (show)
        ImGui::OpenPopup("##LanguagePrompt");
    centerNextWindow();
    // No p_open and no close button: the user must pick a language to dismiss it.
    if (ImGui::BeginPopupModal("##LanguagePrompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("langprompt.title"));
        ImGui::Separator();
        for (const auto& e : i18n::languages()) {
            const bool selected = e.lang == current;
            if (ImGui::Selectable(e.endonym, selected)) {
                commands.push_back(SetLanguageCmd{e.lang});
                show = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("langprompt.help"));
        ImGui::EndPopup();
    }
    return commands;
}

// ============================================================================
// Result popup
// ============================================================================

void drawResultDialog(UIFlags& ui) {
    if (ui.show_result_popup) {
        ImGui::OpenPopup("##ResultDialog");
        ui.show_result_popup = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("##ResultDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool success = ui.result_popup_success;
        if (success) {
            ImGui::TextColored(kSuccessColor, "%s", tr("dialog.result.success"));
        } else {
            ImGui::TextColored(kErrorColor, "%s", tr("dialog.result.error"));
        }
        ImGui::Separator();
        ImGui::TextUnformatted(ui.result_popup_message.c_str());
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button(tr("dialog.result.ok"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// BC3N dialog
// ============================================================================

std::vector<AppCommand> drawBC3NDialog(bool& show) {
    std::vector<AppCommand> commands;

    if (show) {
        ImGui::OpenPopup(tr("dialog.bc3n.title"));
        show = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal(tr("dialog.bc3n.title"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("dialog.bc3n.line1"));
        ImGui::TextUnformatted(tr("dialog.bc3n.line2"));
        ImGui::Spacing();
        if (ImGui::Button(tr("dialog.bc3n.yes"), ImVec2(120, 0))) {
            commands.push_back(ApplyBC3NSwapCmd{});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("dialog.bc3n.no"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return commands;
}

// ============================================================================
// D4 payload dialog
// ============================================================================

std::vector<AppCommand> drawD4PayloadDialog(UIFlags& ui) {
    std::vector<AppCommand> commands;

    if (ui.show_d4_payload_dialog) {
        ImGui::OpenPopup(tr("dialog.d4.title"));
        ui.show_d4_payload_dialog = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal(tr("dialog.d4.title"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("dialog.d4.line1"));
        ImGui::TextUnformatted(tr("dialog.d4.line2"));
        ImGui::Spacing();
        ImGui::TextDisabled(tr("dialog.d4.meta"), ui.pending_d4_meta_path.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("dialog.d4.payload_label"));
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_payload", ui.d4_payload_path_buf, sizeof(ui.d4_payload_path_buf));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("dialog.d4.paylow_label"));
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_paylow", ui.d4_paylow_path_buf, sizeof(ui.d4_paylow_path_buf));
        ImGui::Spacing();

        auto closeDialog = [&] {
            ui.pending_d4_meta_path.clear();
            ui.d4_payload_path_buf[0] = '\0';
            ui.d4_paylow_path_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::Button(tr("dialog.d4.load"), ImVec2(120, 0))) {
            const std::string payload_path(ui.d4_payload_path_buf);
            const std::string paylow_path(ui.d4_paylow_path_buf);
            if (!payload_path.empty() && std::filesystem::exists(payload_path)) {
                commands.push_back(
                    LoadD4PayloadCmd{ui.pending_d4_meta_path, payload_path,
                                     (!paylow_path.empty() && std::filesystem::exists(paylow_path))
                                         ? paylow_path
                                         : std::string{}});
            } else {
                commands.push_back(
                    ShowResultPopupCmd{tr("dialog.d4.not_found") + payload_path, false});
            }
            closeDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("dialog.d4.cancel"), ImVec2(120, 0))) {
            closeDialog();
        }
        ImGui::EndPopup();
    }

    return commands;
}

// ============================================================================
// Upscale dialog
// ============================================================================

#ifdef WHITEOUT_HAS_UPSCALER

std::vector<AppCommand> drawUpscaleDialog(bool& show,
                                          const std::vector<UpscalerModel>& upscaler_models,
                                          i32& selected_index, bool has_gpu, bool is_running,
                                          const std::string& status,
                                          const std::filesystem::path& model_dir, i32 tex_width,
                                          i32 tex_height) {

    std::vector<AppCommand> commands;

    if (show) {
        ImGui::OpenPopup(tr("dialog.upscale.title"));
        show = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal(tr("dialog.upscale.title"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!has_gpu) {
            ImGui::TextColored(kErrorColor, "%s", tr("dialog.upscale.no_gpu"));
            ImGui::Spacing();
            if (ImGui::Button(tr("dialog.upscale.close"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return commands;
        }

        if (upscaler_models.empty()) {
            ImGui::TextColored(kErrorColor, "%s", tr("dialog.upscale.no_models"));
            ImGui::TextUnformatted(tr("dialog.upscale.download_with"));
#if defined(_WIN32)
            ImGui::TextDisabled("  .\\scripts\\download_models.ps1");
#else
            ImGui::TextDisabled("  pwsh ./scripts/download_models.ps1");
#endif
            ImGui::TextUnformatted(tr("dialog.upscale.models_dir"));
            ImGui::TextDisabled("  %s", model_dir.string().c_str());
            ImGui::Spacing();
            if (ImGui::Button(tr("dialog.upscale.close"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return commands;
        }

        ImGui::TextUnformatted(tr("dialog.upscale.intro"));
        ImGui::Spacing();

        // Model selector
        if (ImGui::BeginCombo(tr("dialog.upscale.model"),
                              upscaler_models[selected_index].display_name.c_str())) {
            for (i32 i = 0; i < static_cast<i32>(upscaler_models.size()); ++i) {
                bool selected = (i == selected_index);
                std::string label = upscaler_models[i].label();
                if (ImGui::Selectable(label.c_str(), selected)) {
                    selected_index = i;
                }
            }
            ImGui::EndCombo();
        }

        const auto& model = upscaler_models[selected_index];
        if (tex_width > 0 && tex_height > 0) {
            i32 outw = tex_width * model.scale;
            i32 outh = tex_height * model.scale;
            ImGui::Text(tr("dialog.upscale.output"), outw, outh, model.scale);
        }

        ImGui::Spacing();

        if (!status.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::Spacing();
        }

        if (is_running)
            ImGui::BeginDisabled();
        if (ImGui::Button(tr("dialog.upscale.upscale"), ImVec2(120, 0))) {
            if (tex_width > 0 && tex_height > 0) {
                commands.push_back(StartUpscaleCmd{selected_index, false});
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("dialog.upscale.close"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        if (is_running)
            ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    return commands;
}

#endif // WHITEOUT_HAS_UPSCALER

} // namespace whiteout::textool::views
