// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file casc_service.h
/// @brief CASC archive I/O operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_http_handler.h>

namespace whiteout::textool::services {

/// connectStep() value for the file-list walk that follows a successful
/// openOnline.  The library reports no ProgressStep for it — by then the
/// storage is already Ready — but under LoadOnDemand it is where the encoding
/// table and root manifest are actually fetched, so it is the longest wait of
/// the whole connect and needs a label of its own.
inline constexpr i32 kConnectStepFileList = -2;

/// connectStep() value for the enumeration that follows a successful local
/// open.  The library's steps end at Ready, but walking the root for textures
/// and sorting them is a second or more on a big install, so it gets a label of
/// its own rather than leaving the modal on a finished-looking "Ready".
inline constexpr i32 kConnectStepEnumerate = -3;

/// Result returned when extracting a file from CASC storage.
struct CascFileResult {
    std::string name;        ///< Display name (e.g. CASC path or D4 tex name).
    std::vector<u8> data;    ///< File data, D4 meta data, or Overwatch TXTR header.
    std::vector<u8> payload; ///< D4 hi-res payload (empty for non-D4).
    std::vector<u8> paylow;  ///< D4 low-res payload (optional).
    /// Overwatch `04D` payload files, in chain order.  Entries the storage
    /// could not supply are left empty rather than dropped, so a partial set
    /// still decodes to the mips it does cover.
    std::vector<std::vector<u8>> payloads;
    bool is_d4_tex = false;  ///< True when the result is a D4 TEX triplet.
    bool is_txtr = false;    ///< True when the result is an Overwatch TXTR.

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
/// Overwatch, WoW) and enriches paths accordingly.  D4 textures are split
/// across meta / payload / paylow files and Overwatch textures across a header
/// and up to three payloads; this service reassembles both on read.
class CascService {
public:
    CascService() = default;

    /// Joins any open still running.  Without this, quitting the app mid-open
    /// destroys a joinable std::thread, which terminates the process.
    ~CascService() {
        close();
    }

    /// Open a local CASC storage directory.  Enumerates supported texture files
    /// and discovers D4 TEX entries when applicable.  Blocking; prefer
    /// startLocalOpen() from a UI thread.
    CascStorageInfo openStorage(const std::string& path);

    /// Begin an asynchronous local open.  Returns immediately and is polled
    /// exactly like startOnlineConnect(): pollConnect() each frame, progress
    /// via the connect*() accessors.
    ///
    /// Opening a full game install costs seconds — three on Overwatch, most of
    /// it inside the library — so it cannot run on the frame loop.
    void startLocalOpen(std::string path);

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

    /// True while an async open (local or online) is in progress.
    bool isConnecting() const { return is_connecting_.load(std::memory_order_acquire); }

    /// True when the async open in progress is a local one, so the UI can
    /// label it as opening a folder rather than as talking to a CDN.
    bool isLocalOpen() const { return connect_is_local_.load(std::memory_order_relaxed); }

    /// If an async online connection has just finished, returns the result
    /// and finalizes internal state.  Returns nullopt if still connecting
    /// or if no connection was pending.
    std::optional<CascStorageInfo> pollConnect();

    // ── Progress accessors (updated by bg thread) ──────────────────────
    /// The step the connect last reported, cast from ProgressStep; or
    /// kConnectStepFileList during the post-open walk; or -1 before it starts.
    i32 connectStep()    const { return connect_step_.load(std::memory_order_relaxed); }
    /// Units done / expected within the current step.  Items when the step
    /// counts them, bytes when it only counts those, 0 when it counts neither.
    u64 connectCurrent() const { return connect_current_.load(std::memory_order_relaxed); }
    u64 connectTotal()   const { return connect_total_.load(std::memory_order_relaxed); }

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

    /// Read an Overwatch TXTR (`004` header plus its `04D` payloads) from the
    /// open storage.  The payload GUIDs are derived from the header's own, and
    /// each payload is fetched from the manifest folder the header lives in.
    CascFileResult readTxtr(const std::string& header_path);

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
    /// Body of the local open.  Assumes any previous storage is already closed,
    /// which is what lets it run on the connect thread (close() joins that
    /// thread, so it must never be reached from inside it).
    CascStorageInfo doOpenLocal(const std::string& path);
    /// Body of the online connect, executed on the background thread.
    CascStorageInfo doOpenOnline(const std::string& product, const std::string& region,
                                 const std::string& cache_dir);
    /// Enumerate storage files into all_files_ / d4_tex_entries_.
    void enumerateStorage();
    /// Reset the progress atomics and mark an async open as started.
    void beginConnect(bool local);
    /// The progress callback both open paths install: it feeds the connect
    /// atomics and returns false once close() has asked the open to stop.
    std::function<bool(const whiteout::storages::casc::ProgressInfo&)> progressSink();

    // ── Storage ────────────────────────────────────────────────────────
    std::optional<whiteout::storages::casc::Storage> storage_;
    std::unique_ptr<whiteout::interfaces::HttpHandler> http_handler_;
    bool storage_open_ = false;
    bool is_d4_ = false;
    bool is_ow_ = false;
    /// Opened from disk rather than from a CDN.  Decides how enumerateStorage()
    /// walks the root: only a local storage has its encoding table fully
    /// resolved, which is what makes the cheap single walk safe.
    bool is_local_ = false;

    std::vector<std::string> all_files_;
    std::vector<CascD4TexEntry> d4_tex_entries_;

    // ── Listfile ───────────────────────────────────────────────────────
    std::vector<u8> listfile_data_;

    // ── Async online connect ───────────────────────────────────────────
    std::atomic<bool>    is_connecting_{false};
    std::thread          connect_thread_;
    std::mutex           connect_mutex_;
    std::optional<CascStorageInfo> connect_result_; ///< Written by bg thread.

    /// Set by close() so an open that is still running gives up instead of
    /// holding the shutdown (or the next open) for its full duration.  Read by
    /// the progress callback and the enumeration walk.
    std::atomic<bool>    cancel_open_{false};

    // Progress (written by bg thread, read by UI thread).
    std::atomic<bool>    connect_is_local_{false};
    std::atomic<i32>     connect_step_{-1};
    std::atomic<u64>     connect_current_{0};
    std::atomic<u64>     connect_total_{0};
};

} // namespace whiteout::textool::services
