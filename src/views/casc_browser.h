// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>
#include <vector>

#include "common_types.h"
#include "models/commands.h"
#include "preferences.h"
#include "services/casc_service.h"
#include "services/mpq_service.h"

#include <SDL3/SDL.h>

namespace whiteout::textool::views {

/// Backwards-compatible alias — kept so callers (e.g. App) do not need to
/// change their variable types.
using CascBrowserResult = services::CascFileResult;

/// Unified Blizzard storage browser.  Supports local CASC directories,
/// local MPQ archives, and online CDN-backed CASC archives.
class CascBrowser {
public:
    CascBrowser() = default;

    /// Show the browser window.
    void open();

    /// Draw the browser window.  Returns commands when the user selects
    /// a texture to open (e.g. LoadCascTextureCmd).
    std::vector<models::AppCommand> draw(SDL_Window* window, RecentPaths& recent_paths);

private:
    // ── Inner types ────────────────────────────────────────────────────

    /// Active local storage kind.
    enum class LocalKind { None, Casc, Mpq };

    /// A node in the virtual file-system tree built from CASC/MPQ paths.
    struct TreeNode {
        std::string name;      ///< Directory or file name segment.
        std::string full_path; ///< Full path.
        bool is_d4_tex = false;///< True if this is a D4 TEX meta entry.
        std::vector<TreeNode> children;
        bool is_file = false;
        u32 total_files = 0;   ///< Files in this subtree (folders only).
    };

    // ── Methods ────────────────────────────────────────────────────────

    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist,
                                             i32 filter);
    static void SDLCALL fileDialogCallback(void* userdata, const char* const* filelist,
                                           i32 filter);

    void processFolderResult();
    void processFileResult();
    void openLocalStorage();
    void loadListfileFromExeDir();
    void buildTree();
    /// Insert @p file_path into @p root, creating the nodes its segments name.
    ///
    /// Callers must supply paths in ascending order (both service file lists
    /// are sorted).  That is what keeps the insert linear: siblings sharing a
    /// prefix arrive contiguously, so a file leaf either continues the previous
    /// path or is new, and never has to be searched for among the ones already
    /// there.  Overwatch puts 390,000 files in a single folder, where searching
    /// costs 2e11 string comparisons — hours, not seconds.
    void insertPathIntoTree(TreeNode& root, const std::string& file_path);
    /// Sort @p node recursively (folders first, then files, each
    /// case-insensitively by name) and return the subtree's file count.
    static u32 sortTree(TreeNode& node);
    /// Draw one file row; a double click reads the file and appends a load
    /// command to @p commands.
    void drawFileNode(const TreeNode& child, std::vector<models::AppCommand>& commands);
    std::vector<models::AppCommand> drawTree(const TreeNode& node);

    // ── State ──────────────────────────────────────────────────────────

    services::CascService casc_service_;
    services::MpqService  mpq_service_;
    LocalKind local_kind_ = LocalKind::None;

    // Mode toggle
    enum class BrowserMode { Local, Online };
    BrowserMode mode_ = BrowserMode::Local;
    int selected_product_idx_ = 0;
    int selected_region_idx_ = 0;

    bool show_window_ = false;
    char storage_path_buf_[PATH_BUFFER_SIZE] = {};
    char search_buf_[256] = {};

    FolderState folder_state_; ///< For CASC folder dialog.
    FolderState file_state_;   ///< For MPQ file dialog (reuses the same type).

    /// Folder whose async local open is in flight; empty when the pending
    /// open (if any) is an online connect.  Pushed to the recent list once
    /// the open lands successfully.
    std::string pending_local_path_;

    // Display
    TreeNode root_;
    bool auto_expand_ = false; ///< Force folders open while a filter is active.
    std::string product_name_;
    u32 file_count_ = 0;
    std::string status_;
};

} // namespace whiteout::textool::views
