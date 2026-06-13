// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "models/commands.h"
#include "preferences.h"
#include "save_dialog.h"
#include "texture_converter.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <string>
#include <unordered_map>
#include <vector>

#include <whiteout/textures/texture.h>

namespace whiteout::textool::views {

/// Draws the image details panel and mip list.
/// Returns commands for the coordinator to dispatch (no business logic here).
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
    /// @return Commands describing requested actions.
    std::vector<models::AppCommand> drawDetailsPanel(
        whiteout::textures::Texture* texture, const std::string& path,
        whiteout::textures::TextureFileFormat file_format,
        whiteout::textures::PixelFormat source_fmt, f32 width, f32 height);

    /// Draw the mip list (selectable mip levels).
    /// @param texture      The currently loaded texture.
    /// @param selected_mip Currently selected mip index (from ImageViewer).
    /// @param width        Panel width.
    /// @param height       Panel height.
    /// @return The newly selected mip index, or -1 if unchanged.
    std::vector<models::AppCommand> drawMipList(const whiteout::textures::Texture& texture,
                                                i32 selected_mip, f32 width, f32 height);

    /// Set the runnable standard pipelines shown by the Pipeline section's
    /// category-grouped list, along with each pipeline's settable Real/Integer
    /// parameters (keyed by file) shown inline when a pipeline is selected.
    void setPipelines(std::vector<models::PipelineInfo> pipelines,
                      std::unordered_map<std::string, std::vector<models::PipelineParam>> params);

    /// Set whether image-edit undo / redo steps are available (drawn as the
    /// History buttons; emits UndoImageCmd / RedoImageCmd).
    void setHistory(bool can_undo, bool can_redo) {
        can_undo_ = can_undo;
        can_redo_ = can_redo;
    }

#ifdef WHITEOUT_HAS_UPSCALER
    /// Set the list of available upscaler models (call when models change).
    void setUpscalerModels(std::vector<UpscalerModel> models);
    /// Set whether an upscale operation is currently in progress.
    void setUpscaleInProgress(bool in_progress);
#endif

private:
    // Pipeline runner (category-grouped list + inline parameters)
    std::vector<models::PipelineInfo> pipelines_;
    std::unordered_map<std::string, std::vector<models::PipelineParam>> pipeline_params_;
    std::string selected_pipeline_file_;  ///< File of the selected pipeline ("" = none).
    std::vector<double> param_values_;    ///< Working values for the selection's params.
    /// Select a pipeline by file and reset its parameter working values to defaults.
    void selectPipeline(const std::string& file);
    /// Draw the category-grouped pipeline runner (list + inline params + Run),
    /// appending any RunPipelineCmd to @p commands.  Shown first in the panel.
    void drawPipelineSection(std::vector<models::AppCommand>& commands);
    // Mipmap regeneration options
    bool generate_mips_ = true;
    MipmapMode mipmap_mode_ = MipmapMode::KeepOriginal;
    i32 mipmap_custom_count_ = 1;

    // Downscale options
    i32 downscale_level_ = 0; // 0 = x2 (1 level), 1 = x4 (2 levels)

    // Image edit history availability (set by the app each frame)
    bool can_undo_ = false;
    bool can_redo_ = false;

#ifdef WHITEOUT_HAS_UPSCALER
    std::vector<UpscalerModel> upscale_models_;
    i32 upscale_model_index_ = 0;
    bool upscale_in_progress_ = false;
    bool upscale_alpha_ = false;
#endif
};

} // namespace whiteout::textool::views
