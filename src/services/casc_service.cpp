// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "format_registry.h"
#include "services/casc_service.h"
#include "thread_pool_manager.h"

#include <whiteout/textures/txtr/txtr.h>
#include <whiteout/utils/simple_http_handler.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <span>
#include <string>

namespace {

using namespace whiteout;

/// D4 enriched meta texture path prefix (after the root folder and colon).
constexpr std::string_view kD4MetaPrefix = ":meta\\Texture\\";

/// The extensions of every archive-browsable format, collected from the format
/// registry once per open.
///
/// The test runs on every path a storage holds — 2.2 million on Diablo IV — so
/// it cannot afford the registry's two linear table scans (nor the std::string
/// the old version built for each one).  The registry stays the source of
/// truth; this is just its Archive slice, flattened.
class ArchiveExtensions {
public:
    ArchiveExtensions() {
        for (const auto& row : textures::formatTable()) {
            if (!row.has(textures::FmtCap::Archive))
                continue;
            for (const auto& ext : row.exts) {
                // Outgrowing either bound would silently stop the browser from
                // listing a format, so say so rather than dropping it quietly.
                assert(count_ < exts_.size() && ext.size() <= kMaxExt &&
                       "widen ArchiveExtensions for the new format");
                if (count_ >= exts_.size() || ext.size() > kMaxExt)
                    continue;
                exts_[count_++] = ext;
                max_len_ = std::max(max_len_, ext.size());
            }
        }
    }

    /// Returns true if @p name ends in one of them (case-insensitive).
    bool matches(std::string_view name) const {
        const auto dot = name.rfind('.');
        if (dot == std::string_view::npos)
            return false;
        const auto ext = name.substr(dot);
        if (ext.size() > max_len_)
            return false;
        char lower[kMaxExt];
        for (std::size_t i = 0; i < ext.size(); ++i)
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
        const std::string_view lowered(lower, ext.size());
        for (std::size_t i = 0; i < count_; ++i)
            if (exts_[i] == lowered)
                return true;
        return false;
    }

private:
    /// Twice the longest archive extension today (".texture").
    static constexpr std::size_t kMaxExt = 16;

    std::array<std::string_view, 24> exts_{};
    std::size_t count_ = 0;
    std::size_t max_len_ = 0;
};

/// Returns true if @p path is a D4 enriched meta texture path.
/// Format: <folder>:meta\Texture\<name>.tex
bool isD4MetaTexturePath(std::string_view path) {
    auto colon = path.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    return path.substr(colon).starts_with(kD4MetaPrefix);
}

/// Returns true if @p path is a D4 enriched sub-file (payload/paylow/paymed/child).
/// Only matches known D4 sub-path prefixes so that TVFS sub-container paths
/// (e.g. Warcraft III "war3.w3mod:units\...") are not incorrectly filtered.
bool isD4SubPath(std::string_view path) {
    auto colon = path.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    auto rest = path.substr(colon);
    return rest.starts_with(":meta\\") ||
           rest.starts_with(":payload\\") ||
           rest.starts_with(":paylow\\") ||
           rest.starts_with(":paymed\\") ||
           rest.starts_with(":child\\");
}

/// Extract display name from a D4 meta path.
/// e.g. "base:meta\Texture\SomeName.tex" → "SomeName.tex"
std::string d4DisplayName(std::string_view meta_path) {
    auto sep = meta_path.find_last_of("\\/");
    if (sep != std::string_view::npos)
        return std::string(meta_path.substr(sep + 1));
    return std::string(meta_path);
}

/// Replace ":meta\" with ":<target>\" in a D4 path.
std::string d4ReplaceSub(const std::string& meta_path, std::string_view target) {
    constexpr std::string_view kMetaSub = ":meta\\";
    auto pos = meta_path.find(kMetaSub);
    if (pos == std::string::npos)
        return {};
    std::string result = meta_path;
    result.replace(pos + 1, kMetaSub.size() - 2, target);
    return result;
}


// ============================================================================
// Overwatch asset-path helpers
// ============================================================================

/// Sixteen lowercase hex digits — how the Overwatch root names an asset file.
std::string owHex16(u64 value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4)
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
    return out;
}

