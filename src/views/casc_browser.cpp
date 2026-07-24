// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "casc_browser.h"
#include "localization.h"
#include "preferences.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <imgui.h>

#ifdef __APPLE__
#include "macos_folder_dialog.h"
#endif

namespace whiteout::textool::views {

using namespace models;
using namespace services;
using i18n::tr;

// ============================================================================
// Online product / region tables
// ============================================================================

namespace {

struct OnlineProduct {
    const char* display_name;
    const char* product_code;
};

// Trimmed to retail + PTR per game family.  Beta variants and Overwatch
// (unsupported) were removed.
constexpr OnlineProduct kOnlineProducts[] = {
    {"World of Warcraft",         "wow"},
    {"WoW PTR",                   "wowt"},
    {"WoW Classic",               "wow_classic"},
    {"WoW Classic PTR",           "wow_classic_ptr"},
    {"Diablo III",                "d3"},
    {"Diablo III PTR",            "d3t"},
    {"Diablo IV",                 "fenris"},
    {"Diablo IV PTR",             "fenrisb"},
    {"Heroes of the Storm",       "hero"},
    {"Heroes of the Storm PTR",   "herot"},
    {"Warcraft III: Reforged",    "w3"},
    {"Warcraft III PTR",          "w3t"},
    {"StarCraft II",              "s2"},
    {"StarCraft: Remastered",     "s1"},
};

constexpr const char* kRegionCodes[] = {"us", "eu", "kr", "cn", "tw"};
constexpr const char* kRegionNameKeys[] = {
    "region.us", "region.eu", "region.kr", "region.cn", "region.tw"};

/// Returns a label for the given progress step.  Only the steps the library
/// actually emits on the online path (0, 2, 6) are listed; everything else
/// (including the synthetic post-open step 7) is mapped to a single honest
/// "downloading file list" message rather than a per-sub-step label that the
/// library never delivers.
const char* connectStepLabel(int8_t step) {
    switch (step) {
        case 0:  return tr("casc.step_loading_configs");
        case 2:  return tr("casc.step_loading_indexes");
        case 6:  return tr("casc.step_downloading_list");
        case 7:  return tr("casc.step_downloading_list");
        default: return tr("casc.step_contacting_cdn");
    }
}

/// Build (and ensure-existence-of) a per-CDN cache directory next to the
/// executable: `<exe>/cache/<product>_<region>`.  Returns "" if the path
/// can't be created.  Each CDN target gets its own subdirectory so
/// switching products/regions doesn't pollute one shared cache.
std::string onlineCacheDirFor(const char* product, const char* region) {
    const char* base = SDL_GetBasePath();
    if (!base)
        return {};
    std::filesystem::path dir = std::filesystem::path(base) / "cache" /
                                (std::string(product) + "_" + region);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {};
    return dir.string();
}

/// Render an indeterminate "knight-rider" bar: a fixed-width segment
/// slides back and forth across the bar.  Used when we have no real
/// fraction (total == 0) instead of ImGui::ProgressBar's default
/// percentage overlay, which is misleading on an oscillating sine.
void indeterminateBar(float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    if (width <= 0.0f)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg);

    const float seg = width * 0.25f;
    const float travel = width - seg;
    const float t = static_cast<float>(ImGui::GetTime());
    const float phase = 0.5f + 0.5f * std::sin(t * 1.6f);
    const float seg_x = phase * travel;
    dl->AddRectFilled(
        ImVec2(pos.x + seg_x, pos.y),
        ImVec2(pos.x + seg_x + seg, pos.y + height),
        fg);
    ImGui::Dummy(ImVec2(width, height));
}

} // namespace

// ============================================================================
// Dialog callbacks
// ============================================================================

void SDLCALL CascBrowser::folderDialogCallback(void* userdata, const char* const* filelist,
                                               i32 /*filter*/) {
    if (!filelist || !filelist[0])
        return;
    auto* state = static_cast<FolderState*>(userdata);
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->has_pending.store(true);
}

