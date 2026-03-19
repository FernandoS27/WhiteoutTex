// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "preferences.h"
#include "services/batch_service.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace whiteout::gui {

/// Batch conversion dialog.  Converts all textures in an input directory
/// to a chosen output format and writes them to an output directory.
class BatchConvert {
public:
    BatchConvert() = default;

    /// Open the batch conversion dialog.
    void open(const BatchPrefs& prefs);

    /// Draw the dialog.  Returns a status message when conversion finishes
    /// (empty string otherwise).
    std::string draw(SDL_Window* window, BatchPrefs& prefs,
                     RecentPaths& recent_input_dirs, RecentPaths& recent_output_dirs);

#ifdef WHITEOUT_HAS_UPSCALER
    /// Set the list of available upscaler models for the transformation pipeline.
    void setUpscalerModels(std::vector<UpscalerModel> models);
#endif

private:
    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist,
                                              i32 filter);

    void processFolderResults();
    std::string beginBatch();
    void drawBlpOptions();
    void drawDdsOptions();
    void drawTransformPipeline();
    void drawProgressDialog(std::string& status);

    void applyPrefs(const BatchPrefs& prefs);
    void persistPrefs(BatchPrefs& prefs) const;

    bool show_dialog_ = false;

    // Persisted preferences (format options, filters, pipeline, etc.)
    BatchPrefs prefs_;

    // ImGui text-input buffers (char arrays required by ImGui API)
    char input_dir_buf_[PATH_BUFFER_SIZE] = {};
    char output_dir_buf_[PATH_BUFFER_SIZE] = {};

#ifdef WHITEOUT_HAS_UPSCALER
    std::vector<UpscalerModel> upscale_models_;
#endif

    // Batch execution service
    BatchService batch_service_;

    FolderState input_folder_state_;
    FolderState output_folder_state_;
};

} // namespace whiteout::gui
