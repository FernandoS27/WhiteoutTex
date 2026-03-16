// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "casc_browser.h"

#include <algorithm>
#include <cstring>

#include <whiteout/sno/sno_types.h>

#include <imgui.h>

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
    for (int gid = 0; gid <= 180; ++gid) {
        auto g = static_cast<SnoGroup>(gid);
        const char* gname = sno::snoGroupName(g);
        if (gname && groupStr == gname)
            return g;
    }
    return SnoGroup::None;
}

} // anonymous namespace

namespace whiteout::gui {

// ============================================================================
// Folder dialog callback
// ============================================================================

void SDLCALL CascBrowser::folderDialogCallback(void* userdata, const char* const* filelist,
                                                 int /*filter*/) {
    if (!filelist || !filelist[0])
        return;
    auto* state = static_cast<FolderState*>(userdata);
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->has_pending.store(true);
}

// ============================================================================
// Open / folder helpers
// ============================================================================

void CascBrowser::open() {
    show_window_ = true;
}

void CascBrowser::processFolderResult() {
    if (!folder_state_.has_pending.load())
        return;
    std::lock_guard lock(folder_state_.mtx);
    copyToBuffer(storage_path_buf_, folder_state_.pending_path);
    folder_state_.has_pending.store(false);
}

// ============================================================================
// Static helpers
// ============================================================================

bool CascBrowser::isSupportedExtension(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = to_lower(name.substr(dot));
    for (const char* s : kSupportedExtensions)
        if (ext == s)
            return true;
    return false;
}

bool CascBrowser::hasVisibleAscii(const std::string& s) {
    for (unsigned char c : s)
        if (c > ' ' && c < 127)
            return true;
    return false;
}

// ============================================================================
// Storage open / close
// ============================================================================

void CascBrowser::resetState() {
    storage_.close();
    storage_open_ = false;
    is_d4_ = false;
    all_files_.clear();
    d4_tex_entries_.clear();
    combined_cache_.clear();
    d4_tex_format_hash_ = 0;
    root_ = {};
    product_name_.clear();
    file_count_ = 0;
}

void CascBrowser::openStorage() {
    const std::string path(storage_path_buf_);
    if (path.empty()) {
        status_ = "Please enter a CASC storage path.";
        return;
    }

    resetState();

    if (!storage_.open(path)) {
        status_ = "Failed to open CASC storage at: " + path;
        return;
    }

    if (auto prod = storage_.product())
        product_name_ = prod->codeName + " (build " + std::to_string(prod->buildNumber) + ")";
    if (auto count = storage_.totalFileCount())
        file_count_ = *count;

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

    status_ = "Enumerating files...";

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
                  [](const D4TexEntry& a, const D4TexEntry& b) { return a.name < b.name; });

        // Also load combined meta files for textures that live there.
        loadD4Textures();
    }

    // Enumerate regular files (WoW, D3, or any non-D4 textures).
    storage_.enumerate("", [&](const std::string& name) -> bool {
        if (isSupportedExtension(name))
            all_files_.push_back(name);
        return true;
    });
    std::sort(all_files_.begin(), all_files_.end());

    buildTree();
    storage_open_ = true;

    const size_t total = all_files_.size() + d4_tex_entries_.size();
    status_ = "Opened: " + std::to_string(total) + " supported textures found";
    if (is_d4_) {
        status_ += " (" + std::to_string(d4_tex_entries_.size()) + " D4 TEX + " +
                   std::to_string(all_files_.size()) + " other)";
    }
    status_ += " (of " + std::to_string(file_count_) + " total files).";
}

// ============================================================================
// D4 combined meta loading
// ============================================================================

