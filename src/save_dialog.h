// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "preferences.h"
#include "texture_converter.h"

#include <array>
#include <string>

#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace whiteout::gui {

/// File extension filters for the Save dialog.
constexpr SDL_DialogFileFilter SAVE_FILTERS[] = {
    {"BLP (Blizzard Picture)", "blp"},
    {"BMP (Bitmap)", "bmp"},
    {"DDS (DirectDraw Surface)", "dds"},
    {"JPEG", "jpg;jpeg"},
    {"PNG", "png"},
    {"TGA (Targa)", "tga"},
};
constexpr int SAVE_FILTER_COUNT = static_cast<int>(std::size(SAVE_FILTERS));

/// Human-readable names for TextureKind enum values (shared by details panel and save dialog).
constexpr const char* TEXTURE_KIND_NAMES[] = {
    "Other",    "Diffuse",    "Normal",    "Specular",          "ORM",
    "Albedo",   "Roughness",  "Metalness", "Ambient Occlusion", "Gloss",
    "Emissive", "Alpha Mask", "Lightmap",  "Environment (PBR)", "Environment (Legacy)"};
constexpr int TEXTURE_KIND_COUNT = static_cast<int>(std::size(TEXTURE_KIND_NAMES));

/// Human-readable names for BLP encoding indices (0–6).
constexpr const char* BLP_ENCODING_NAMES[] = {
    "Infer (Auto)", "True Color (BGRA)", "Paletted (256 colors)", "JPEG",
    "BC1 (DXT1)",   "BC2 (DXT3)",        "BC3 (DXT5)"};
constexpr int BLP_ENCODING_COUNT = static_cast<int>(std::size(BLP_ENCODING_NAMES));

/// Human-readable names for DDS pixel-format indices (0–8).
constexpr const char* DDS_FORMAT_NAMES[] = {
    "True Color (RGBA8)", "BC1 (DXT1)",      "BC2 (DXT3)",        "BC3 (DXT5)",
    "BC4 (RGTC1)",        "BC5 (RGTC2)",     "BC6H (BPTC Float)", "BC7 (BPTC)",
    "BC3N (DXT5nm)"};
constexpr int DDS_FORMAT_COUNT = static_cast<int>(std::size(DDS_FORMAT_NAMES));
/// BC3N (DXT5nm) is stored as index 8 in the DDS format list.
constexpr int DDS_FORMAT_BC3N = 8;

/// Centre the next ImGui popup on the primary viewport.
inline void centerNextWindow() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

/// Map a BLP encoding combo-box index (0–6) to the corresponding BlpEncoding enum value.
inline whiteout::textures::blp::BlpEncoding toBlpEncoding(int index) noexcept {
    using E = whiteout::textures::blp::BlpEncoding;
    switch (index) {
    case 0:  return E::Infer;
    case 1:  return E::BGRA;
    case 2:  return E::Palettized;
    case 3:  return E::JPEG;
    default: return E::DXT; // indices 4 (BC1), 5 (BC2), 6 (BC3)
    }
}

/// Pixel format for a BLP DXT subtype by combo-box index (4→BC1, 5→BC2, 6→BC3).
inline whiteout::textures::PixelFormat blpDxtPixelFormat(int index) noexcept {
    constexpr whiteout::textures::PixelFormat kFormats[] = {
        whiteout::textures::PixelFormat::BC1,
        whiteout::textures::PixelFormat::BC2,
        whiteout::textures::PixelFormat::BC3,
    };
    return kFormats[index - 4];
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
    int filterCount() const {
        return SAVE_FILTER_COUNT;
    }

    /// Call after the OS save-dialog callback delivers a result.
    /// Sets up the pending save path, detects overwrites, restores options.
    void onFileChosen(const std::string& path, int filter_idx, SavePrefs& prefs,
                      const whiteout::textures::Texture* loaded_texture);

    /// Draw the overwrite-confirmation popup and save-options popup.
    /// Returns a status message after a save attempt (empty if nothing happened).
    std::string draw(whiteout::textures::TextureConverter& converter,
                     const whiteout::textures::Texture* loaded_texture, SavePrefs& prefs);

private:
    /// Per-format save options edited in the options dialog.
    struct Options {
        std::string save_path;
        whiteout::textures::TextureFileFormat target_format =
            whiteout::textures::TextureFileFormat::Unknown;
        bool show_dialog = false;
        bool confirm_overwrite = false;

        // BLP
        int blp_version = 1;
        int blp_encoding = 0;
        bool blp_dither = false;
        float blp_dither_strength = 0.8f;

        // DDS
        int dds_format = 0;
        bool dds_invert_y = false;

        // JPEG quality
        int jpeg_quality = 75;

        // Common
        bool generate_mipmaps = false;
        int texture_kind = 0;

        void applyPrefs(const SavePrefs& p) {
            blp_version = p.blp_version;
            blp_encoding = p.blp_encoding;
            blp_dither = p.blp_dither;
            blp_dither_strength = p.blp_dither_strength;
            dds_format = p.dds_format;
            jpeg_quality = p.jpeg_quality;
            generate_mipmaps = p.generate_mipmaps;
        }
        void persistPrefs(SavePrefs& p) const {
            p.blp_version = blp_version;
            p.blp_encoding = blp_encoding;
            p.blp_dither = blp_dither;
            p.blp_dither_strength = blp_dither_strength;
            p.dds_format = dds_format;
            p.dds_invert_y = dds_invert_y;
            p.jpeg_quality = jpeg_quality;
            p.generate_mipmaps = generate_mipmaps;
        }
    };

    /// Execute the actual save using the current options.
    std::string performSave(whiteout::textures::TextureConverter& converter,
                            const whiteout::textures::Texture& source, SavePrefs& prefs);

    void drawBlpOptions();
    void drawDdsOptions();

    Options opts_;
    std::array<SDL_DialogFileFilter, SAVE_FILTER_COUNT> active_filters_;
    std::array<int, SAVE_FILTER_COUNT> filter_map_;
};

} // namespace whiteout::gui
