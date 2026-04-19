// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file casc_service.h
/// @brief CASC archive I/O operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"

#include <optional>
#include <string>
#include <vector>

#include <whiteout/storages/casc/storage.h>

namespace whiteout::textool::services {

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

/// Lightweight D4 TEX entry discovered from the enriched root.
struct CascD4TexEntry {
    std::string name;      ///< Display name (e.g. "SomeName.tex").
    std::string meta_path; ///< Full CASC path to the meta file.
};

/// Information about an opened CASC storage.
struct CascStorageInfo {
    std::string product_name;
    u32 file_count = 0;
    bool is_d4 = false;
    std::string status;
};

/// CASC archive I/O service.  Owns the storage handle and enumerated file
/// lists.  No UI or SDL dependencies.
///
/// The underlying library auto-detects game-specific root formats (D3, D4,
/// WoW) and enriches paths accordingly.  D4 textures are split across
/// meta / payload / paylow files; this service reassembles them on read.
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
    CascFileResult readD4Tex(const std::string& meta_path);

    /// Enumerated regular files (sorted).
    const std::vector<std::string>& files() const {
        return all_files_;
    }

    /// Enumerated D4 texture entries (sorted by name).
    const std::vector<CascD4TexEntry>& d4Entries() const {
        return d4_tex_entries_;
    }

private:
    std::optional<whiteout::storages::casc::Storage> storage_;
    bool storage_open_ = false;
    bool is_d4_ = false;

    std::vector<std::string> all_files_;
    std::vector<CascD4TexEntry> d4_tex_entries_;
};

} // namespace whiteout::textool::services
