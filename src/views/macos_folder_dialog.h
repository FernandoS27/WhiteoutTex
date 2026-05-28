// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#ifdef __APPLE__

#include <SDL3/SDL.h>

namespace whiteout::textool::views {

// Drop-in replacement for SDL_ShowOpenFolderDialog on macOS that enables
// `treatsFilePackagesAsDirectories` on the NSOpenPanel, so the user can pick
// folders that live inside .app bundles (e.g. the Warcraft III install on
// macOS, which is shipped as `Warcraft III.app`).
//
// Signature mirrors SDL_ShowOpenFolderDialog so callers can swap them out
// with a single #ifdef.
void ShowMacFolderDialogAllowingPackages(SDL_DialogFileCallback callback,
                                         void* userdata,
                                         SDL_Window* window,
                                         const char* default_location,
                                         bool allow_many);

} // namespace whiteout::textool::views

#endif // __APPLE__
