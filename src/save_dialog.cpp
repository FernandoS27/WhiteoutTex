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
    const i32 preferred = std::clamp(prefs.last_filter, 0, SAVE_FILTER_COUNT - 1);
    filter_map_[0] = preferred;
    i32 slot = 1;
    for (i32 i = 0; i < SAVE_FILTER_COUNT; ++i) {
        if (i != preferred) {
            filter_map_[slot++] = i;
        }
    }
    for (i32 i = 0; i < SAVE_FILTER_COUNT; ++i) {
        active_filters_[i] = SAVE_FILTERS[filter_map_[i]];
    }
}

// ============================================================================
// File chosen callback
// ============================================================================

void SaveDialog::onFileChosen(const std::string& path, i32 filter_idx, SavePrefs& prefs,
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
        static_cast<i32>(loaded_texture ? loaded_texture->kind() : tex::TextureKind::Other);
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
            ImGui::SliderInt("Quality", &opts_.prefs.jpeg_quality, 1, 100);
            break;

        default:
            break;
        }

        // Common options
        ImGui::SeparatorText("Common Options");
        {
            auto cur = static_cast<tex::TextureKind>(opts_.texture_kind);
            const char* preview = textureKindName(cur);
            if (ImGui::BeginCombo("Texture Kind", preview)) {
                for (i32 i = 0; i < kSelectableKindCount; ++i) {
                    bool selected = (kSelectableKinds[i].kind == cur);
                    if (ImGui::Selectable(kSelectableKinds[i].name, selected))
                        opts_.texture_kind = static_cast<i32>(kSelectableKinds[i].kind);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        {
            i32 maxMips = 0;
            if (loaded_texture)
                maxMips = static_cast<i32>(
                    tex::computeMaxMipCount(loaded_texture->width(), loaded_texture->height()));
            drawMipmapModeUI(opts_.prefs.generate_mipmaps, opts_.prefs.mipmap_mode,
                             opts_.prefs.mipmap_custom_count, maxMips);
        }

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
    drawBlpOptionsUI(opts_.prefs.blp_version, opts_.prefs.blp_encoding,
                     opts_.prefs.blp_dither, opts_.prefs.blp_dither_strength,
                     opts_.prefs.jpeg_quality);
}

void SaveDialog::drawDdsOptions() {
    ImGui::SeparatorText("DDS Options");

    const auto tk_kind = static_cast<tex::TextureKind>(opts_.texture_kind);
    const i32* allowed;
    i32 allowed_count;
    ddsPresetForKind(tk_kind, allowed, allowed_count);
    validateDdsFormat(tk_kind, opts_.prefs.dds_format);

    char preset_label[128];
    {
        i32 n = std::snprintf(preset_label, sizeof(preset_label), "Preset: %s — ",
                              textureKindName(static_cast<tex::TextureKind>(opts_.texture_kind)));
        for (i32 i = 0; i < allowed_count && n < (i32)sizeof(preset_label) - 1; ++i) {
            if (i > 0)
                n += std::snprintf(preset_label + n, sizeof(preset_label) - n, ", ");
            n += std::snprintf(preset_label + n, sizeof(preset_label) - n, "%s",
                               DDS_FORMAT_NAMES[allowed[i]]);
        }
    }
    ImGui::TextDisabled("%s", preset_label);

    if (ImGui::BeginCombo("Pixel Format", DDS_FORMAT_NAMES[opts_.prefs.dds_format])) {
        for (i32 i = 0; i < allowed_count; ++i) {
            bool selected = (opts_.prefs.dds_format == allowed[i]);
            if (ImGui::Selectable(DDS_FORMAT_NAMES[allowed[i]], selected))
                opts_.prefs.dds_format = allowed[i];
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (tk_kind == tex::TextureKind::Normal) {
        ImGui::Checkbox("Invert Y Channel", &opts_.prefs.dds_invert_y);
    }
}

// ============================================================================
// Save execution
// ============================================================================

std::string SaveDialog::performSave(TC& converter, const tex::Texture& source, SavePrefs& prefs) {
    auto tex_copy = source;
    tex_copy.setKind(static_cast<tex::TextureKind>(opts_.texture_kind));

    auto* pool = threadPoolManager().get();

    if (opts_.prefs.generate_mipmaps) {
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        const auto mipCount = effectiveMipCount(opts_.prefs.mipmap_mode,
                                                opts_.prefs.mipmap_custom_count, tex_copy);
        if (auto err = tex_copy.generateMipmaps(mipCount, pool))
            return "Mipmap generation failed: " + *err;
    }

    bool ok = false;
    switch (opts_.target_format) {
    case TFF::BLP: {
        auto blp = buildBlpSaveOptions(opts_.prefs.blp_version, opts_.prefs.blp_encoding,
                                       opts_.prefs.blp_dither, opts_.prefs.blp_dither_strength,
                                       opts_.prefs.jpeg_quality);
        coerceBlpFormat(tex_copy, opts_.prefs.blp_encoding, blp.encoding, pool);
        ok = converter.save(tex_copy, opts_.save_path, blp);
        break;
    }
    case TFF::DDS: {
        coerceDdsFormat(tex_copy, opts_.prefs.dds_format, opts_.prefs.dds_invert_y, pool);
        ok = converter.save(tex_copy, opts_.save_path);
        break;
    }
    case TFF::JPEG:
        ok = converter.save(tex_copy, opts_.save_path, opts_.prefs.jpeg_quality);
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