/// The GUID an Overwatch asset path ends with, or 0 when it carries none.
/// Asset paths are `<manifest folders>\<16 hex digits>.<type>`.
u64 owTrailingGuid(std::string_view path) {
    if (auto const slash = path.find_last_of("\\/"); slash != std::string_view::npos)
        path = path.substr(slash + 1);
    if (auto const dot = path.find('.'); dot != std::string_view::npos)
        path = path.substr(0, dot);
    if (path.size() != 16)
        return 0;

    u64 guid = 0;
    for (char const c : path) {
        if (c >= '0' && c <= '9')
            guid = (guid << 4) | static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f')
            guid = (guid << 4) | static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            guid = (guid << 4) | static_cast<u64>(c - 'A' + 10);
        else
            return 0;
    }
    return guid;
}

/// The path a payload of @p asset_path lives at.  A payload sits in the manifest
/// folder its texture does, so only the GUID changes.  The type extension is
/// left off: the root matches an asset path without one, so the lookup does not
/// depend on the payload type carrying a name.
std::string owSiblingPath(std::string_view asset_path, u64 sibling_guid) {
    auto const slash = asset_path.find_last_of("\\/");
    auto const dir =
        (slash == std::string_view::npos) ? std::string_view{} : asset_path.substr(0, slash + 1);
    return std::string(dir) + owHex16(sibling_guid);
}

/// Returns true if @p path names an Overwatch TXTR header (extension `.txtr`,
/// which the root gives both texture asset types, `004` and `0F1`).
bool isTxtrPath(std::string_view path) {
    constexpr std::string_view kExt = ".txtr";
    if (path.size() < kExt.size())
        return false;
    auto const tail = path.substr(path.size() - kExt.size());
    for (std::size_t i = 0; i < kExt.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != kExt[i])
            return false;
    return true;
}

} // anonymous namespace

namespace whiteout::textool::services {

// ============================================================================
// Open / Close
// ============================================================================

CascStorageInfo CascService::openStorage(const std::string& path) {
    close();
    return doOpenLocal(path);
}

CascStorageInfo CascService::doOpenLocal(const std::string& path) {
    CascStorageInfo info;

    if (path.empty()) {
        info.status = "Please enter a CASC storage path.";
        return info;
    }

    // Use OpenOptions with a memory cache for D4 combined meta performance,
    // and the global worker pool for parallel I/O.
    storages::casc::OpenOptions opts;
    opts.path = path;
    opts.memoryCacheSize = 256 * 1024 * 1024; // 256 MB
    opts.pool = threadPoolManager().get();
    if (!listfile_data_.empty())
        opts.listfile = std::span<const u8>(listfile_data_);
    opts.progressCallback = progressSink();

    auto result = storages::casc::Storage::open(opts);
    if (!result) {
        info.status = "Failed to open CASC storage at: " + path;
        return info;
    }
    storage_ = std::move(*result);
    is_local_ = true;

    // The library's steps stop at Ready; the walk below is the rest of the wait.
    connect_step_.store(kConnectStepEnumerate, std::memory_order_relaxed);
    connect_current_.store(0, std::memory_order_relaxed);
    connect_total_.store(0, std::memory_order_relaxed);

    if (auto prod = storage_->product())
        info.product_name = prod->name + " (" + prod->version + ")";
    if (auto count = storage_->totalFileCount())
        info.file_count = *count;

    // Enumerate all files.  The library handles D3/D4 root enrichment
    // automatically — D4 paths are human-readable (e.g. base:meta\Texture\Name.tex),
    // D3 paths use the correct group directory names.
    enumerateStorage();

    info.is_d4 = is_d4_;
    storage_open_ = true;

    const size_t total = all_files_.size() + d4_tex_entries_.size();
    info.status = "Opened: " + std::to_string(total) + " supported textures found";
    if (is_d4_) {
        info.status += " (" + std::to_string(d4_tex_entries_.size()) + " D4 TEX + " +
                       std::to_string(all_files_.size()) + " other)";
    }
    info.status += " (of " + std::to_string(info.file_count) + " total files).";
    return info;
}

void CascService::close() {
    // Join any pending background open before clearing state, telling it to
    // stop first — an Overwatch open is seconds long, and neither quitting the
    // app nor opening a different folder should have to sit through it.
    cancel_open_.store(true, std::memory_order_relaxed);
    if (connect_thread_.joinable())
        connect_thread_.join();
    cancel_open_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(connect_mutex_);
        connect_result_.reset();
    }
    is_connecting_.store(false, std::memory_order_release);

