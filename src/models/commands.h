// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file commands.h
/// @brief Typed command structs that views emit and the coordinator (App) dispatches.
///        Each struct represents a single user intent. Combined into AppCommand variant.

#include "common_types.h"

#include <string>
#include <variant>
#include <vector>

namespace whiteout::gui {

// ============================================================================
// Commands emitted by views
// ============================================================================

/// Request to open and load a texture from a file path.
struct OpenFileCmd {
    std::string path;
};

/// Request to refresh the viewer display (e.g. after texture kind change).
struct RefreshDisplayCmd {};

/// Request to regenerate mipmaps on the current texture.
struct RegenerateMipmapsCmd {
    u32 mip_count;
};

/// Request to downscale the current texture by the given number of levels.
struct DownscaleCmd {
    u32 levels;
};

/// Request to start an async AI upscale operation.
struct StartUpscaleCmd {
    i32 model_index;
    bool upscale_alpha;
};

/// Request to show a result popup to the user.
struct ShowResultPopupCmd {
    std::string message;
    bool success = false;
};

/// Request to open a CASC storage archive at the given path.
struct OpenCascCmd {
    std::string path;
};

/// Request to select a specific mip level in the viewer.
struct SelectMipCmd {
    i32 level;
};

// ── Menu bar / UI navigation commands ──────────────────────────────────

/// Request to show the OS file-open dialog.
struct ShowOpenDialogCmd {};

/// Request to show the OS file-save dialog.
struct ShowSaveDialogCmd {};

/// Request to open the batch-convert dialog.
struct OpenBatchConvertCmd {};

/// Request to open the CASC browser window.
struct OpenCascBrowserCmd {};

/// Request to open the AI upscale popup.
struct ShowUpscaleDialogCmd {};

/// Request to show the About dialog.
struct ShowAboutCmd {};

/// Request to clear the recent-files list.
struct ClearRecentFilesCmd {};

// ── Data-carrying commands from views ──────────────────────────────────

/// Load a texture extracted from a CASC archive.
struct LoadCascTextureCmd {
    std::string name;
    std::vector<u8> data;
    std::vector<u8> payload;
    std::vector<u8> paylow;
    bool is_d4_tex = false;
};

/// Apply BC3N channel swap to the current texture.
struct ApplyBC3NSwapCmd {};

/// Load a D4 TEX with user-specified payload paths.
struct LoadD4PayloadCmd {
    std::string meta_path;
    std::string payload_path;
    std::string paylow_path;
};

// ============================================================================
// Variant that unifies all command types
// ============================================================================

/// Visitor helper for std::visit with multiple lambdas.
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

using AppCommand = std::variant<
    OpenFileCmd,
    RefreshDisplayCmd,
    RegenerateMipmapsCmd,
    DownscaleCmd,
    StartUpscaleCmd,
    ShowResultPopupCmd,
    OpenCascCmd,
    SelectMipCmd,
    ShowOpenDialogCmd,
    ShowSaveDialogCmd,
    OpenBatchConvertCmd,
    OpenCascBrowserCmd,
    ShowUpscaleDialogCmd,
    ShowAboutCmd,
    ClearRecentFilesCmd,
    LoadCascTextureCmd,
    ApplyBC3NSwapCmd,
    LoadD4PayloadCmd
>;

} // namespace whiteout::gui
