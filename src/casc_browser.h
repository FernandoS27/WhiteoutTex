// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>
#include <vector>

#include "common_types.h"
#include "models/commands.h"
#include "services/casc_service.h"

#include <SDL3/SDL.h>

namespace whiteout::gui {

struct RecentPaths;

/// Backwards-compatible alias — kept so callers (e.g. App) do not need to
/// change their variable types.
using CascBrowserResult = CascFileResult;

/// CASC archive browser.  Opens a Blizzard CASC storage directory and
/// displays a filterable tree of supported texture files.  The user can
/// select a file to extract and open in the texture viewer.
class CascBrowser {
public:
    CascBrowser() = default;

    /// Show the browser window.
    void open();

    /// Draw the browser window.  Returns commands when the user selects
    /// a texture to open (e.g. LoadCascTextureCmd).
    std::vector<AppCommand> draw(SDL_Window* window, RecentPaths& recent_paths);

private:
    // ── Inner types ────────────────────────────────────────────────────

    /// A node in the virtual file-system tree built from CASC paths.
    struct TreeNode {
        std::string name;           ///< Directory or file name segment.
        std::string full_path;      ///< Full CASC path (files only).
        i32 sno_id = -1;            ///< D4 SNO ID (-1 if not a D4 entry).
        std::vector<TreeNode> children;
        bool is_file = false;
    };

    // ── Methods ────────────────────────────────────────────────────────

    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist,
                                              i32 filter);

    void processFolderResult();
    void openStorage();
    void buildTree();
    void insertPathIntoTree(TreeNode& root, const std::string& file_path);
    std::vector<AppCommand> drawTree(const TreeNode& node);

    // ── State ──────────────────────────────────────────────────────────

    CascService casc_service_;

    bool show_window_ = false;
    char storage_path_buf_[PATH_BUFFER_SIZE] = {};
    char search_buf_[256] = {};

    FolderState folder_state_;

    // Display
    TreeNode root_;
    std::string product_name_;
    u32 file_count_ = 0;
    std::string status_;
};

} // namespace whiteout::gui
