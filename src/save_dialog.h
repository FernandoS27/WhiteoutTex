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
    "Other",     "Diffuse",           "Normal", "Specular", "ORM", "Albedo", "Roughness",
    "Metalness", "Ambient Occlusion", "Gloss",  "Emissive"};
constexpr int TEXTURE_KIND_COUNT = static_cast<int>(std::size(TEXTURE_KIND_NAMES));

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

        // JPEG quality
        int jpeg_quality = 75;

        // Common
        bool generate_mipmaps = false;
        int texture_kind = 0;
    };

    /// Execute the actual save using the current options.
    std::string performSave(whiteout::textures::TextureConverter& converter,
                            const whiteout::textures::Texture& source, SavePrefs& prefs);

    Options opts_;
    std::array<SDL_DialogFileFilter, SAVE_FILTER_COUNT> active_filters_;
    std::array<int, SAVE_FILTER_COUNT> filter_map_;
};

} // namespace whiteout::gui