    if (storage_)
        storage_->close();
    storage_.reset();
    // Release the HTTP handler after the storage (which holds a raw pointer to it).
    http_handler_.reset();
    storage_open_ = false;
    is_d4_ = false;
    is_ow_ = false;
    is_local_ = false;
    all_files_.clear();
    d4_tex_entries_.clear();
}

// ============================================================================
// Listfile
// ============================================================================

void CascService::setListfile(std::vector<u8> data) {
    listfile_data_ = std::move(data);
}

// ============================================================================
// Async online connect
// ============================================================================

void CascService::beginConnect(bool local) {
    cancel_open_.store(false, std::memory_order_relaxed);
    connect_is_local_.store(local, std::memory_order_relaxed);
    connect_step_.store(-1, std::memory_order_relaxed);
    connect_current_.store(0, std::memory_order_relaxed);
    connect_total_.store(0, std::memory_order_relaxed);
    is_connecting_.store(true, std::memory_order_release);
}

std::function<bool(const storages::casc::ProgressInfo&)> CascService::progressSink() {
    return [this](const storages::casc::ProgressInfo& progress) -> bool {
        connect_step_.store(static_cast<i32>(progress.step), std::memory_order_relaxed);
        // A step counts items or bytes, not both; report whichever it filled in.
        const bool by_items = progress.total != 0;
        connect_current_.store(by_items ? progress.current : progress.bytesDone,
                               std::memory_order_relaxed);
        connect_total_.store(by_items ? progress.total : progress.bytesTotal,
                             std::memory_order_relaxed);
        // Returning false makes the library abandon the open.
        return !cancel_open_.load(std::memory_order_relaxed);
    };
}

void CascService::startLocalOpen(std::string path) {
    close(); // also joins any pending thread and clears state
    beginConnect(true);

    connect_thread_ = std::thread([this, path = std::move(path)] {
        // doOpenLocal, not openStorage: openStorage closes first, and close()
        // joins this very thread.
        auto info = doOpenLocal(path);
        {
            std::lock_guard lock(connect_mutex_);
            connect_result_ = std::move(info);
        }
        is_connecting_.store(false, std::memory_order_release);
    });
}

void CascService::startOnlineConnect(const std::string& product, const std::string& region,
                                     std::string cache_dir) {
    close(); // also joins any pending thread and clears state
    beginConnect(false);

    connect_thread_ = std::thread([this, product, region, cache_dir = std::move(cache_dir)] {
        auto info = doOpenOnline(product, region, cache_dir);
        {
            std::lock_guard lock(connect_mutex_);
            connect_result_ = std::move(info);
        }
        is_connecting_.store(false, std::memory_order_release);
    });
}

std::optional<CascStorageInfo> CascService::pollConnect() {
    // Still running — nothing to return yet.
    if (is_connecting_.load(std::memory_order_acquire))
        return std::nullopt;
    // No thread pending.
    if (!connect_thread_.joinable())
        return std::nullopt;
    connect_thread_.join();
    std::lock_guard lock(connect_mutex_);
    return std::move(connect_result_);
}

