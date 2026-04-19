// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file mpq_service.h
/// @brief MPQ archive I/O operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"

#include <optional>
#include <string>
#include <vector>

#include <whiteout/storages/mpq/storage.h>

namespace whiteout::textool::services {

/// Result returned when extracting a file from an MPQ archive.
struct MpqFileResult {
    std::string name;        ///< File name within the MPQ.
    std::vector<u8> data;    ///< File data.

    explicit operator bool() const { return !data.empty(); }
};

/// Information about an opened MPQ archive.
struct MpqStorageInfo {
    std::string archive_name; ///< Display name (filename without path).
    u32 file_count = 0;
    std::string status;
};

/// MPQ archive I/O service.  Owns the archive handle and the enumerated
/// texture file list.  No UI or SDL dependencies.
class MpqService {
public:
    MpqService() = default;

    /// Open an MPQ archive file.  Enumerates supported texture files.
    MpqStorageInfo openStorage(const std::string& path);

    /// Close the current archive and clear all cached data.
    void close();

    /// Returns true if an archive is currently open.
    bool isOpen() const { return storage_open_; }

    /// Read a file from the open archive.
    MpqFileResult readFile(const std::string& name);

    /// Enumerated texture files (sorted).
    const std::vector<std::string>& files() const { return all_files_; }

private:
    std::optional<whiteout::storages::mpq::Storage> storage_;
    bool storage_open_ = false;
    std::vector<std::string> all_files_;
};

} // namespace whiteout::textool::services
