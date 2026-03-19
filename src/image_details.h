// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "preferences.h"
#include "save_dialog.h"
#include "texture_converter.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <optional>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>

namespace whiteout::gui {

/// Result returned by ImageDetails::draw() when the texture was modified.
struct ImageDetailsResult {
    /// If set, the loaded texture was replaced (e.g. mipmaps regenerated).
    std::optional<whiteout::textures::Texture> updated_texture;
    /// If set, the viewer display needs refreshing (e.g. kind changed).
    bool refresh_display = false;
    /// Result message to show in a popup (empty = no popup).
    std::string result_message;
    /// Whether the result message indicates success.
    bool result_success = false;
#ifdef WHITEOUT_HAS_UPSCALER
    /// If >= 0, the user clicked Upscale with this model index.
    i32 upscale_model_index = -1;
    /// Whether to upscale alpha through the model.
    bool upscale_alpha = false;
#endif
};

/// Draws the image details panel and mip list, and handles mipmap regeneration.
class ImageDetails {
public:
    ImageDetails() = default;

    /// Draw the details panel.
    /// @param texture        The currently loaded texture (or nullptr if none).
    /// @param path           File path of the loaded texture.
    /// @param file_format    File format of the loaded texture.
    /// @param source_fmt     Original pixel format before any conversion.
    /// @param width          Panel width.
    /// @param height         Panel height.
    /// @return Result describing any texture modifications or messages.
    ImageDetailsResult drawDetailsPanel(
        whiteout::textures::Texture* texture,
        const std::string& path,
        whiteout::textures::TextureFileFormat file_format,
        whiteout::textures::PixelFormat source_fmt,
        f32 width, f32 height);

    /// Draw the mip list (selectable mip levels).
    /// @param texture      The currently loaded texture.
    /// @param selected_mip Currently selected mip index (from ImageViewer).
    /// @param width        Panel width.
    /// @param height       Panel height.
    /// @return The newly selected mip index, or -1 if unchanged.
    i32 drawMipList(const whiteout::textures::Texture& texture,
                    i32 selected_mip, f32 width, f32 height);

#ifdef WHITEOUT_HAS_UPSCALER
    /// Set the list of available upscaler models (call when models change).
    void setUpscalerModels(std::vector<UpscalerModel> models);
    /// Set whether an upscale operation is currently in progress.
    void setUpscaleInProgress(bool in_progress);
#endif

private:
    // Mipmap regeneration options
    bool generate_mips_ = true;
    MipmapMode mipmap_mode_ = MipmapMode::KeepOriginal;
    i32 mipmap_custom_count_ = 1;

    // Downscale options
    i32 downscale_level_ = 0; // 0 = x2 (1 level), 1 = x4 (2 levels)

#ifdef WHITEOUT_HAS_UPSCALER
    std::vector<UpscalerModel> upscale_models_;
    i32 upscale_model_index_ = 0;
    bool upscale_in_progress_ = false;
    bool upscale_alpha_ = false;
#endif
};

} // namespace whiteout::gui
