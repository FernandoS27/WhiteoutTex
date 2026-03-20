// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file casc_service.h
/// @brief CASC archive I/O operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <whiteout/casc/storage.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>

namespace whiteout::gui {

/// Result returned when extracting a file from CASC storage.
struct CascFileResult {
    std::string name;        ///< Display name (e.g. CASC path or D4 tex name).
    std::vector<u8> data;    ///< File data (non-D4) or D4 meta data.
    std::vector<u8> payload; ///< D4 hi-res payload (empty for non-D4).
    std::vector<u8> paylow;  ///< D4 low-res payload (optional).
    bool is_d4_tex = false;  ///< True when the result is a D4 TEX triplet.

    explicit operator bool() const {
        return !data.empty();
    }
};

/// Lightweight D4 TOC texture entry (name + SNO ID).
struct CascD4TexEntry {
    std::string name;
    i32 sno_id;
};

/// Information about an opened CASC storage.
struct CascStorageInfo {
    std::string product_name;
    u32 file_count = 0;
    bool is_d4 = false;
    std::string status;
};

/// CASC archive I/O service.  Owns the storage handle, combined meta cache,
/// and enumerated file lists.  No UI or SDL dependencies.
class CascService {
public:
    CascService() = default;

    /// Open a CASC storage directory.  Enumerates supported texture files
    /// and discovers D4 TEX entries when applicable.
    CascStorageInfo openStorage(const std::string& path);

    /// Close the current storage and clear all cached data.
    void close();

    /// Returns true if a storage is currently open.
    bool isOpen() const {
        return storage_open_;
    }

    /// Returns true if the current storage is a Diablo IV archive.
    bool isD4() const {
        return is_d4_;
    }

    /// Read a regular (non-D4) file from the open storage.
    CascFileResult readFile(const std::string& casc_path);

    /// Read a D4 TEX (meta + payload + paylow) from the open storage.
    CascFileResult readD4Tex(const std::string& name, i32 sno_id);

    /// Enumerated regular files (sorted).
    const std::vector<std::string>& files() const {
        return all_files_;
    }

    /// Enumerated D4 texture entries (sorted by name).
    const std::vector<CascD4TexEntry>& d4Entries() const {
        return d4_tex_entries_;
    }

private:
    /// Combined meta cache entry for a single SNO.
    struct CombinedEntry {
        std::shared_ptr<std::vector<u8>> file_data;
        size_t data_offset = 0;
        size_t data_size = 0;
    };

    void loadD4Textures(whiteout::sno::CoreToc& toc);

    whiteout::casc::Storage storage_;
    std::unordered_map<i32, CombinedEntry> combined_cache_;
    u32 d4_tex_format_hash_ = 0;
    bool storage_open_ = false;
    bool is_d4_ = false;

    std::vector<std::string> all_files_;
    std::vector<CascD4TexEntry> d4_tex_entries_;
};

} // namespace whiteout::gui
