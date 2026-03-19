// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/casc_service.h"

#include <algorithm>
#include <cstring>

#include <whiteout/sno/sno_types.h>

namespace {

using namespace whiteout;
using sno::SnoGroup;
using sno::kSnoMagic;

/// Supported texture extensions (lowercase, with dot).
constexpr const char* kSupportedExtensions[] = {
    ".blp", ".bmp", ".dds", ".jpg", ".jpeg", ".png", ".tex", ".tga",
};

/// Signature of D4 combined meta files.
constexpr u32 kCombinedMetaMagic = 0x44CF00F5;

/// Data alignment within combined meta files.
constexpr size_t kCombinedAlignment = 8;

/// Strip a 16-byte SNO header from @p buf if present.
static void stripSnoHeader(std::vector<u8>& buf) {
    if (buf.size() > 16) {
        u32 magic = 0;
        std::memcpy(&magic, buf.data(), 4);
        if (magic == kSnoMagic)
            buf.erase(buf.begin(), buf.begin() + 16);
    }
}

/// Build a 16-byte synthetic SNO header prepended to @p data.
static std::vector<u8> buildSyntheticSnoHeader(u32 formatHash, const u8* data, size_t size) {
    std::vector<u8> result(16 + size);
    u32 magic = kSnoMagic;
    u32 zero = 0;
    std::memcpy(result.data() + 0, &magic, 4);
    std::memcpy(result.data() + 4, &formatHash, 4);
    std::memcpy(result.data() + 8, &zero, 4);
    std::memcpy(result.data() + 12, &zero, 4);
    std::memcpy(result.data() + 16, data, size);
    return result;
}

/// Return true if @p fileName looks like an encrypted combined meta variant.
static bool isEncryptedDatFile(const std::string& fileName) {
    auto dot = fileName.rfind('.');
    std::string base = (dot != std::string::npos) ? fileName.substr(0, dot) : fileName;
    auto last_dash = base.rfind('-');
    if (last_dash == std::string::npos)
        return false;
    std::string suffix = base.substr(last_dash + 1);
    return suffix.size() > 2 && suffix[0] == '0' && suffix[1] == 'x';
}

/// Extract the bare filename from a path.
static std::string baseName(const std::string& path) {
    auto sep = path.find_last_of("\\/:");
    return (sep != std::string::npos) ? path.substr(sep + 1) : path;
}

/// Resolve SnoGroup from a combined meta file name.
static SnoGroup groupFromCombinedFileName(const std::string& path) {
    std::string name = baseName(path);
    auto dash = name.find('-');
    if (dash == std::string::npos)
        return SnoGroup::None;
    std::string groupStr = name.substr(0, dash);
    for (i32 gid = 0; gid <= 180; ++gid) {
        auto g = static_cast<SnoGroup>(gid);
        const char* gname = sno::snoGroupName(g);
        if (gname && groupStr == gname)
            return g;
    }
    return SnoGroup::None;
}

/// Returns true if @p name has a supported texture extension.
static bool isSupportedExtension(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext;
    ext.reserve(name.size() - dot);
    for (size_t i = dot; i < name.size(); ++i)
        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    for (const char* s : kSupportedExtensions)
        if (ext == s)
            return true;
    return false;
}

/// Returns true if @p s contains at least one visible ASCII character.
static bool hasVisibleAscii(const std::string& s) {
    for (unsigned char c : s)
        if (c > ' ' && c < 127)
            return true;
    return false;
}

} // anonymous namespace

namespace whiteout::gui {

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

    if (!storage_.open(path)) {
        info.status = "Failed to open CASC storage at: " + path;
        return info;
    }

    if (auto prod = storage_.product())
        info.product_name = prod->codeName + " (build " + std::to_string(prod->buildNumber) + ")";
    if (auto count = storage_.totalFileCount()) {
        info.file_count = *count;
    }

    // Detect D4 by trying to read CoreTOC.dat.
    sno::CoreToc toc;
    for (const char* toc_path : {"base:CoreTOC.dat", "CoreTOC.dat"}) {
        if (auto toc_data = storage_.readFile(toc_path)) {
            if (toc.parse(*toc_data)) {
                auto fmt = toc.format();
                is_d4_ = (fmt == sno::CoreTocFormat::D4Old ||
                           fmt == sno::CoreTocFormat::D4New);
            }
            break;
        }
    }
    info.is_d4 = is_d4_;

    if (is_d4_) {
        // Cache the Texture group's format hash for synthetic SNO headers.
        auto& fh_map = toc.formatHashes();
        auto it = fh_map.find(static_cast<i32>(SnoGroup::Texture));
        if (it != fh_map.end())
            d4_tex_format_hash_ = it->second;

        // Collect valid D4 Texture entries from the CoreTOC.
        for (const auto& entry : toc.entriesForGroup(SnoGroup::Texture)) {
            if (hasVisibleAscii(entry.name))
                d4_tex_entries_.push_back({entry.name, entry.snoId});
        }
        std::sort(d4_tex_entries_.begin(), d4_tex_entries_.end(),
                  [](const CascD4TexEntry& a, const CascD4TexEntry& b) {
                      return a.name < b.name;
                  });

        loadD4Textures(toc);
    }

