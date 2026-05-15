// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file casc_service.h
/// @brief CASC archive I/O operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_http_handler.h>

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

    /// Open a local CASC storage directory.  Enumerates supported texture files
    /// and discovers D4 TEX entries when applicable.
    CascStorageInfo openStorage(const std::string& path);

    /// Set an external listfile (e.g. community-listfile.csv).
    /// The data is used on the next openStorage / startOnlineConnect call.
    void setListfile(std::vector<u8> data);

    /// Begin an asynchronous CDN connection.  Returns immediately.
    /// Poll with pollConnect() each frame; inspect progress via connect*()
    /// accessors.
    ///
    /// @param cache_dir Optional per-CDN persistent cache directory.  When
    ///                  non-empty, the caller must have created it; downloads
    ///                  are stored there so subsequent opens are much faster.
    void startOnlineConnect(const std::string& product, const std::string& region,
                            std::string cache_dir = {});

    /// True while an async online connection is in progress.
    bool isConnecting() const { return is_connecting_.load(std::memory_order_acquire); }

    /// If an async online connection has just finished, returns the result
    /// and finalizes internal state.  Returns nullopt if still connecting
    /// or if no connection was pending.
    std::optional<CascStorageInfo> pollConnect();

    // ── Progress accessors (updated by bg thread) ──────────────────────
    /// Progress step index (cast from ProgressStep), or -1 = not started.
    int8_t connectStep()    const { return connect_step_.load(std::memory_order_relaxed); }
    u32    connectCurrent() const { return connect_current_.load(std::memory_order_relaxed); }
    u32    connectTotal()   const { return connect_total_.load(std::memory_order_relaxed); }

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
    // ── Core helpers ───────────────────────────────────────────────────
    /// Body of the online connect, executed on the background thread.
    CascStorageInfo doOpenOnline(const std::string& product, const std::string& region,
                                 const std::string& cache_dir);
    /// Enumerate storage files into all_files_ / d4_tex_entries_.
    void enumerateStorage();

    // ── Storage ────────────────────────────────────────────────────────
    std::optional<whiteout::storages::casc::Storage> storage_;
    std::unique_ptr<whiteout::utils::SimpleHttpHandler> http_handler_;
    bool storage_open_ = false;
    bool is_d4_ = false;

    std::vector<std::string> all_files_;
    std::vector<CascD4TexEntry> d4_tex_entries_;

    // ── Listfile ───────────────────────────────────────────────────────
    std::vector<u8> listfile_data_;

    // ── Async online connect ───────────────────────────────────────────
    std::atomic<bool>    is_connecting_{false};
    std::thread          connect_thread_;
    std::mutex           connect_mutex_;
    std::optional<CascStorageInfo> connect_result_; ///< Written by bg thread.

    // Progress (written by bg thread, read by UI thread).
    std::atomic<int8_t>  connect_step_{-1};
    std::atomic<u32>     connect_current_{0};
    std::atomic<u32>     connect_total_{0};
};

} // namespace whiteout::textool::services