CascStorageInfo CascService::doOpenOnline(const std::string& product, const std::string& region,
                                           const std::string& cache_dir) {
    CascStorageInfo info;

    if (product.empty()) {
        info.status = "Please select a product.";
        return info;
    }

    http_handler_ = std::make_unique<whiteout::utils::SimpleHttpHandler>(4);

    storages::casc::OnlineOpenOptions opts;
    opts.product = product;
    opts.region = region.empty() ? "us" : region;
    opts.http = http_handler_.get();
    opts.memoryCacheSize = 256 * 1024 * 1024; // 256 MB
    opts.pool = threadPoolManager().get();
    opts.cacheDir = cache_dir;
    // Use FullLazy *minus* LazyVfsSubmanifest:
    //
    // - LoadOnDemand / LazyEncodingFrames / LazyArchiveIndex / LazyIdxBuckets
    //   defer the big downloads, so openOnline returns quickly; the heavy
    //   work runs during our first listFiles() under the synthetic step
    //   below.
    //
    // - LazyVfsSubmanifest must NOT be set for plain-TVFS games (notably
    //   Warcraft III Reforged), because their entire file table lives inside
    //   `war3.w3mod:` / `_hd.w3mod:` sub-manifests.  Under LazyVfsSubmanifest
    //   those sub-manifests are resolved one-by-one via a synchronous
    //   resolver during TvfsRoot::parse; any resolver miss silently drops
    //   the sub-tree (the container entry survives but its children don't),
    //   leaving an apparently-empty storage.  Letting prefetchVfsOnline run
    //   pulls them all in parallel up front.
    opts.flags = storages::casc::StorageFeatureFlags::FullLazy &
                 ~storages::casc::StorageFeatureFlags::LazyVfsSubmanifest;
    if (!listfile_data_.empty())
        opts.listfile = std::span<const u8>(listfile_data_);
    opts.progressCallback = progressSink();

    auto result = storages::casc::Storage::openOnline(opts);
    if (!result) {
        info.status = "Failed to connect to CDN for product '" + product + "' (" + opts.region + ").";
        http_handler_.reset();
        return info;
    }
    storage_ = std::move(*result);

    // Under FullLazy + LoadOnDemand, openOnline emits Ready before any of the
    // big work has happened — encoding TOC + root manifest fetch is deferred
    // until the first ensureLoaded() call, which is triggered below by
    // totalFileCount/listFiles.  Move to the synthetic step so the modal labels
    // this final (network-bound) phase honestly instead of sitting on a stale
    // "Ready".
    connect_step_.store(kConnectStepFileList, std::memory_order_relaxed);
    connect_current_.store(0, std::memory_order_relaxed);
    connect_total_.store(0, std::memory_order_relaxed);

    if (auto prod = storage_->product())
        info.product_name = prod->name + " (" + prod->version + ")";
    if (auto count = storage_->totalFileCount())
        info.file_count = *count;

    enumerateStorage();

    info.is_d4 = is_d4_;
    storage_open_ = true;

    const size_t total_files = all_files_.size() + d4_tex_entries_.size();
    info.status = "[Online] Opened: " + std::to_string(total_files) + " supported textures found";
    if (is_d4_) {
        info.status += " (" + std::to_string(d4_tex_entries_.size()) + " D4 TEX + " +
                       std::to_string(all_files_.size()) + " other)";
    }
    info.status += " (of " + std::to_string(info.file_count) + " total files).";
    return info;
}

// ============================================================================
// enumerateStorage  (shared by local and online paths)
// ============================================================================

