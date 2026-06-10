// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "localization.h"
#include "models/commands.h"

#include <string>
#include <vector>

namespace whiteout::textool::views {

/// Draw the main menu bar.
/// Returns commands for actions the user selected (Open, Save As, etc.).
/// @param has_texture      True if a texture is currently loaded.
/// @param recent_paths     Recent file paths (most-recent first).
/// @param has_upscaler     True if the upscaler feature is compiled in.
/// @param current_language Active UI language (for the Language submenu checkmark).
/// @param pipelines        Discovered standard pipelines (Tools > Run Pipeline).
std::vector<models::AppCommand> drawMenuBar(bool has_texture,
                                            const std::vector<std::string>& recent_paths,
                                            bool has_upscaler, i18n::Language current_language,
                                            const std::vector<models::PipelineInfo>& pipelines);

} // namespace whiteout::textool::views
