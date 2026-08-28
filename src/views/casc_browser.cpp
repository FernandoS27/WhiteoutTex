// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "casc_browser.h"
#include "localization.h"
#include "preferences.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string_view>

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

// Trimmed to retail + PTR per game family; beta variants were removed.
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
    {"Overwatch 2",               "pro"},
    {"Overwatch 2 PTR",           "prot"},
};

constexpr const char* kRegionCodes[] = {"us", "eu", "kr", "cn", "tw"};
constexpr const char* kRegionNameKeys[] = {
    "region.us", "region.eu", "region.kr", "region.cn", "region.tw"};

/// Returns a label for the given connect step.  The library's ten steps are
/// folded into the four phases a user can act on; the synthetic post-open step
/// (where a LoadOnDemand storage actually fetches its tables) gets the
/// file-list label, since that is what the wait is for.
///
/// A local open runs the same steps minus the network, so it shares the
/// mapping — only the phases that name the CDN need a folder-flavoured
/// wording, and Ready means "now scanning the root", not "downloading".
const char* connectStepLabel(i32 step, bool local) {
    using Step = storages::casc::ProgressStep;
    if (step == services::kConnectStepEnumerate)
        return tr("casc.step_scanning_files");
    if (step == services::kConnectStepFileList)
        return tr("casc.step_downloading_list");
    if (step < 0)
        return local ? tr("casc.step_opening_storage") : tr("casc.step_contacting_cdn");

    switch (static_cast<Step>(step)) {
    case Step::ResolvingVersion:
        return tr("casc.step_contacting_cdn");
    case Step::LoadingBuildConfig:
    case Step::LoadingCdnConfig:
        return tr("casc.step_loading_configs");
    case Step::LoadingIndexFiles:
    case Step::MappingArchives:
    case Step::LoadingArchiveIndexes:
        return tr("casc.step_loading_indexes");
    case Step::LoadingEncodingTable:
    case Step::LoadingVfsManifests:
    case Step::LoadingRootManifest:
        return tr("casc.step_loading_manifests");
    case Step::Ready:
        return local ? tr("casc.step_scanning_files") : tr("casc.step_downloading_list");
    }
    return local ? tr("casc.step_opening_storage") : tr("casc.step_contacting_cdn");
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

/// ASCII lowercase lookup.
///
/// std::tolower is a locale-aware function call the compiler can neither inline
/// nor hoist, and the tree comparator below reaches it tens of millions of
/// times on a full game storage.  Swapping it for this table is what takes the
/// Overwatch tree sort from 950 ms to 155 ms.
constexpr std::array<unsigned char, 256> makeLowerTable() {
    std::array<unsigned char, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = static_cast<unsigned char>((i >= 'A' && i <= 'Z') ? i + 32 : i);
    return table;
}
constexpr auto kLower = makeLowerTable();

inline unsigned char lowerOf(char c) noexcept {
    return kLower[static_cast<unsigned char>(c)];
}

/// True when @p haystack contains @p needle, ignoring case.  @p needle must
/// already be lowercase.
///
/// Unlike to_lower(haystack).find(needle) this allocates nothing, which matters
/// because the filter runs over every path in the storage on each keystroke —
/// 1.2 million of them on Overwatch.
bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return true;
    if (haystack.size() < needle.size())
        return false;
    const auto last = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lowerOf(haystack[i + j]) == needle[j])
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
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
        buildTree();
        return;
    }

    // A CASC directory takes seconds to open and scan, so it runs on the
    // connect thread and lands back in draw() through pollConnect().  The
    // pending path is what tells that handler the result was a local one.
    loadListfileFromExeDir();
    status_.clear();
    root_ = {};
    root_.name = "/";
    pending_local_path_ = path;
    casc_service_.startLocalOpen(path);
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
            // A view, not a copy: every path here is split into segments and
            // almost all of them only ever get compared against a node that
            // already exists.  Only the segment that becomes a new node is
            // materialised.
            const std::string_view segment(file_path.data() + start, sep - start);
            const bool is_last = (sep >= file_path.size());

            // Paths arrive sorted, so the segment continuing the previous one
            // is this node's last child.
            TreeNode* child = nullptr;
            if (!current->children.empty() && current->children.back().name == segment &&
                current->children.back().is_file == is_last) {
                child = &current->children.back();
            } else if (!is_last) {
                // A directory can still be further back when a file sorted
                // between two of its entries.  Directories per level are few,
                // so this scan stays cheap; file leaves skip it, and that is
                // what keeps a 390,000-entry folder from being quadratic.
                for (auto& c : current->children) {
                    if (c.name == segment && !c.is_file) {
                        child = &c;
                        break;
                    }
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
    // comparison, and through the lookup table rather than std::tolower —
    // this comparator runs a few million times on a full game storage.
    std::sort(node.children.begin(), node.children.end(),
              [](const TreeNode& a, const TreeNode& b) {
                  if (a.is_file != b.is_file)
                      return !a.is_file;
                  const auto& x = a.name;
                  const auto& y = b.name;
                  const std::size_t n = std::min(x.size(), y.size());
                  for (std::size_t i = 0; i < n; ++i) {
                      const auto cx = lowerOf(x[i]);
                      const auto cy = lowerOf(y[i]);
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
            if (!containsNoCase(path, filter))
                continue;
            insertPathIntoTree(root_, path);
        }
        sortTree(root_);
        return;
    }

    // ── CASC branch (local or online) ─────────────────────────────────
    for (const auto& path : casc_service_.files()) {
        if (!containsNoCase(path, filter))
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
        if (!containsNoCase(entry.name, filter))
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

/// Draw one file row and, on a double click, read it and emit the load command.
void CascBrowser::drawFileNode(const TreeNode& child, std::vector<AppCommand>& commands) {
    constexpr ImGuiTreeNodeFlags kLeafFlags = ImGuiTreeNodeFlags_Leaf |
                                              ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                              ImGuiTreeNodeFlags_SpanAvailWidth;
    ImGui::TreeNodeEx(child.name.c_str(), kLeafFlags);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        if (local_kind_ == LocalKind::Mpq) {
            // ── MPQ read ──
            auto mpq_result = mpq_service_.readFile(child.full_path);
            status_ = mpq_result ? ("Loaded: " + child.full_path)
                                 : ("Failed to read: " + child.full_path);
            if (mpq_result) {
                commands.push_back(LoadCascTextureCmd{std::move(mpq_result.name),
                                                      std::move(mpq_result.data),
                                                      {}, {}, {}, false, false});
            }
        } else {
            // ── CASC read ──
            CascBrowserResult file_result;
            if (child.is_d4_tex) {
                file_result = casc_service_.readD4Tex(child.full_path);
                status_ = file_result ? ("Loaded D4 TEX: " + child.name)
                                      : ("Skipped (encrypted or unavailable): " + child.name);
            } else {
                file_result = casc_service_.readFile(child.full_path);
                status_ = file_result ? ("Loaded: " + child.full_path)
                                      : ("Failed to read: " + child.full_path);
            }
            if (file_result) {
                commands.push_back(LoadCascTextureCmd{std::move(file_result.name),
                                                      std::move(file_result.data),
                                                      std::move(file_result.payload),
                                                      std::move(file_result.paylow),
                                                      std::move(file_result.payloads),
                                                      file_result.is_d4_tex,
                                                      file_result.is_txtr});
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        if (child.is_d4_tex)
            ImGui::SetTooltip("D4 TEX: %s", child.full_path.c_str());
        else
            ImGui::SetTooltip("%s", child.full_path.c_str());
    }
}

std::vector<AppCommand> CascBrowser::drawTree(const TreeNode& node) {
    std::vector<AppCommand> commands;

    // `node.children` is pre-sorted by sortTree(): folders first, then files.
    const auto files_begin =
        std::partition_point(node.children.begin(), node.children.end(),
                             [](const TreeNode& c) { return !c.is_file; });

    // ── Folders ───────────────────────────────────────────────────────
    // Names are unique per kind, but a folder and a file can share one — scope
    // the ImGui ID by index so their states never collide.  The index runs
    // across both groups, so a file's id is its position in `children`.
    int index = 0;
    for (auto it = node.children.begin(); it != files_begin; ++it, ++index) {
        ImGui::PushID(index);
        // While a filter is typed, keep every surviving folder open so the
        // matches are visible without hand-expanding the hierarchy.
        if (auto_expand_)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        const bool open =
            ImGui::TreeNodeEx(it->name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine();
        ImGui::TextDisabled("(%u)", it->total_files);
        if (open) {
            auto child_cmds = drawTree(*it);
            commands.insert(commands.end(), std::make_move_iterator(child_cmds.begin()),
                            std::make_move_iterator(child_cmds.end()));
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ── Files ─────────────────────────────────────────────────────────
    // Every file row is one line tall, so the run can be clipped to what is on
    // screen.  Overwatch puts 390,000 of them under a single manifest folder;
    // submitting them all each frame is not survivable.  Folders are drawn
    // above, and each recursion finishes before this clipper starts, so no two
    // clippers are ever active at once.
    const int file_count = static_cast<int>(node.children.end() - files_begin);
    if (file_count > 0) {
        ImGuiListClipper clipper;
        clipper.Begin(file_count);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::PushID(index + i);
                drawFileNode(files_begin[i], commands);
                ImGui::PopID();
            }
        }
    }
    return commands;
}

// ============================================================================
// Main draw
// ============================================================================

std::vector<AppCommand> CascBrowser::draw(SDL_Window* window, RecentPaths& recent_paths) {
    if (!show_window_)
        return {};

    // ── Poll for a completed async open (local folder or CDN) ──────────
    if (auto result = casc_service_.pollConnect()) {
        status_ = result->status;
        const bool opened = casc_service_.isOpen();
        if (opened) {
            product_name_ = result->product_name;
            file_count_   = result->file_count;
            if (!pending_local_path_.empty())
                local_kind_ = LocalKind::Casc;
            buildTree();
        }
        if (!pending_local_path_.empty()) {
            // Only a folder that actually opened is worth offering again.
            if (opened)
                recent_paths.push(pending_local_path_);
            pending_local_path_.clear();
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
                // A CASC folder opens asynchronously; its path is remembered
                // when the open lands.  An archive is already done here.
                if (mpq_service_.isOpen())
                    recent_paths.push(std::string(storage_path_buf_));
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
            const bool local  = casc_service_.isLocalOpen();
            const i32 step    = casc_service_.connectStep();
            const u64 current = casc_service_.connectCurrent();
            const u64 total   = casc_service_.connectTotal();

            ImGui::TextUnformatted(local ? tr("casc.opening_title")
                                         : tr("casc.connecting_title"));
            ImGui::Separator();
            ImGui::TextUnformatted(connectStepLabel(step, local));

            // Not every step can count its work; when one can't, use a
            // knight-rider bar so the indicator is honestly "working,
            // indeterminate" instead of a misleading percentage.
            if (total > 0) {
                float fraction = static_cast<float>(current) / static_cast<float>(total);
                ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
                ImGui::TextDisabled("%llu / %llu", static_cast<unsigned long long>(current),
                                    static_cast<unsigned long long>(total));
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