void SDLCALL CascBrowser::fileDialogCallback(void* userdata, const char* const* filelist,
                                             i32 /*filter*/) {
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
    consumeFolderResult(folder_state_, storage_path_buf_);
}

void CascBrowser::processFileResult() {
    consumeFolderResult(file_state_, storage_path_buf_);
}

// ============================================================================
// Storage open (delegates to CascService / MpqService)
// ============================================================================

void CascBrowser::openLocalStorage() {
    const std::string path(storage_path_buf_);

    // A directory → CASC; any regular file → archive (MPQ / WC3 map / SC2).
    const bool is_directory = std::filesystem::is_directory(path);

    casc_service_.close();
    mpq_service_.close();
    local_kind_ = LocalKind::None;
    product_name_.clear();

    if (!is_directory) {
        auto info = mpq_service_.openStorage(path);
        status_ = info.status;
        if (!mpq_service_.isOpen())
            return;
        product_name_ = info.archive_name;
        file_count_   = info.file_count;
        local_kind_   = LocalKind::Mpq;
    } else {
        loadListfileFromExeDir();
        auto info = casc_service_.openStorage(path);
        status_ = info.status;
        if (!casc_service_.isOpen())
            return;
        product_name_ = info.product_name;
        file_count_   = info.file_count;
        local_kind_   = LocalKind::Casc;
    }

    buildTree();
}

void CascBrowser::loadListfileFromExeDir() {
    const char* base = SDL_GetBasePath();
    if (!base)
        return;
    std::string lf_path = std::string(base) + "listfile.csv";
    std::ifstream lf(lf_path, std::ios::binary | std::ios::ate);
    if (!lf)
        return;
    const auto sz = static_cast<std::size_t>(lf.tellg());
    lf.seekg(0);
    std::vector<u8> data(sz);
    lf.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    if (lf)
        casc_service_.setListfile(std::move(data));
}

// ============================================================================
// Tree construction
// ============================================================================