void CascService::enumerateStorage() {
    is_ow_ = storage_->rootFormat() == storages::casc::RootFormat::Overwatch;

    if (is_ow_) {
        // Overwatch names every asset by GUID alone — a current install has
        // around twenty-four million of them, and listFiles() would hand back
        // more than a gigabyte of strings before a single one is filtered.  The
        // masked walk keeps the suffix test inside the root, so only the
        // textures ever become std::strings.  It is also the only browsable
        // type here: everything else is model, sound, or client data.
        storage_->enumerate("*.txtr", [this](const storages::casc::EnumerateEntry& e) {
            all_files_.emplace_back(e.path);
            return !cancel_open_.load(std::memory_order_relaxed);
        });
        std::sort(all_files_.begin(), all_files_.end());
        // An asset listed by two manifests that reduce to the same folder comes
        // back twice under one path.  On a current install that is every one of
        // them, so dropping the repeats halves both the count shown and the
        // ~100 MiB the paths occupy.
        all_files_.erase(std::unique(all_files_.begin(), all_files_.end()), all_files_.end());
        return;
    }

    const ArchiveExtensions archive_exts;

    // Sort one path into the lists it belongs in.  Takes the path by forwarding
    // reference so the walk below can hand over ownership of a string it has
    // already built instead of having it copied a second time.
    auto consider = [&](auto&& path) {
        if (!archive_exts.matches(path))
            return;
        if (isD4MetaTexturePath(path)) {
            is_d4_ = true;
            d4_tex_entries_.push_back({d4DisplayName(path), std::string(path)});
        } else if (!isD4SubPath(path)) {
            all_files_.emplace_back(std::forward<decltype(path)>(path));
        }
    };

    if (is_local_) {
        // One walk of the root, with the extension test applied to the path the
        // root already holds, so only the textures ever become std::strings.
        // listFiles() would build all of them first — 2.2 million strings and
        // 200 MiB on Diablo IV — to then discard four fifths.
        storage_->enumerate([&](const storages::casc::EnumerateEntry& e) {
            consider(e.path);
            return !cancel_open_.load(std::memory_order_relaxed);
        });
    } else {
        // An online storage must not take that path: with the encoding table
        // loaded lazily, enumerate() resolves a file size per entry and each
        // resolve can fault in a CDN page — turning enumeration into minutes.
        // listFiles() reads paths straight from the root manifest and touches
        // no encoding table at all.
        auto paths = storage_->listFiles();
        for (auto& path : paths)
            consider(std::move(path));
    }

    std::sort(all_files_.begin(), all_files_.end());
    std::sort(d4_tex_entries_.begin(), d4_tex_entries_.end(),
              [](const CascD4TexEntry& a, const CascD4TexEntry& b) { return a.name < b.name; });
}

// ============================================================================
// File reading
// ============================================================================

CascFileResult CascService::readFile(const std::string& casc_path) {
    // An Overwatch texture is a header plus payload files that only its GUID
    // names, so it cannot be read as a plain file.
    if (is_ow_ && isTxtrPath(casc_path))
        return readTxtr(casc_path);

    auto data = storage_->readFile(casc_path);
    if (!data || data->empty())
        return {};

    CascFileResult result;
    result.name = casc_path;
    result.data = std::move(*data);
    return result;
}

CascFileResult CascService::readD4Tex(const std::string& meta_path) {
    // Build batch requests for meta, payload, and paylow in parallel.
    std::string payload_path = d4ReplaceSub(meta_path, "payload");
    std::string paylow_path = d4ReplaceSub(meta_path, "paylow");

    storages::casc::BatchReadRequest requests[3];
    requests[0].path = meta_path;
    requests[1].path = payload_path;
    requests[2].path = paylow_path;

    auto results = storage_->readBatch(requests);

    // Meta and payload are required; paylow is optional.
    if (!results[0].success || results[0].data.empty())
        return {};
    if (!results[1].success || results[1].data.empty())
        return {};

    CascFileResult result;
    result.name = d4DisplayName(meta_path);
    result.data = std::move(results[0].data);
    result.payload = std::move(results[1].data);
    if (results[2].success)
        result.paylow = std::move(results[2].data);
    result.is_d4_tex = true;
    return result;
}

CascFileResult CascService::readTxtr(const std::string& header_path) {
    auto header = storage_->readFile(header_path);
    if (!header || header->empty())
        return {};

    CascFileResult result;
    result.name = header_path;
    result.is_txtr = true;

    // The header declares how long its payload chain is; the GUIDs of the links
    // are derived from the texture's own, which the path ends with.  A texture
    // small enough to fit inside its header names none.
    const u64 guid = owTrailingGuid(header_path);
    const auto payload_guids =
        guid != 0 ? textures::txtr::Parser::payloadGuids(*header, guid) : std::vector<u64>{};

    if (!payload_guids.empty()) {
        std::vector<storages::casc::BatchReadRequest> requests(payload_guids.size());
        for (std::size_t i = 0; i < payload_guids.size(); ++i)
            requests[i].path = owSiblingPath(header_path, payload_guids[i]);

        auto results = storage_->readBatch(requests);
        result.payloads.resize(results.size());
        for (std::size_t i = 0; i < results.size(); ++i) {
            // A payload the install never downloaded is left empty rather than
            // failing the read: the parser then decodes the mips it does have.
            if (results[i].success)
                result.payloads[i] = std::move(results[i].data);
        }
    }

    result.data = std::move(*header);
    return result;
}

} // namespace whiteout::textool::services
