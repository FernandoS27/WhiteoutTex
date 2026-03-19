// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <whiteout/casc/storage.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>

#include "common_types.h"

#include <SDL3/SDL.h>

namespace whiteout::gui {

struct RecentPaths;

/// Result returned by CascBrowser::draw() when a file is selected.
struct CascBrowserResult {
    std::string name;                      ///< Display name (e.g. CASC path or D4 tex name).
    std::vector<u8> data;        ///< File data (non-D4) or D4 meta data.
    std::vector<u8> payload;     ///< D4 hi-res payload (empty for non-D4).
    std::vector<u8> paylow;      ///< D4 low-res payload (optional).
    bool is_d4_tex = false;                ///< True when the result is a D4 TEX triplet.

    explicit operator bool() const { return !data.empty(); }
};

/// CASC archive browser.  Opens a Blizzard CASC storage directory and
/// displays a filterable tree of supported texture files.  The user can
/// select a file to extract and open in the texture viewer.
class CascBrowser {
public:
    CascBrowser() = default;

    /// Show the browser window.
    void open();

    /// Draw the browser window.  Returns a result when the user selects
    /// a texture to open (empty/false otherwise).
    CascBrowserResult draw(SDL_Window* window, RecentPaths& recent_paths);

private:
    // ── Inner types ────────────────────────────────────────────────────

    /// A node in the virtual file-system tree built from CASC paths.
    struct TreeNode {
        std::string name;           ///< Directory or file name segment.
        std::string full_path;      ///< Full CASC path (files only).
        i32 sno_id = -1;  ///< D4 SNO ID (-1 if not a D4 entry).
        std::vector<TreeNode> children;
        bool is_file = false;
    };

    /// Combined meta cache entry for a single SNO within a combined .dat file.
    struct CombinedEntry {
        std::shared_ptr<std::vector<u8>> file_data;
        size_t data_offset = 0;
        size_t data_size = 0;
    };

    /// Lightweight D4 TOC texture entry (name + SNO ID).
    struct D4TexEntry {
        std::string name;
        i32 sno_id;
    };

    // ── Methods ────────────────────────────────────────────────────────

    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist,
                                              i32 filter);

    void processFolderResult();
    void openStorage();
    void resetState();
    void loadD4Textures();
    void buildTree();
    void insertPathIntoTree(TreeNode& root, const std::string& file_path);
    CascBrowserResult drawTree(const TreeNode& node);
    CascBrowserResult readCascFile(const std::string& casc_path);
    CascBrowserResult readD4Tex(const std::string& name, i32 sno_id);

    static bool isSupportedExtension(const std::string& name);
    static bool hasVisibleAscii(const std::string& s);

    // ── State ──────────────────────────────────────────────────────────

    bool show_window_ = false;
    bool storage_open_ = false;
    bool is_d4_ = false;
    char storage_path_buf_[PATH_BUFFER_SIZE] = {};
    char search_buf_[256] = {};

    FolderState folder_state_;
    whiteout::casc::Storage storage_;

    // Enumerated data
    std::vector<std::string> all_files_;
    std::vector<D4TexEntry> d4_tex_entries_;
    std::unordered_map<i32, CombinedEntry> combined_cache_;
    u32 d4_tex_format_hash_ = 0;

    // Display
    TreeNode root_;
    std::string product_name_;
    u32 file_count_ = 0;
    std::string status_;
};

} // namespace whiteout::gui
