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

namespace whiteout::textool::views {

using namespace models;

// ============================================================================
// Filter ordering
// ============================================================================

void SaveDialog::buildFilterOrder(const SavePrefs& prefs, bool is_multi_layer) {
    is_multi_layer_ = is_multi_layer;

    // Base list of writable formats, in registry (FmtCap::Write) order.
    const auto& base = dialogFiltersFor(tex::FmtCap::Write, /*allSupported=*/false,
                                        /*allFiles=*/false);
    active_filters_.clear();
    filter_map_.clear();

    if (is_multi_layer) {
        // Only DDS supports multi-layer textures (2D array, cube, cube array).
        const i32 dds = tex::capSliceIndex(tex::FmtCap::Write, TFF::DDS);
        active_filters_.push_back(base[dds]);
        filter_map_.push_back(dds);
        return;
    }

    const i32 count = static_cast<i32>(base.size());
    const i32 preferred = std::clamp(prefs.last_filter, 0, count - 1);
    filter_map_.push_back(preferred);
    for (i32 i = 0; i < count; ++i)
        if (i != preferred)
            filter_map_.push_back(i);
    for (i32 idx : filter_map_)
        active_filters_.push_back(base[idx]);
}

// ============================================================================
// File chosen callback
// ============================================================================

void SaveDialog::onFileChosen(const std::string& path, i32 filter_idx, SavePrefs& prefs,
                              const tex::Texture* loaded_texture) {
    // Append extension if missing
    std::string final_path = path;
    if (std::filesystem::path(final_path).extension().empty() && filter_idx >= 0 &&
        filter_idx < static_cast<i32>(active_filters_.size())) {
        std::string_view pat = active_filters_[filter_idx].pattern;
        auto sep = pat.find(';');
        final_path += '.';
        final_path += (sep == std::string_view::npos) ? pat : pat.substr(0, sep);
    }

    opts_.save_path = final_path;
    opts_.target_format = TC::classifyPath(final_path);

    // Map reordered index back to original
    if (filter_idx >= 0 && filter_idx < static_cast<i32>(filter_map_.size())) {
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

std::vector<AppCommand> SaveDialog::draw(TC& converter, const tex::Texture* loaded_texture,
                                         SavePrefs& prefs) {
    std::vector<AppCommand> commands;

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
        if (is_multi_layer_)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Multi-layer textures can only be saved as DDS.");
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
            ImGui::Checkbox("Progressive", &opts_.prefs.jpeg_progressive);
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
        const bool save_blocked = is_multi_layer_ && opts_.target_format != TFF::DDS;
        if (save_blocked)
            ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(120, 0)) && loaded_texture) {
            std::string status = performSave(converter, *loaded_texture, prefs);
            if (!status.empty()) {
                bool ok = status.starts_with("Saved:");
                commands.push_back(ShowResultPopupCmd{std::move(status), ok});
            }
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (save_blocked)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return commands;
}

// ============================================================================
// Format-specific option panels
// ============================================================================

void SaveDialog::drawBlpOptions() {
    drawBlpOptionsUI(opts_.prefs.blp_version, opts_.prefs.blp_encoding, opts_.prefs.blp_dither,
                     opts_.prefs.blp_dither_strength, opts_.prefs.jpeg_quality,
                     opts_.prefs.jpeg_progressive);
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
    if (is_multi_layer_ && opts_.target_format != TFF::DDS)
        return "Error: 2D array, cube, and cube-array textures can only be saved as DDS.";

    auto tex_copy = source;
    tex_copy.setKind(static_cast<tex::TextureKind>(opts_.texture_kind));

    auto* pool = threadPoolManager().get();

    if (opts_.prefs.generate_mipmaps) {
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        const auto mipCount =
            effectiveMipCount(opts_.prefs.mipmap_mode, opts_.prefs.mipmap_custom_count, tex_copy);
        if (auto err = tex_copy.generateMipmaps(mipCount, pool))
            return "Mipmap generation failed: " + *err;
    }

    bool ok = false;
    switch (opts_.target_format) {
    case TFF::BLP: {
        auto blp = buildBlpSaveOptions(opts_.prefs.blp_version, opts_.prefs.blp_encoding,
                                       opts_.prefs.blp_dither, opts_.prefs.blp_dither_strength,
                                       opts_.prefs.jpeg_quality, opts_.prefs.jpeg_progressive);
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
        ok = converter.save(tex_copy, opts_.save_path, opts_.prefs.jpeg_quality,
                            opts_.prefs.jpeg_progressive);
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

} // namespace whiteout::textool::views
