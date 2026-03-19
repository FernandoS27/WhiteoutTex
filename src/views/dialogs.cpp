// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/dialogs.h"
#include "save_dialog.h" // centerNextWindow()

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

namespace whiteout::gui {

// ============================================================================
// About dialog
// ============================================================================

void drawAboutDialog(bool& show) {
    if (show) {
        ImGui::OpenPopup("About WhiteoutTex");
        show = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("About WhiteoutTex", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SeparatorText("WhiteoutTex");
        ImGui::TextUnformatted("A texture viewer and converter for game assets.");
        ImGui::Spacing();
        ImGui::SeparatorText("License");
        ImGui::TextUnformatted(kLicenseText);
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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
            ImGui::TextColored(kSuccessColor, "Success");
        } else {
            ImGui::TextColored(kErrorColor, "Error");
        }
        ImGui::Separator();
        ImGui::TextUnformatted(ui.result_popup_message.c_str());
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button("OK", ImVec2(120, 0))) {
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
        ImGui::OpenPopup("BC3N Normal Map");
        show = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("BC3N Normal Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This DDS uses BC3 compression and is identified as a Normal map.");
        ImGui::TextUnformatted("Treat it as BC3N (X stored in alpha, Y stored in green)?");
        ImGui::Spacing();
        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            commands.push_back(ApplyBC3NSwapCmd{});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) {
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
        ImGui::OpenPopup("Diablo IV Payload Required");
        ui.show_d4_payload_dialog = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("Diablo IV Payload Required", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This is a Diablo IV TEX file. Pixel data lives in a separate");
        ImGui::TextUnformatted("payload file that could not be found automatically.");
        ImGui::Spacing();
        ImGui::TextDisabled("Meta: %s", ui.pending_d4_meta_path.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Payload file path:");
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_payload", ui.d4_payload_path_buf, sizeof(ui.d4_payload_path_buf));
        ImGui::Spacing();
        ImGui::TextUnformatted("Low-res payload file path (Optional):");
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_paylow", ui.d4_paylow_path_buf, sizeof(ui.d4_paylow_path_buf));
        ImGui::Spacing();

        auto closeDialog = [&] {
            ui.pending_d4_meta_path.clear();
            ui.d4_payload_path_buf[0] = '\0';
            ui.d4_paylow_path_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::Button("Load", ImVec2(120, 0))) {
            const std::string payload_path(ui.d4_payload_path_buf);
            const std::string paylow_path(ui.d4_paylow_path_buf);
            if (!payload_path.empty() && std::filesystem::exists(payload_path)) {
                commands.push_back(LoadD4PayloadCmd{
                    ui.pending_d4_meta_path,
                    payload_path,
                    (!paylow_path.empty() && std::filesystem::exists(paylow_path))
                        ? paylow_path
                        : std::string{}});
            } else {
                commands.push_back(ShowResultPopupCmd{
                    "Payload file not found: " + payload_path, false});
            }
            closeDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            closeDialog();
        }
        ImGui::EndPopup();
    }

    return commands;
}

} // namespace whiteout::gui
