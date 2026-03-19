// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file app_state.h
/// @brief Pure data structs that describe application state.
///        No SDL or ImGui dependencies — only standard library and WhiteoutLib types.

#include "common_types.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <whiteout/textures/texture.h>

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

namespace whiteout::gui {

// ============================================================================
// Shared OS-dialog state
// ============================================================================

/// Thread-safe state for file-picker OS dialog callbacks (open / save).
struct FileDialogState {
    std::mutex mtx;
    std::string pending_path;
    i32 pending_filter = -1;
    std::atomic<bool> has_pending{false};
};

// ============================================================================
// Loaded-texture state
// ============================================================================

/// All state related to the currently loaded texture.
struct TextureState {
    std::optional<whiteout::textures::Texture> texture;
    std::string path;
    std::string status_message;
    whiteout::textures::TextureFileFormat file_format =
        whiteout::textures::TextureFileFormat::Unknown;
    whiteout::textures::PixelFormat source_fmt =
        whiteout::textures::PixelFormat::RGBA8;
};

// ============================================================================
// UI flags & transient dialog state
// ============================================================================

/// Flags and small pieces of state that drive popup dialogs.
struct UIFlags {
    bool show_about = false;

    // Generic result popup
    bool show_result_popup = false;
    bool result_popup_success = false;
    std::string result_popup_message;

    // BC3N normal-map dialog
    bool show_bc3n_dialog = false;

    // Diablo IV TEX payload dialog
    bool show_d4_payload_dialog = false;
    std::string pending_d4_meta_path;
    char d4_payload_path_buf[PATH_BUFFER_SIZE] = {};
    char d4_paylow_path_buf[PATH_BUFFER_SIZE] = {};
};

// ============================================================================
// Upscaler state (optional feature)
// ============================================================================

#ifdef WHITEOUT_HAS_UPSCALER
/// All state related to the AI upscaler.
struct UpscaleState {
    Upscaler upscaler;
    bool show_dialog = false;
    bool in_progress = false;
    i32 model_index = 0;
    i32 gpu_id = 0;
    std::vector<UpscalerModel> models;
    std::string status;
    mutable std::mutex status_mtx; ///< Guards status across threads.
    std::atomic<bool> done{false};
    bool success = false;
    std::optional<textures::Texture> result;
    std::thread thread;
};
#endif

} // namespace whiteout::gui
