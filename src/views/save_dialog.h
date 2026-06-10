// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "localization.h"
#include "models/commands.h"
#include "preferences.h"
#include "texture_converter.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace whiteout::textool::views {

/// The Save dialog's writable-format filters are built from the format registry
/// (FmtCap::Write) via dialogFiltersFor() in save_helpers.h — there is no
/// hand-maintained array here.

/// Entry mapping a localization key to a TextureKind enum value.  Use
/// kindLabel() / textureKindName() to obtain the translated display string.
struct KindEntry {
    const char* key; ///< Localization key, e.g. "kind.diffuse".
    whiteout::textures::TextureKind kind;
};

/// Kinds selectable in the top-level Kind combo (excludes deprecated ORM and internal Unused).
inline constexpr KindEntry kSelectableKinds[] = {
    // clang-format off
    {"kind.other",              whiteout::textures::TextureKind::Other},
    {"kind.diffuse",            whiteout::textures::TextureKind::Diffuse},
    {"kind.normal",             whiteout::textures::TextureKind::Normal},
    {"kind.specular",           whiteout::textures::TextureKind::Specular},
    {"kind.albedo",             whiteout::textures::TextureKind::Albedo},
    {"kind.roughness",          whiteout::textures::TextureKind::Roughness},
    {"kind.metalness",          whiteout::textures::TextureKind::Metalness},
    {"kind.ao",                 whiteout::textures::TextureKind::AmbientOcclusion},
    {"kind.gloss",              whiteout::textures::TextureKind::Gloss},
    {"kind.emissive",           whiteout::textures::TextureKind::Emissive},
    {"kind.alpha_mask",         whiteout::textures::TextureKind::AlphaMask},
    {"kind.binary_mask",        whiteout::textures::TextureKind::BinaryMask},
    {"kind.transparency_mask",  whiteout::textures::TextureKind::TransparencyMask},
    {"kind.blend_mask",         whiteout::textures::TextureKind::BlendMask},
    {"kind.lightmap",           whiteout::textures::TextureKind::Lightmap},
    {"kind.env_pbr",            whiteout::textures::TextureKind::EnvironmentPBR},
    {"kind.env_legacy",         whiteout::textures::TextureKind::EnvironmentLegacy},
    {"kind.multikind",          whiteout::textures::TextureKind::Multikind},
    // clang-format on
};
inline constexpr i32 kSelectableKindCount = static_cast<i32>(std::size(kSelectableKinds));

/// Kinds selectable per-channel inside a Multikind texture.
inline constexpr KindEntry kChannelKinds[] = {
    // clang-format off
    {"kind.unused",             whiteout::textures::TextureKind::Unused},
    {"kind.roughness",          whiteout::textures::TextureKind::Roughness},
    {"kind.metalness",          whiteout::textures::TextureKind::Metalness},
    {"kind.ao",                 whiteout::textures::TextureKind::AmbientOcclusion},
    {"kind.gloss",              whiteout::textures::TextureKind::Gloss},
    {"kind.albedo",             whiteout::textures::TextureKind::Albedo},
    {"kind.diffuse",            whiteout::textures::TextureKind::Diffuse},
    {"kind.normal",             whiteout::textures::TextureKind::Normal},
    {"kind.specular",           whiteout::textures::TextureKind::Specular},
    {"kind.emissive",           whiteout::textures::TextureKind::Emissive},
    {"kind.alpha_mask",         whiteout::textures::TextureKind::AlphaMask},
    {"kind.binary_mask",        whiteout::textures::TextureKind::BinaryMask},
    {"kind.transparency_mask",  whiteout::textures::TextureKind::TransparencyMask},
    {"kind.blend_mask",         whiteout::textures::TextureKind::BlendMask},
    {"kind.lightmap",           whiteout::textures::TextureKind::Lightmap},
    // clang-format on
};
inline constexpr i32 kChannelKindCount = static_cast<i32>(std::size(kChannelKinds));

/// Translated display label for a kind-table entry.
inline const char* kindLabel(const KindEntry& e) {
    return i18n::tr(e.key);
}

/// Look up the translated display name for any TextureKind value.
inline const char* textureKindName(whiteout::textures::TextureKind k) {
    for (const auto& e : kSelectableKinds)
        if (e.kind == k)
            return i18n::tr(e.key);
    if (k == whiteout::textures::TextureKind::Unused)
        return i18n::tr("kind.unused");
    if (k == whiteout::textures::TextureKind::ORM)
        return i18n::tr("kind.orm");
    return i18n::tr("kind.unknown");
}

/// Human-readable names for BLP encoding indices (0–6).
constexpr const char* BLP_ENCODING_NAMES[] = {
    "Infer (Auto)", "True Color (BGRA)", "Paletted (256 colors)", "JPEG", "BC1 (DXT1)",
    "BC2 (DXT3)",   "BC3 (DXT5)"};
constexpr i32 BLP_ENCODING_COUNT = static_cast<i32>(std::size(BLP_ENCODING_NAMES));

/// Human-readable names for DDS pixel-format indices (0–8).
constexpr const char* DDS_FORMAT_NAMES[] = {
    // clang-format off
    "True Color (RGBA8)", "BC1 (DXT1)",      "BC2 (DXT3)",        "BC3 (DXT5)",
    "BC4 (RGTC1)",        "BC5 (RGTC2)",     "BC6H (BPTC Float)", "BC7 (BPTC)",
    "BC3N (DXT5nm)"
    // clang-format on
};
constexpr i32 DDS_FORMAT_COUNT = static_cast<i32>(std::size(DDS_FORMAT_NAMES));
/// BC3N (DXT5nm) is stored as index 8 in the DDS format list.
constexpr i32 DDS_FORMAT_BC3N = 8;

