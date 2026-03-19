// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "models/app_state.h"
#include "models/commands.h"

#include <vector>

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

} // namespace whiteout::gui
