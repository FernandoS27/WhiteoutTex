// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace whiteout::gui {

// ============================================================================
// Batch transformation pipeline
// ============================================================================

/// Type of transformation step in the batch pipeline.
enum class TransformType : i32 {
    Upscale   = 0, ///< AI upscale (Real-ESRGAN).
    Downscale = 1, ///< Halve dimensions by dropping mip levels.
};

/// A single step in the batch transformation pipeline.
struct TransformStep {
    TransformType type = TransformType::Downscale;

    // Downscale options
    i32 downscale_levels = 1; ///< 1 = x2, 2 = x4.

    // Upscale options
    i32 upscale_model_index = 0; ///< Index into the available upscaler models.
    bool upscale_alpha = false;  ///< Upscale alpha channel through the model.
};

constexpr i32 MAX_RECENT_PATHS = 10;

/// Generic recently-used path list (MRU).  Used for recently opened files,
/// CASC storage paths, batch input/output directories, etc.
struct RecentPaths {
    std::vector<std::string> paths; ///< Most-recent first, up to MAX_RECENT_PATHS.

    /// Push a path to the front, removing duplicates and trimming to the limit.
    void push(const std::string& path) {
        paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
        paths.insert(paths.begin(), path);
        if (static_cast<i32>(paths.size()) > MAX_RECENT_PATHS)
            paths.resize(MAX_RECENT_PATHS);
    }
};

/// Backward-compatible alias — RecentFiles is structurally identical to RecentPaths.
using RecentFiles = RecentPaths;

/// Saved host (SDL) window size persisted in the INI file.
struct SavedHostWindowSize {
    i32 width = 0;
    i32 height = 0;
    bool has_size = false;
};

/// ImGui main-window position/size read back from the INI file.
struct MainWindowIniRect {
    i32 pos_x = 0;
    i32 pos_y = 0;
    i32 width = 0;
    i32 height = 0;
    bool has_pos = false;
    bool has_size = false;
};

/// Mipmap generation mode.
enum class MipmapMode : i32 {
    KeepOriginal = 0, ///< Do not regenerate mipmaps.
    Maximum      = 1, ///< Generate the maximum possible mip chain.
    Custom       = 2, ///< Generate a user-specified number of mip levels.
};

/// Save preferences persisted in the [WhiteoutTex][SavePrefs] INI section.
struct SavePrefs {
    i32 last_filter = 0;
    std::string last_open_dir;
    std::string last_save_dir;
    std::string last_casc_dir;
    i32 blp_version = 1;
    i32 blp_encoding = 0;
    bool blp_dither = false;
    f32 blp_dither_strength = 0.8f;
    i32 dds_format = 0;
    bool dds_invert_y = false;
    i32 jpeg_quality = 75;
    bool generate_mipmaps = false;
    MipmapMode mipmap_mode = MipmapMode::KeepOriginal;
    i32 mipmap_custom_count = 1;
};

/// Load the [Window][##MainWindow] section from an ImGui INI file.
MainWindowIniRect load_main_window_ini_rect(const std::string& ini_path);

/// Load the [WhiteoutTex][SDLWindow] section.
SavedHostWindowSize load_saved_host_window_size(const std::string& ini_path);

/// Append the [WhiteoutTex][SDLWindow] section.
void append_saved_host_window_size(const std::string& ini_path, i32 width, i32 height);

/// Batch-convert preferences persisted in the [WhiteoutTex][BatchPrefs] INI section.
struct BatchPrefs {
    // Input filters
    bool filter_blp = true;
    bool filter_bmp = true;
    bool filter_dds = true;
    bool filter_jpeg = true;
    bool filter_png = true;
    bool filter_tex = true;
    bool filter_tga = true;
    bool recursive = true;
    bool keep_layout = true;

    // Directories
    std::string last_input_dir;
    std::string last_output_dir;

    // Output
    i32 output_format = 2; // DDS

    // BLP options
    i32 blp_version = 1;
    i32 blp_encoding = 0;
    bool blp_dither = false;
    f32 blp_dither_strength = 0.8f;

    // DDS options
    i32 dds_mode = 0;
    i32 dds_format_general = 7;
    bool dds_invert_y_general = false;
    i32 dds_format_normal = 5;
    bool dds_invert_y_normal = false;
    i32 dds_format_channel = 4;
    i32 dds_format_other = 7;

    // JPEG quality
    i32 jpeg_quality = 75;

    // Common
    bool generate_mipmaps = false;
    MipmapMode mipmap_mode = MipmapMode::KeepOriginal;
    i32 mipmap_custom_count = 1;

    // Transformation pipeline
    std::vector<TransformStep> transform_pipeline;
};

/// Load the [WhiteoutTex][SavePrefs] section.
SavePrefs load_save_prefs(const std::string& ini_path);

/// Append the [WhiteoutTex][SavePrefs] section.
void append_save_prefs(const std::string& ini_path, const SavePrefs& prefs);

/// Load the [WhiteoutTex][BatchPrefs] section.
BatchPrefs load_batch_prefs(const std::string& ini_path);

/// Append the [WhiteoutTex][BatchPrefs] section.
void append_batch_prefs(const std::string& ini_path, const BatchPrefs& prefs);

/// Load the [WhiteoutTex][RecentFiles] section.
RecentFiles load_recent_files(const std::string& ini_path);

/// Append the [WhiteoutTex][RecentFiles] section.
void append_recent_files(const std::string& ini_path, const RecentFiles& recent);

/// Load a RecentPaths list from the given INI section (e.g. "[WhiteoutTex][RecentCascPaths]").
RecentPaths load_recent_paths(const std::string& ini_path, const char* section);

/// Append a RecentPaths list under the given INI section header.
void append_recent_paths(const std::string& ini_path, const char* section, const RecentPaths& recent);

} // namespace whiteout::gui
