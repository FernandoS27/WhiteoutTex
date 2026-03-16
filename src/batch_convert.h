// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "texture_converter.h"

#include <string>
#include <thread>
#include <vector>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>

namespace whiteout::gui {

/// Batch conversion dialog.  Converts all textures in an input directory
/// to a chosen output format and writes them to an output directory.
class BatchConvert {
public:
    BatchConvert() = default;

    /// Open the batch conversion dialog.
    void open();

    /// Draw the dialog.  Returns a status message when conversion finishes
    /// (empty string otherwise).
    std::string draw(SDL_Window* window);

private:
    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist,
                                              int filter);

    void processFolderResults();
    std::string beginBatch();
    void workerFunc();
    bool saveOne(whiteout::textures::TextureConverter& converter,
                 whiteout::textures::Texture tex_copy, const std::string& out_path,
                 whiteout::textures::TextureKind kind);
    void drawBlpOptions();
    void drawDdsOptions();
    void drawProgressDialog(std::string& status);

    bool show_dialog_ = false;

    // Input
    char input_dir_buf_[PATH_BUFFER_SIZE] = {};
    bool filter_blp_ = true;
    bool filter_bmp_ = true;
    bool filter_dds_ = true;
    bool filter_jpeg_ = true;
    bool filter_png_ = true;
    bool filter_tex_ = true;
    bool filter_tga_ = true;

    // Output
    char output_dir_buf_[PATH_BUFFER_SIZE] = {};
    int output_format_ = 2; // DDS

    // BLP options
    int blp_version_ = 1;
    int blp_encoding_ = 0;
    bool blp_dither_ = false;
    float blp_dither_strength_ = 0.8f;

    // DDS options
    int dds_mode_ = 0; // 0 = general, 1 = per kind group
    int dds_format_general_ = 7;
    bool dds_invert_y_general_ = false;
    int dds_format_normal_ = 5;
    bool dds_invert_y_normal_ = false;
    int dds_format_channel_ = 4;
    int dds_format_other_ = 7;

    // JPEG quality (shared with BLP JPEG encoding)
    int jpeg_quality_ = 75;

    // Common
    bool generate_mipmaps_ = false;
    bool recursive_ = true;
    bool keep_layout_ = true;

    // Progress state
    bool converting_ = false;
    std::vector<std::string> pending_files_;
    std::string batch_input_dir_;
    std::string batch_output_dir_;
    std::atomic<int> batch_processed_{0};
    std::atomic<int> batch_success_{0};
    std::atomic<int> batch_fail_{0};
    std::atomic<bool> batch_done_{false};
    std::vector<std::thread> workers_;
    std::atomic<int> work_index_{0};

    FolderState input_folder_state_;
    FolderState output_folder_state_;
};

} // namespace whiteout::gui
