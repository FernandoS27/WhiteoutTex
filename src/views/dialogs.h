// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "models/app_state.h"
#include "models/commands.h"

#include <filesystem>
#include <string>
#include <vector>

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h" // UpscalerModel
#endif

namespace whiteout::gui {

/// Draw the About WhiteoutTex popup.
void drawAboutDialog(bool& show);

/// Draw the generic result popup (success/error message).
void drawResultDialog(UIFlags& ui);

/// Draw the BC3N normal-map confirmation dialog.
/// Returns ApplyBC3NSwapCmd when the user clicks "Yes".
std::vector<AppCommand> drawBC3NDialog(bool& show);

/// Draw the Diablo IV payload-path dialog.
/// Returns LoadD4PayloadCmd when the user provides valid paths and clicks "Load".
std::vector<AppCommand> drawD4PayloadDialog(UIFlags& ui);

#ifdef WHITEOUT_HAS_UPSCALER
/// Draw the AI Upscale popup.
/// Returns StartUpscaleCmd when the user clicks "Upscale".
/// @param show              Trigger flag — set true to open the popup.
/// @param models            Available upscaler models.
/// @param selected_index    Currently selected model index (mutated by combo).
/// @param has_gpu           Whether a Vulkan-capable GPU is available.
/// @param is_running        Whether an upscale is already in progress.
/// @param status            Current upscaler status string (may be empty).
/// @param model_dir         Path to the models directory (shown in UI).
/// @param tex_width         Current texture width (0 if no texture loaded).
/// @param tex_height        Current texture height (0 if no texture loaded).
std::vector<AppCommand> drawUpscaleDialog(
    bool& show,
    const std::vector<UpscalerModel>& models,
    i32& selected_index,
    bool has_gpu,
    bool is_running,
    const std::string& status,
    const std::filesystem::path& model_dir,
    i32 tex_width, i32 tex_height);
#endif

} // namespace whiteout::gui
