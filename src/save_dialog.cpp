// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "save_dialog.h"
#include "save_helpers.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include <whiteout/textures/blp/types.h>

#include <imgui.h>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

namespace whiteout::gui {

// ============================================================================
// Filter ordering
// ============================================================================

void SaveDialog::buildFilterOrder(const SavePrefs& prefs) {
    const int preferred = std::clamp(prefs.last_filter, 0, SAVE_FILTER_COUNT - 1);
    filter_map_[0] = preferred;
    int slot = 1;
    for (int i = 0; i < SAVE_FILTER_COUNT; ++i) {
        if (i != preferred) {
            filter_map_[slot++] = i;
        }
    }
    for (int i = 0; i < SAVE_FILTER_COUNT; ++i) {
        active_filters_[i] = SAVE_FILTERS[filter_map_[i]];
    }
}

// ============================================================================
// File chosen callback
// ============================================================================

void SaveDialog::onFileChosen(const std::string& path, int filter_idx, SavePrefs& prefs,
                              const tex::Texture* loaded_texture) {
    // Append extension if missing
    std::string final_path = path;
    if (std::filesystem::path(final_path).extension().empty() && filter_idx >= 0 &&
        filter_idx < SAVE_FILTER_COUNT) {
        std::string_view pat = active_filters_[filter_idx].pattern;
        auto sep = pat.find(';');
        final_path += '.';
        final_path += (sep == std::string_view::npos) ? pat : pat.substr(0, sep);
    }

    opts_.save_path = final_path;
    opts_.target_format = TC::classifyPath(final_path);

    // Map reordered index back to original
    if (filter_idx >= 0 && filter_idx < SAVE_FILTER_COUNT) {
        prefs.last_filter = filter_map_[filter_idx];
    }

    if (std::filesystem::exists(final_path)) {
        opts_.confirm_overwrite = true;
    } else {
        opts_.show_dialog = true;
    }

    // Restore last-used options
    opts_.applyPrefs(prefs);
    opts_.texture_kind =
        static_cast<int>(loaded_texture ? loaded_texture->kind() : tex::TextureKind::Other);
}

// ============================================================================
// Draw
// ============================================================================

std::string SaveDialog::draw(TC& converter, const tex::Texture* loaded_texture, SavePrefs& prefs) {
    std::string status;

    // Overwrite confirmation
    if (opts_.confirm_overwrite) {
        ImGui::OpenPopup("Overwrite?");
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("File already exists:\n  %s\n\nOverwrite?", opts_.save_path.c_str());
        ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            opts_.confirm_overwrite = false;
            opts_.show_dialog = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) {
            opts_.confirm_overwrite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Save options dialog
    if (opts_.show_dialog) {
        ImGui::OpenPopup("Save Options");
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("Save Options", &opts_.show_dialog,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saving to: %s", opts_.save_path.c_str());
        ImGui::Text("Format: %s", TC::fileFormatName(opts_.target_format));
        ImGui::Separator();

        // Format-specific options
        switch (opts_.target_format) {
        case TFF::BLP:
            drawBlpOptions();
            break;

        case TFF::DDS:
            drawDdsOptions();
            break;

        case TFF::JPEG:
            ImGui::SeparatorText("JPEG Options");
            ImGui::SliderInt("Quality", &opts_.jpeg_quality, 1, 100);
            break;

        default:
            break;
        }

        // Common options
        ImGui::SeparatorText("Common Options");
        ImGui::Combo("Texture Kind", &opts_.texture_kind, TEXTURE_KIND_NAMES, TEXTURE_KIND_COUNT);
        ImGui::Checkbox("Generate Mipmaps", &opts_.generate_mipmaps);

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0)) && loaded_texture) {
            status = performSave(converter, *loaded_texture, prefs);
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return status;
}

// ============================================================================
// Format-specific option panels
// ============================================================================

void SaveDialog::drawBlpOptions() {
    ImGui::SeparatorText("BLP Options");
    ImGui::Combo("BLP Version", &opts_.blp_version,
                 "BLP1 (Warcraft 3 Classic)\0BLP2 (WoW)\0");
    {
        const int* enc_allowed;
        int enc_count;
        blpAllowedEncodings(opts_.blp_version, enc_allowed, enc_count);
        validateBlpEncoding(opts_.blp_version, opts_.blp_encoding);

        if (ImGui::BeginCombo("Encoding", BLP_ENCODING_NAMES[opts_.blp_encoding])) {
            for (int i = 0; i < enc_count; ++i) {
                bool selected = (opts_.blp_encoding == enc_allowed[i]);
                if (ImGui::Selectable(BLP_ENCODING_NAMES[enc_allowed[i]], selected))
                    opts_.blp_encoding = enc_allowed[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    if (opts_.blp_encoding == 2) {
        ImGui::Checkbox("Dither", &opts_.blp_dither);
        if (opts_.blp_dither)
            ImGui::SliderFloat("Dither Strength", &opts_.blp_dither_strength, 0.0f, 1.0f);
    }
    if (opts_.blp_encoding == 3) {
        ImGui::SliderInt("JPEG Quality", &opts_.jpeg_quality, 1, 100);
    }
}

void SaveDialog::drawDdsOptions() {
    ImGui::SeparatorText("DDS Options");

    const auto tk_kind = static_cast<tex::TextureKind>(opts_.texture_kind);
    const int* allowed;
    int allowed_count;
    ddsPresetForKind(tk_kind, allowed, allowed_count);
    validateDdsFormat(tk_kind, opts_.dds_format);

    char preset_label[128];
    {
        int n = std::snprintf(preset_label, sizeof(preset_label), "Preset: %s — ",
                              TEXTURE_KIND_NAMES[opts_.texture_kind]);
        for (int i = 0; i < allowed_count && n < (int)sizeof(preset_label) - 1; ++i) {
            if (i > 0)
                n += std::snprintf(preset_label + n, sizeof(preset_label) - n, ", ");
            n += std::snprintf(preset_label + n, sizeof(preset_label) - n, "%s",
                               DDS_FORMAT_NAMES[allowed[i]]);
        }
    }
    ImGui::TextDisabled("%s", preset_label);

    if (ImGui::BeginCombo("Pixel Format", DDS_FORMAT_NAMES[opts_.dds_format])) {
        for (int i = 0; i < allowed_count; ++i) {
            bool selected = (opts_.dds_format == allowed[i]);
            if (ImGui::Selectable(DDS_FORMAT_NAMES[allowed[i]], selected))
                opts_.dds_format = allowed[i];
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (tk_kind == tex::TextureKind::Normal) {
        ImGui::Checkbox("Invert Y Channel", &opts_.dds_invert_y);
    }
}

// ============================================================================
// Save execution
// ============================================================================

std::string SaveDialog::performSave(TC& converter, const tex::Texture& source, SavePrefs& prefs) {
    auto tex_copy = source;
    tex_copy.setKind(static_cast<tex::TextureKind>(opts_.texture_kind));

    auto* pool = threadPoolManager().get();

    if (opts_.generate_mipmaps) {
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        if (auto err = tex_copy.generateMipmaps(pool))
            return "Mipmap generation failed: " + *err;
    }

    bool ok = false;
    switch (opts_.target_format) {
    case TFF::BLP: {
        auto blp = buildBlpSaveOptions(opts_.blp_version, opts_.blp_encoding,
                                       opts_.blp_dither, opts_.blp_dither_strength,
                                       opts_.jpeg_quality);
        coerceBlpFormat(tex_copy, opts_.blp_encoding, blp.encoding, pool);
        ok = converter.save(tex_copy, opts_.save_path, blp);
        break;
    }
    case TFF::DDS: {
        coerceDdsFormat(tex_copy, opts_.dds_format, opts_.dds_invert_y, pool);
        ok = converter.save(tex_copy, opts_.save_path);
        break;
    }
    case TFF::JPEG:
        ok = converter.save(tex_copy, opts_.save_path, opts_.jpeg_quality);
        break;
    default:
        ok = converter.save(tex_copy, opts_.save_path);
        break;
    }

    std::string status;
    if (ok) {
        status = "Saved: " + opts_.save_path;
        opts_.persistPrefs(prefs);
    } else {
        status = "Failed to save: " + opts_.save_path;
        if (converter.hasIssues())
            appendIssues(status, converter.getIssues());
    }
    return status;
}

} // namespace whiteout::gui