void CascBrowser::loadD4Textures() {
    // Enumerate .dat files and identify Texture combined meta files.
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

    // Filter to Texture combined meta files.
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

    // Parse each combined meta file into the cache.
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

        // Walk data section.  Texture entries are 8-byte aligned with an
        // extra 8-byte skip before each entry.
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
// Tree construction
// ============================================================================

void CascBrowser::insertPathIntoTree(TreeNode& root, const std::string& file_path) {
    TreeNode* current = &root;
    std::string::size_type start = 0;
    while (start < file_path.size()) {
        auto sep = file_path.find_first_of("/\\", start);
        if (sep == std::string::npos)
            sep = file_path.size();

        const std::string segment = file_path.substr(start, sep - start);
        const bool is_last = (sep >= file_path.size());

        TreeNode* child = nullptr;
        for (auto& c : current->children) {
            if (c.name == segment) {
                child = &c;
                break;
            }
        }
        if (!child) {
            current->children.push_back({});
            child = &current->children.back();
            child->name = segment;
            if (is_last) {
                child->is_file = true;
                child->full_path = file_path;
            }
        }
        current = child;
        start = sep + 1;
    }
}

void CascBrowser::buildTree() {
    root_ = {};
    root_.name = "/";

    const std::string filter = to_lower(std::string(search_buf_));

    // Insert regular CASC files.
    for (const auto& path : all_files_) {
        if (!filter.empty() && to_lower(path).find(filter) == std::string::npos)
            continue;
        insertPathIntoTree(root_, path);
    }

    // Insert D4 TEX entries under a virtual "Diablo IV Textures" folder.
    if (d4_tex_entries_.empty())
        return;

    TreeNode* d4_folder = nullptr;
    for (auto& c : root_.children) {
        if (c.name == "Diablo IV Textures") {
            d4_folder = &c;
            break;
        }
    }
    if (!d4_folder) {
        root_.children.push_back({});
        d4_folder = &root_.children.back();
        d4_folder->name = "Diablo IV Textures";
    }

    for (const auto& entry : d4_tex_entries_) {
        const std::string display = entry.name + ".tex";
        if (!filter.empty() && to_lower(display).find(filter) == std::string::npos)
            continue;

        d4_folder->children.push_back({});
        auto& node = d4_folder->children.back();
        node.name = display;
        node.full_path = entry.name;
        node.sno_id = entry.sno_id;
        node.is_file = true;
    }
}

// ============================================================================
// Tree drawing
// ============================================================================

CascBrowserResult CascBrowser::drawTree(const TreeNode& node) {
    CascBrowserResult result;

    for (const auto& child : node.children) {
        if (child.is_file) {
            constexpr ImGuiTreeNodeFlags kLeafFlags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            ImGui::TreeNodeEx(child.name.c_str(), kLeafFlags);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                result = (child.sno_id >= 0) ? readD4Tex(child.full_path, child.sno_id)
                                              : readCascFile(child.full_path);
            }
            if (ImGui::IsItemHovered()) {
                if (child.sno_id >= 0)
                    ImGui::SetTooltip("D4 TEX: %s (snoId=%d)", child.full_path.c_str(),
                                      child.sno_id);
                else
                    ImGui::SetTooltip("%s", child.full_path.c_str());
            }
        } else {
            if (ImGui::TreeNode(child.name.c_str())) {
                auto child_result = drawTree(child);
                if (child_result)
                    result = std::move(child_result);
                ImGui::TreePop();
            }
        }
    }
    return result;
}

// ============================================================================
// CASC read — regular files
// ============================================================================

CascBrowserResult CascBrowser::readCascFile(const std::string& casc_path) {
    auto data = storage_.readFile(casc_path);
    if (!data || data->empty()) {
        status_ = "Failed to read: " + casc_path;
        return {};
    }

    CascBrowserResult result;
    result.name = casc_path;
    result.data = std::move(*data);
    status_ = "Loaded: " + casc_path;
    return result;
}

// ============================================================================
// CASC read — D4 TEX (meta + payload + paylow)
// ============================================================================

CascBrowserResult CascBrowser::readD4Tex(const std::string& name, i32 sno_id) {
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

    if (!meta || meta->empty()) {
        status_ = "Skipped (encrypted or unavailable): " + name;
        return {};
    }

    // --- Payload ---
    auto payload = storage_.readFile("base:payload\\" + std::to_string(sno_id));
    if (!payload || payload->empty()) {
        status_ = "Skipped (no payload, likely encrypted): " + name;
        return {};
    }
    stripSnoHeader(*payload);

    // --- Paylow (optional) ---
    std::vector<u8> paylow;
    if (auto plow = storage_.readFile("base:paylow\\" + std::to_string(sno_id))) {
        paylow = std::move(*plow);
        stripSnoHeader(paylow);
    }

    CascBrowserResult result;
    result.name = name + ".tex";
    result.data = std::move(*meta);
    result.payload = std::move(*payload);
    result.paylow = std::move(paylow);
    result.is_d4_tex = true;
    status_ = "Loaded D4 TEX: " + name;
    return result;
}

// ============================================================================
// Main draw
// ============================================================================

CascBrowserResult CascBrowser::draw(SDL_Window* window) {
    if (!show_window_)
        return {};

    processFolderResult();

    CascBrowserResult result;

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("CASC Browser", &show_window_)) {

        // ── Storage path row ───────────────────────────────────────────
        ImGui::Text("Storage Path:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160.0f);
        ImGui::InputText("##casc_path", storage_path_buf_, sizeof(storage_path_buf_));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            SDL_ShowOpenFolderDialog(folderDialogCallback, &folder_state_, window, nullptr, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            openStorage();
        }

        // ── Status bar ─────────────────────────────────────────────────
        if (!status_.empty()) {
            ImGui::TextWrapped("%s", status_.c_str());
        }

        if (!storage_open_) {
            ImGui::End();
            return {};
        }

        // ── Storage info ───────────────────────────────────────────────
        ImGui::Separator();
        if (!product_name_.empty()) {
            ImGui::Text("Product: %s", product_name_.c_str());
        }
        ImGui::Text("Textures: %zu", all_files_.size() + d4_tex_entries_.size());
        if (is_d4_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu D4 TEX)", d4_tex_entries_.size());
        }
        ImGui::Separator();

        // ── Search filter ──────────────────────────────────────────────
        ImGui::Text("Filter:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##casc_filter", search_buf_, sizeof(search_buf_))) {
            buildTree(); // Rebuild tree when filter changes.
        }

        // ── File tree ──────────────────────────────────────────────────
        ImGui::BeginChild("##casc_tree", ImVec2(0, 0), ImGuiChildFlags_Borders);
        result = drawTree(root_);
        ImGui::EndChild();
    }
    ImGui::End();

    return result;
}

} // namespace whiteout::gui