/// Centre the next ImGui popup on the primary viewport.
inline void centerNextWindow() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

/// Map a BLP encoding combo-box index (0–6) to the corresponding BlpEncoding enum value.
inline whiteout::textures::blp::BlpEncoding toBlpEncoding(i32 index) noexcept {
    using E = whiteout::textures::blp::BlpEncoding;
    switch (index) {
    case 0:
        return E::Infer;
    case 1:
        return E::BGRA;
    case 2:
        return E::Palettized;
    case 3:
        return E::JPEG;
    default:
        return E::DXT; // indices 4 (BC1), 5 (BC2), 6 (BC3)
    }
}

/// Pixel format for a BLP DXT subtype by combo-box index (4→BC1, 5→BC2, 6→BC3).
inline whiteout::textures::PixelFormat blpDxtPixelFormat(i32 index) noexcept {
    constexpr whiteout::textures::PixelFormat kFormats[] = {
        whiteout::textures::PixelFormat::BC1,
        whiteout::textures::PixelFormat::BC2,
        whiteout::textures::PixelFormat::BC3,
    };
    return kFormats[index - 4];
}

/// Draw the "Generate Mipmaps" checkbox, mipmap mode combo, and optional
/// custom count input.
/// @p generate  Controls whether mipmap regeneration is enabled at all.
/// @p mode      Which mip-count strategy to use when generating.
/// @p customCount  User-specified count (only used when mode == Custom).
/// @p maxMips   Maximum possible mip count for the current texture
///              (pass 0 if unknown, e.g. batch mode without a loaded texture).
inline void drawMipmapModeUI(bool& generate, MipmapMode& mode, i32& customCount, i32 maxMips = 0) {
    ImGui::Checkbox(i18n::tr("save.gen_mipmaps"), &generate);
    if (!generate)
        return;
    const char* MIPMAP_MODE_NAMES[] = {i18n::tr("save.mip_mode_keep"), i18n::tr("save.mip_mode_max"),
                                       i18n::tr("save.mip_mode_custom")};
    i32 modeIdx = static_cast<i32>(mode);
    if (ImGui::Combo(i18n::tr("save.mipmap_mode"), &modeIdx, MIPMAP_MODE_NAMES,
                     static_cast<i32>(std::size(MIPMAP_MODE_NAMES)))) {
        mode = static_cast<MipmapMode>(modeIdx);
    }
    if (mode == MipmapMode::Custom) {
        const i32 lo = 1;
        const i32 hi = maxMips > 0 ? maxMips : 16;
        customCount = std::clamp(customCount, lo, hi);
        ImGui::InputInt(i18n::tr("save.mipmap_count"), &customCount);
        customCount = std::clamp(customCount, lo, hi);
        if (maxMips > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled(i18n::tr("save.mipmap_max"), maxMips);
        }
    }
}

/// Manages the save-options popup and performs the actual save operation.
class SaveDialog {
public:
    SaveDialog() = default;

    /// Rebuild the reordered filter array (last-used format first).
    /// @param is_multi_layer  When true, restricts the filter to DDS only
    ///                        (2D arrays, cube maps, and cube-map arrays).
    void buildFilterOrder(const SavePrefs& prefs, bool is_multi_layer = false);

    /// Access the reordered filter array for SDL_ShowSaveFileDialog.
    const SDL_DialogFileFilter* filterData() const {
        return active_filters_.data();
    }
    i32 filterCount() const {
        return static_cast<i32>(active_filters_.size());
    }

    /// Call after the OS save-dialog callback delivers a result.
    /// Sets up the pending save path, detects overwrites, restores options.
    void onFileChosen(const std::string& path, i32 filter_idx, SavePrefs& prefs,
                      const whiteout::textures::Texture* loaded_texture);

    /// Draw the overwrite-confirmation popup and save-options popup.
    /// Returns commands (e.g. ShowResultPopupCmd after a save attempt).
    std::vector<models::AppCommand> draw(whiteout::textures::TextureConverter& converter,
                                         const whiteout::textures::Texture* loaded_texture,
                                         SavePrefs& prefs);

private:
    /// Per-format save options edited in the options dialog.
    struct Options {
        std::string save_path;
        whiteout::textures::TextureFileFormat target_format =
            whiteout::textures::TextureFileFormat::Unknown;
        bool show_dialog = false;
        bool confirm_overwrite = false;

        // Persisted format/mipmap options (composed, not duplicated)
        SavePrefs prefs;

        // Dialog-only state (not persisted)
        i32 texture_kind = 0;

        void applyPrefs(const SavePrefs& p) {
            prefs = p;
        }
        void persistPrefs(SavePrefs& p) const {
            p = prefs;
        }
    };

    /// Execute the actual save using the current options.
    std::string performSave(whiteout::textures::TextureConverter& converter,
                            const whiteout::textures::Texture& source, SavePrefs& prefs);

    void drawBlpOptions();
    void drawDdsOptions();

    Options opts_;
    std::vector<SDL_DialogFileFilter> active_filters_; ///< Reordered (preferred-first) filters.
    std::vector<i32> filter_map_;                      ///< active_filters_ index → registry Write-slice index.
    bool is_multi_layer_ = false;
};

} // namespace whiteout::textool::views