void CascBrowser::insertPathIntoTree(TreeNode& root, const std::string& file_path) {
    TreeNode* current = &root;
    std::string::size_type start = 0;
    while (start < file_path.size()) {
        // ':' is a separator too: Warcraft III (and other TVFS roots) namespace
        // their content as `war3.w3mod:_hd.w3mod:units/human/...`, which would
        // otherwise collapse into one unreadable top-level segment.
        auto sep = file_path.find_first_of("/\\:", start);
        if (sep == std::string::npos)
            sep = file_path.size();

        if (sep > start) {
            const std::string segment = file_path.substr(start, sep - start);
            const bool is_last = (sep >= file_path.size());

            TreeNode* child = nullptr;
            for (auto& c : current->children) {
                if (c.name == segment && c.is_file == is_last) {
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
        }
        start = sep + 1;
    }
}

u32 CascBrowser::sortTree(TreeNode& node) {
    u32 count = 0;
    for (auto& child : node.children) {
        if (child.is_file)
            ++count;
        else
            count += sortTree(child);
    }
    node.total_files = count;

    // Folders first, then files; alphabetical (case-insensitive) within each
    // group.  Compared in place so huge listfile trees don't allocate per
    // comparison.
    std::sort(node.children.begin(), node.children.end(),
              [](const TreeNode& a, const TreeNode& b) {
                  if (a.is_file != b.is_file)
                      return !a.is_file;
                  const auto& x = a.name;
                  const auto& y = b.name;
                  const std::size_t n = std::min(x.size(), y.size());
                  for (std::size_t i = 0; i < n; ++i) {
                      const auto cx =
                          std::tolower(static_cast<unsigned char>(x[i]));
                      const auto cy =
                          std::tolower(static_cast<unsigned char>(y[i]));
                      if (cx != cy)
                          return cx < cy;
                  }
                  return x.size() < y.size();
              });
    return count;
}

void CascBrowser::buildTree() {
    root_ = {};
    root_.name = "/";

    const std::string filter = to_lower(std::string(search_buf_));
    auto_expand_ = !filter.empty();

    // ── MPQ branch ────────────────────────────────────────────────────
    if (local_kind_ == LocalKind::Mpq) {
        for (const auto& path : mpq_service_.files()) {
            if (!filter.empty() && to_lower(path).find(filter) == std::string::npos)
                continue;
            insertPathIntoTree(root_, path);
        }
        sortTree(root_);
        return;
    }

    // ── CASC branch (local or online) ─────────────────────────────────
    for (const auto& path : casc_service_.files()) {
        if (!filter.empty() && to_lower(path).find(filter) == std::string::npos)
            continue;
        insertPathIntoTree(root_, path);
    }

    // Insert D4 TEX entries under a virtual "Diablo IV Textures" folder.
    const auto& d4_entries = casc_service_.d4Entries();
    if (d4_entries.empty()) {
        sortTree(root_);
        return;
    }

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

    for (const auto& entry : d4_entries) {
        if (!filter.empty() && to_lower(entry.name).find(filter) == std::string::npos)
            continue;

        d4_folder->children.push_back({});
        auto& node = d4_folder->children.back();
        node.name = entry.name;
        node.full_path = entry.meta_path;
        node.is_d4_tex = true;
        node.is_file = true;
    }

    sortTree(root_);
}

// ============================================================================
// Tree drawing
// ============================================================================

std::vector<AppCommand> CascBrowser::drawTree(const TreeNode& node) {
    std::vector<AppCommand> commands;

    // `node.children` is pre-sorted by sortTree(): folders first, then files.
    int index = 0;
    for (const auto& child : node.children) {
        // Names are unique per kind, but a folder and a file can share one —
        // scope the ImGui ID by index so their states never collide.
        ImGui::PushID(index++);
        if (child.is_file) {
            constexpr ImGuiTreeNodeFlags kLeafFlags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_SpanAvailWidth;
            ImGui::TreeNodeEx(child.name.c_str(), kLeafFlags);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (local_kind_ == LocalKind::Mpq) {
                    // ── MPQ read ──
                    auto mpq_result = mpq_service_.readFile(child.full_path);
                    status_ = mpq_result ? ("Loaded: " + child.full_path)
                                         : ("Failed to read: " + child.full_path);
                    if (mpq_result) {
                        commands.push_back(
                            LoadCascTextureCmd{std::move(mpq_result.name),
                                               std::move(mpq_result.data),
                                               {}, {}, false});
                    }
                } else {
                    // ── CASC read ──
                    CascBrowserResult file_result;
                    if (child.is_d4_tex) {
                        file_result = casc_service_.readD4Tex(child.full_path);
                        status_ = file_result
                                      ? ("Loaded D4 TEX: " + child.name)
                                      : ("Skipped (encrypted or unavailable): " + child.name);
                    } else {
                        file_result = casc_service_.readFile(child.full_path);
                        status_ = file_result ? ("Loaded: " + child.full_path)
                                              : ("Failed to read: " + child.full_path);
                    }
                    if (file_result) {
                        commands.push_back(
                            LoadCascTextureCmd{std::move(file_result.name),
                                               std::move(file_result.data),
                                               std::move(file_result.payload),
                                               std::move(file_result.paylow),
                                               file_result.is_d4_tex});
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                if (child.is_d4_tex)
                    ImGui::SetTooltip("D4 TEX: %s", child.full_path.c_str());
                else
                    ImGui::SetTooltip("%s", child.full_path.c_str());
            }
        } else {
            // While a filter is typed, keep every surviving folder open so the
            // matches are visible without hand-expanding the hierarchy.
            if (auto_expand_)
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);

            const bool open = ImGui::TreeNodeEx(child.name.c_str(),
                                                ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::SameLine();
            ImGui::TextDisabled("(%u)", child.total_files);
            if (open) {
                auto child_cmds = drawTree(child);
                commands.insert(commands.end(), std::make_move_iterator(child_cmds.begin()),
                                std::make_move_iterator(child_cmds.end()));
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
    return commands;
}

// ============================================================================
// Main draw
// ============================================================================

std::vector<AppCommand> CascBrowser::draw(SDL_Window* window, RecentPaths& recent_paths) {
    if (!show_window_)
        return {};

    // ── Poll for completed async connect ───────────────────────────────
    if (auto result = casc_service_.pollConnect()) {
        status_ = result->status;
        if (casc_service_.isOpen()) {
            product_name_ = result->product_name;
            file_count_   = result->file_count;
            buildTree();
        }
    }

    // Pre-fill the storage path from the most recent entry if the buffer is empty.
    if (storage_path_buf_[0] == '\0' && !recent_paths.paths.empty())
        copyToBuffer(storage_path_buf_, recent_paths.paths.front());

    processFolderResult();
    processFileResult();

    std::vector<AppCommand> commands;
    const bool is_connecting = casc_service_.isConnecting();

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(tr("casc.window_title"), &show_window_)) {

        // ── Mode toggle (Local / Online) — disabled while connecting ───
        if (is_connecting)
            ImGui::BeginDisabled();

        const BrowserMode prev_mode = mode_;
        if (ImGui::RadioButton(tr("casc.mode_local"),  mode_ == BrowserMode::Local))
            mode_ = BrowserMode::Local;
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("casc.mode_online"), mode_ == BrowserMode::Online))
            mode_ = BrowserMode::Online;
        if (prev_mode != mode_) {
            casc_service_.close();
            mpq_service_.close();
            local_kind_ = LocalKind::None;
        }

        ImGui::Separator();

        if (mode_ == BrowserMode::Local) {
            // ── Local: storage path row ────────────────────────────────
            ImGui::TextUnformatted(tr("casc.storage_path"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputText("##casc_path", storage_path_buf_, sizeof(storage_path_buf_));

            // ── Buttons row ────────────────────────────────────────────
            if (ImGui::Button(tr("casc.browse_folder"))) {
#ifdef __APPLE__
                // SDL's NSOpenPanel wrapper doesn't expose
                // `treatsFilePackagesAsDirectories`, which blocks picking
                // folders inside `.app` bundles — Warcraft III on macOS
                // ships as `Warcraft III.app`, so we need to descend into it.
                ShowMacFolderDialogAllowingPackages(
                    folderDialogCallback, &folder_state_, window,
                    storage_path_buf_[0] ? storage_path_buf_ : nullptr, false);
#else
                SDL_ShowOpenFolderDialog(folderDialogCallback, &folder_state_, window,
                                         storage_path_buf_[0] ? storage_path_buf_ : nullptr, false);
#endif
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("casc.browse_archive"))) {
                static const SDL_DialogFileFilter kArchiveFilter[] = {
                    {"Blizzard Archives", "mpq;w3n;w3m;w3x;SC2Map;SC2Mod"},
                    {"All Files",         "*"},
                };
                SDL_ShowOpenFileDialog(fileDialogCallback, &file_state_, window,
                                       kArchiveFilter, 2,
                                       nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("casc.recent")) && !recent_paths.paths.empty())
                ImGui::OpenPopup("##recent_paths");
            if (ImGui::BeginPopup("##recent_paths")) {
                for (const auto& p : recent_paths.paths) {
                    if (ImGui::Selectable(p.c_str()))
                        copyToBuffer(storage_path_buf_, p);
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("casc.open"))) {
                openLocalStorage();
                if (casc_service_.isOpen() || mpq_service_.isOpen()) {
                    recent_paths.push(std::string(storage_path_buf_));
                }
            }
        } else {
            // ── Online: product + region row ───────────────────────────
            ImGui::TextUnformatted(tr("casc.product_label"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 260.0f);
            if (ImGui::BeginCombo("##casc_product",
                                  kOnlineProducts[selected_product_idx_].display_name,
                                  ImGuiComboFlags_HeightLarge)) {
                for (int i = 0; i < static_cast<int>(std::size(kOnlineProducts)); ++i) {
                    const bool selected = (selected_product_idx_ == i);
                    if (ImGui::Selectable(kOnlineProducts[i].display_name, selected))
                        selected_product_idx_ = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::BeginCombo("##casc_region", tr(kRegionNameKeys[selected_region_idx_]))) {
                for (int i = 0; i < static_cast<int>(std::size(kRegionCodes)); ++i) {
                    const bool selected = (selected_region_idx_ == i);
                    if (ImGui::Selectable(tr(kRegionNameKeys[i]), selected))
                        selected_region_idx_ = i;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("casc.connect"))) {
                loadListfileFromExeDir();
                status_.clear();
                const char* product = kOnlineProducts[selected_product_idx_].product_code;
                const char* region  = kRegionCodes[selected_region_idx_];
                casc_service_.startOnlineConnect(product, region,
                                                 onlineCacheDirFor(product, region));
            }
        }

        if (is_connecting)
            ImGui::EndDisabled();

        // ── Status bar ─────────────────────────────────────────────────
        if (!status_.empty()) {
            ImGui::TextWrapped("%s", status_.c_str());
        }

        // ── Storage info + tree (only when storage is open) ────────────
        if (casc_service_.isOpen() || mpq_service_.isOpen()) {
            ImGui::Separator();
            if (!product_name_.empty()) {
                ImGui::Text(tr("casc.product_value"), product_name_.c_str());
            }
            if (local_kind_ == LocalKind::Mpq) {
                ImGui::Text(tr("casc.textures_count"), mpq_service_.files().size());
            } else {
                ImGui::Text(tr("casc.textures_count"),
                            casc_service_.files().size() + casc_service_.d4Entries().size());
                if (casc_service_.isD4()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(tr("casc.d4_tex_count"), casc_service_.d4Entries().size());
                }
            }
            ImGui::Separator();

            // ── Search filter ──────────────────────────────────────────
            ImGui::TextUnformatted(tr("casc.filter_label"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##casc_filter", search_buf_, sizeof(search_buf_))) {
                buildTree();
            }

            // ── File tree ──────────────────────────────────────────────
            ImGui::BeginChild("##casc_tree", ImVec2(0, 0), ImGuiChildFlags_Borders);
            auto tree_cmds = drawTree(root_);
            commands.insert(commands.end(), std::make_move_iterator(tree_cmds.begin()),
                            std::make_move_iterator(tree_cmds.end()));
            ImGui::EndChild();
        }

        // ── Connecting progress modal ──────────────────────────────────
        if (is_connecting)
            ImGui::OpenPopup("##casc_connecting");

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
        constexpr ImGuiWindowFlags kModalFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::BeginPopupModal("##casc_connecting", nullptr, kModalFlags)) {
            const int8_t step    = casc_service_.connectStep();
            const u32    current = casc_service_.connectCurrent();
            const u32    total   = casc_service_.connectTotal();

            ImGui::TextUnformatted(tr("casc.connecting_title"));
            ImGui::Separator();
            ImGui::TextUnformatted(connectStepLabel(step));

            // The online path emits no current/total on any step the library
            // exposes today — total is always 0.  Use a knight-rider bar so
            // the indicator is honestly "working, indeterminate" instead of a
            // misleading sine percentage.
            if (total > 0) {
                float fraction = static_cast<float>(current) / static_cast<float>(total);
                ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
                ImGui::TextDisabled("%u / %u", current, total);
            } else {
                indeterminateBar(ImGui::GetTextLineHeight());
            }

            // Auto-close when the background thread finishes.
            if (!casc_service_.isConnecting())
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }
    ImGui::End();

    return commands;
}

} // namespace whiteout::textool::views
