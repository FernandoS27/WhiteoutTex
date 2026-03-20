// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

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

/// File extension filters for the Save dialog.
constexpr SDL_DialogFileFilter SAVE_FILTERS[] = {
    {"BLP (Blizzard Picture)", "blp"},
    {"BMP (Bitmap)", "bmp"},
    {"DDS (DirectDraw Surface)", "dds"},
    {"JPEG", "jpg;jpeg"},
    {"PNG", "png"},
    {"TGA (Targa)", "tga"},
};
constexpr i32 SAVE_FILTER_COUNT = static_cast<i32>(std::size(SAVE_FILTERS));

/// Entry mapping a human-readable name to a TextureKind enum value.
struct KindEntry {
    const char* name;
    whiteout::textures::TextureKind kind;
};

/// Kinds selectable in the top-level Kind combo (excludes deprecated ORM and internal Unused).
inline constexpr KindEntry kSelectableKinds[] = {
    // clang-format off
    {"Other",              whiteout::textures::TextureKind::Other},
    {"Diffuse",            whiteout::textures::TextureKind::Diffuse},
    {"Normal",             whiteout::textures::TextureKind::Normal},
    {"Specular",           whiteout::textures::TextureKind::Specular},
    {"Albedo",             whiteout::textures::TextureKind::Albedo},
    {"Roughness",          whiteout::textures::TextureKind::Roughness},
    {"Metalness",          whiteout::textures::TextureKind::Metalness},
    {"Ambient Occlusion",  whiteout::textures::TextureKind::AmbientOcclusion},
    {"Gloss",              whiteout::textures::TextureKind::Gloss},
    {"Emissive",           whiteout::textures::TextureKind::Emissive},
    {"Alpha Mask",         whiteout::textures::TextureKind::AlphaMask},
    {"Binary Mask",        whiteout::textures::TextureKind::BinaryMask},
    {"Transparency Mask",  whiteout::textures::TextureKind::TransparencyMask},
    {"Blend Mask",         whiteout::textures::TextureKind::BlendMask},
    {"Lightmap",           whiteout::textures::TextureKind::Lightmap},
    {"Environment (PBR)",  whiteout::textures::TextureKind::EnvironmentPBR},
    {"Environment (Legacy)", whiteout::textures::TextureKind::EnvironmentLegacy},
    {"Multi-Kind",         whiteout::textures::TextureKind::Multikind},
    // clang-format on
};
inline constexpr i32 kSelectableKindCount = static_cast<i32>(std::size(kSelectableKinds));

/// Kinds selectable per-channel inside a Multikind texture.
inline constexpr KindEntry kChannelKinds[] = {
    // clang-format off
    {"Unused",             whiteout::textures::TextureKind::Unused},
    {"Roughness",          whiteout::textures::TextureKind::Roughness},
    {"Metalness",          whiteout::textures::TextureKind::Metalness},
    {"Ambient Occlusion",  whiteout::textures::TextureKind::AmbientOcclusion},
    {"Gloss",              whiteout::textures::TextureKind::Gloss},
    {"Albedo",             whiteout::textures::TextureKind::Albedo},
    {"Diffuse",            whiteout::textures::TextureKind::Diffuse},
    {"Normal",             whiteout::textures::TextureKind::Normal},
    {"Specular",           whiteout::textures::TextureKind::Specular},
    {"Emissive",           whiteout::textures::TextureKind::Emissive},
    {"Alpha Mask",         whiteout::textures::TextureKind::AlphaMask},
    {"Binary Mask",        whiteout::textures::TextureKind::BinaryMask},
    {"Transparency Mask",  whiteout::textures::TextureKind::TransparencyMask},
    {"Blend Mask",         whiteout::textures::TextureKind::BlendMask},
    {"Lightmap",           whiteout::textures::TextureKind::Lightmap},
    // clang-format on
};
inline constexpr i32 kChannelKindCount = static_cast<i32>(std::size(kChannelKinds));

/// Look up the display name for any TextureKind value.
inline const char* textureKindName(whiteout::textures::TextureKind k) {
    for (const auto& e : kSelectableKinds)
        if (e.kind == k)
            return e.name;
    if (k == whiteout::textures::TextureKind::Unused)
        return "Unused";
    if (k == whiteout::textures::TextureKind::ORM)
        return "ORM (Legacy)";
    return "Unknown";
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
    ImGui::Checkbox("Generate Mipmaps", &generate);
    if (!generate)
        return;
    constexpr const char* MIPMAP_MODE_NAMES[] = {"Keep Original", "Maximum", "Custom"};
    i32 modeIdx = static_cast<i32>(mode);
    if (ImGui::Combo("Mipmap Mode", &modeIdx, MIPMAP_MODE_NAMES,
                     static_cast<i32>(std::size(MIPMAP_MODE_NAMES)))) {
        mode = static_cast<MipmapMode>(modeIdx);
    }
    if (mode == MipmapMode::Custom) {
        const i32 lo = 1;
        const i32 hi = maxMips > 0 ? maxMips : 16;
        customCount = std::clamp(customCount, lo, hi);
        ImGui::InputInt("Mipmap Count", &customCount);
        customCount = std::clamp(customCount, lo, hi);
        if (maxMips > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(max %d)", maxMips);
        }
    }
}

/// Manages the save-options popup and performs the actual save operation.
class SaveDialog {
public:
    SaveDialog() = default;

    /// Rebuild the reordered filter array (last-used format first).
    void buildFilterOrder(const SavePrefs& prefs);

    /// Access the reordered filter array for SDL_ShowSaveFileDialog.
    const SDL_DialogFileFilter* filterData() const {
        return active_filters_.data();
    }
    i32 filterCount() const {
        return SAVE_FILTER_COUNT;
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
    std::array<SDL_DialogFileFilter, SAVE_FILTER_COUNT> active_filters_;
    std::array<i32, SAVE_FILTER_COUNT> filter_map_;
};

} // namespace whiteout::textool::views