    // Enumerate regular files (WoW, D3, or any non-D4 textures).
    storage_.enumerate("", [&](const std::string& name) -> bool {
        if (isSupportedExtension(name))
            all_files_.push_back(name);
        return true;
    });
    std::sort(all_files_.begin(), all_files_.end());

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
    storage_.close();
    storage_open_ = false;
    is_d4_ = false;
    all_files_.clear();
    d4_tex_entries_.clear();
    combined_cache_.clear();
    d4_tex_format_hash_ = 0;
}

// ============================================================================
// D4 combined meta loading
// ============================================================================

void CascService::loadD4Textures(sno::CoreToc& toc) {
    std::vector<std::string> dat_files;
    for (const char* pattern : {"*.dat", "base:*.dat", "base\\*.dat"}) {
        if (!dat_files.empty())
            break;
        storage_.enumerate(pattern, "",
                           [&](const std::string& name) -> bool {
                               dat_files.push_back(name);
                               return true;
                           });
    }

    std::vector<std::string> tex_combined;
    for (const auto& path : dat_files) {
        std::string fname = baseName(path);
        if (std::count(fname.begin(), fname.end(), '-') < 2)
            continue;
        if (isEncryptedDatFile(fname))
            continue;
        if (groupFromCombinedFileName(path) == SnoGroup::Texture)
            tex_combined.push_back(path);
    }

    for (const auto& path : tex_combined) {
        auto file_data = storage_.readFile(path);
        if (!file_data || file_data->size() < 8)
            continue;

        auto shared = std::make_shared<std::vector<u8>>(std::move(*file_data));
        const auto& buf = *shared;

        u32 sig = 0;
        std::memcpy(&sig, buf.data(), 4);
        if (sig != kCombinedMetaMagic)
            continue;

        u32 entry_count = 0;
        std::memcpy(&entry_count, buf.data() + 4, 4);

        const size_t index_end = 8 + static_cast<size_t>(entry_count) * 8;
        if (index_end > buf.size())
            continue;

        struct IndexEntry { i32 sno_id; u32 size; };
        std::vector<IndexEntry> index(entry_count);
        for (u32 i = 0; i < entry_count; ++i) {
            const size_t off = 8 + static_cast<size_t>(i) * 8;
            std::memcpy(&index[i].sno_id, buf.data() + off, 4);
            std::memcpy(&index[i].size, buf.data() + off + 4, 4);
        }

        size_t pos = index_end;
        for (u32 i = 0; i < entry_count; ++i) {
            pos = (pos + kCombinedAlignment - 1) & ~(kCombinedAlignment - 1);
            pos += 8; // Texture group extra skip

            if (pos + index[i].size > buf.size())
                break;

            i32 check_id = 0;
            std::memcpy(&check_id, buf.data() + pos, 4);
            if (check_id != index[i].sno_id) {
                pos += index[i].size;
                continue;
            }

            combined_cache_[index[i].sno_id] = {shared, pos, index[i].size};
            pos += index[i].size;
        }
    }
}

// ============================================================================
// File reading
// ============================================================================

CascFileResult CascService::readFile(const std::string& casc_path) {
    auto data = storage_.readFile(casc_path);
    if (!data || data->empty())
        return {};

    CascFileResult result;
    result.name = casc_path;
    result.data = std::move(*data);
    return result;
}

CascFileResult CascService::readD4Tex(const std::string& name, i32 sno_id) {
    // --- Meta ---
    std::optional<std::vector<u8>> meta;
    const char* dir = sno::snoGroupDir(SnoGroup::Texture);
    const char* ext = sno::snoGroupExtension(SnoGroup::Texture);

    if (dir && ext)
        meta = storage_.readFile(std::string("Base\\meta\\") + dir + "\\" + name + "." + ext);
    if (!meta)
        meta = storage_.readFile("base:meta\\" + std::to_string(sno_id));

    // Fallback: build a synthetic SNO from the combined meta cache.
    if (!meta) {
        auto it = combined_cache_.find(sno_id);
        if (it != combined_cache_.end()) {
            auto& ce = it->second;
            meta = buildSyntheticSnoHeader(d4_tex_format_hash_,
                                           ce.file_data->data() + ce.data_offset,
                                           ce.data_size);
        }
    }

    if (!meta || meta->empty())
        return {};

    // --- Payload ---
    auto payload = storage_.readFile("base:payload\\" + std::to_string(sno_id));
    if (!payload || payload->empty())
        return {};
    stripSnoHeader(*payload);

    // --- Paylow (optional) ---
    std::vector<u8> paylow;
    if (auto plow = storage_.readFile("base:paylow\\" + std::to_string(sno_id))) {
        paylow = std::move(*plow);
        stripSnoHeader(paylow);
    }

    CascFileResult result;
    result.name = name + ".tex";
    result.data = std::move(*meta);
    result.payload = std::move(*payload);
    result.paylow = std::move(paylow);
    result.is_d4_tex = true;
    return result;
}

} // namespace whiteout::gui
