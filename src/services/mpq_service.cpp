// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "format_registry.h"
#include "services/mpq_service.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace {

using namespace whiteout;

/// True if @p name ends in an extension of an archive-browsable texture format
/// (FmtCap::Archive in the format registry).  Case-insensitive.
bool isSupportedExtension(std::string_view name) {
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    std::string ext;
    for (char c : name.substr(dot))
        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return textures::formatHasCap(textures::classifyExtension(ext), textures::FmtCap::Archive);
}

} // anonymous namespace

namespace whiteout::textool::services {

// ============================================================================
// Open / Close
// ============================================================================

MpqStorageInfo MpqService::openStorage(const std::string& path) {
    MpqStorageInfo info;

    if (path.empty()) {
        info.status = "Please enter an MPQ archive path.";
        return info;
    }

    close();

    std::string error;
    auto result = storages::mpq::Storage::open(path, &error, threadPoolManager().get());
    if (!result) {
        info.status = "Failed to open MPQ archive: " + (error.empty() ? path : error);
        return info;
    }
    storage_ = std::move(*result);

    // Display name = filename only.
    info.archive_name = std::filesystem::path(path).filename().string();

    // Enumerate all known texture files via the embedded listfile.
    storage_->enumerate([&](const std::string& name) -> bool {
        if (isSupportedExtension(name))
            all_files_.push_back(name);
        return true;
    });

    std::sort(all_files_.begin(), all_files_.end());

    // If no textures found (no listfile or no texture entries), report that
    // clearly so the user knows the archive opened but has no recognized textures.
    storage_open_ = true;
    info.file_count = static_cast<u32>(all_files_.size());

    if (all_files_.empty()) {
        info.status = "Opened " + info.archive_name +
                      " but no supported texture files were found "
                      "(the archive may have no embedded listfile).";
    } else {
        info.status = "Opened: " + std::to_string(all_files_.size()) +
                      " texture(s) found in " + info.archive_name + ".";
    }
    return info;
}

void MpqService::close() {
    if (storage_)
        storage_->close();
    storage_.reset();
    storage_open_ = false;
    all_files_.clear();
}

// ============================================================================
// File reading
// ============================================================================

MpqFileResult MpqService::readFile(const std::string& name) {
    auto data = storage_->readFile(name);
    if (!data || data->empty())
        return {};

    MpqFileResult result;
    result.name = name;
    result.data = std::move(*data);
    return result;
}

} // namespace whiteout::textool::services
