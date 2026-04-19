// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/casc_service.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <span>

namespace {

using namespace whiteout;

/// Supported texture extensions (lowercase, with dot).
constexpr const char* kSupportedExtensions[] = {
    ".blp", ".bmp", ".dds", ".jpg", ".jpeg", ".png", ".tex", ".tga",
};

/// D4 enriched meta texture path prefix (after the root folder and colon).
constexpr std::string_view kD4MetaPrefix = ":meta\\Texture\\";

/// Returns true if @p name has a supported texture extension.
bool isSupportedExtension(std::string_view name) {
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    auto ext = name.substr(dot);
    // Case-insensitive compare against known extensions.
    for (const char* s : kSupportedExtensions) {
        std::string_view sv(s);
        if (ext.size() != sv.size())
            continue;
        bool match = true;
        for (size_t i = 0; i < ext.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(ext[i])) !=
                std::tolower(static_cast<unsigned char>(sv[i]))) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

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

} // anonymous namespace

namespace whiteout::textool::services {

// ============================================================================
// Open / Close
// ============================================================================

CascStorageInfo CascService::openStorage(const std::string& path) {
    CascStorageInfo info;

    if (path.empty()) {
        info.status = "Please enter a CASC storage path.";
        return info;
    }

    close();

    // Use OpenOptions with a memory cache for D4 combined meta performance,
    // and the global worker pool for parallel I/O.
    storages::casc::OpenOptions opts;
    opts.path = path;
    opts.memoryCacheSize = 256 * 1024 * 1024; // 256 MB
    opts.pool = threadPoolManager().get();
    if (!listfile_data_.empty())
        opts.listfile = std::span<const u8>(listfile_data_);

    auto result = storages::casc::Storage::open(opts);
    if (!result) {
        info.status = "Failed to open CASC storage at: " + path;
        return info;
    }
    storage_ = std::move(*result);

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
    // Join any pending background connect before clearing state.
    if (connect_thread_.joinable())
        connect_thread_.join();
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

void CascService::startOnlineConnect(const std::string& product, const std::string& region) {
    close(); // also joins any pending thread and clears state

    connect_step_.store(-1, std::memory_order_relaxed);
    connect_current_.store(0, std::memory_order_relaxed);
    connect_total_.store(0, std::memory_order_relaxed);
    is_connecting_.store(true, std::memory_order_release);

    connect_thread_ = std::thread([this, product, region] {
        auto info = doOpenOnline(product, region);
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

CascStorageInfo CascService::doOpenOnline(const std::string& product, const std::string& region) {
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
    if (!listfile_data_.empty())
        opts.listfile = std::span<const u8>(listfile_data_);
    opts.progressCallback = [this](storages::casc::ProgressStep step, u32 current, u32 total) -> bool {
        connect_step_.store(static_cast<int8_t>(step), std::memory_order_relaxed);
        connect_current_.store(current, std::memory_order_relaxed);
        connect_total_.store(total, std::memory_order_relaxed);
        return true;
    };

    auto result = storages::casc::Storage::openOnline(opts);
    if (!result) {
        info.status = "Failed to connect to CDN for product '" + product + "' (" + opts.region + ").";
        http_handler_.reset();
        return info;
    }
    storage_ = std::move(*result);

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
    storage_->enumerate([&](const storages::casc::EnumerateEntry& entry) -> bool {
        if (!isSupportedExtension(entry.path))
            return true;

        if (isD4MetaTexturePath(entry.path)) {
            is_d4_ = true;
            std::string meta_path{entry.path};
            d4_tex_entries_.push_back({d4DisplayName(entry.path), std::move(meta_path)});
        } else if (!isD4SubPath(entry.path)) {
            all_files_.emplace_back(entry.path);
        }
        return true;
    });

    std::sort(all_files_.begin(), all_files_.end());
    std::sort(d4_tex_entries_.begin(), d4_tex_entries_.end(),
              [](const CascD4TexEntry& a, const CascD4TexEntry& b) { return a.name < b.name; });
}

// ============================================================================
// File reading
// ============================================================================

CascFileResult CascService::readFile(const std::string& casc_path) {
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

} // namespace whiteout::textool::services
