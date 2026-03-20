// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "casc_browser.h"
#include "preferences.h"

#include <algorithm>

#include <imgui.h>

namespace whiteout::textool {

// ============================================================================
// Folder dialog callback
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

// ============================================================================
// Open / folder helpers
// ============================================================================

void CascBrowser::open() {
    show_window_ = true;
}

void CascBrowser::processFolderResult() {
    consumeFolderResult(folder_state_, storage_path_buf_);
}

// ============================================================================
// Storage open (delegates to CascService)
// ============================================================================

void CascBrowser::openStorage() {
    const std::string path(storage_path_buf_);

    auto info = casc_service_.openStorage(path);
    status_ = info.status;

    if (!casc_service_.isOpen())
        return;

    product_name_ = info.product_name;
    file_count_ = info.file_count;

    buildTree();
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
    for (const auto& path : casc_service_.files()) {
        if (!filter.empty() && to_lower(path).find(filter) == std::string::npos)
            continue;
        insertPathIntoTree(root_, path);
    }

    // Insert D4 TEX entries under a virtual "Diablo IV Textures" folder.
    const auto& d4_entries = casc_service_.d4Entries();
    if (d4_entries.empty())
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

    for (const auto& entry : d4_entries) {
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

std::vector<AppCommand> CascBrowser::drawTree(const TreeNode& node) {
    std::vector<AppCommand> commands;

    for (const auto& child : node.children) {
        if (child.is_file) {
            constexpr ImGuiTreeNodeFlags kLeafFlags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            ImGui::TreeNodeEx(child.name.c_str(), kLeafFlags);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                CascBrowserResult file_result;
                if (child.sno_id >= 0) {
                    file_result = casc_service_.readD4Tex(child.full_path, child.sno_id);
                    status_ = file_result
                                  ? ("Loaded D4 TEX: " + child.full_path)
                                  : ("Skipped (encrypted or unavailable): " + child.full_path);
                } else {
                    file_result = casc_service_.readFile(child.full_path);
                    status_ = file_result ? ("Loaded: " + child.full_path)
                                          : ("Failed to read: " + child.full_path);
                }
                if (file_result) {
                    commands.push_back(
                        LoadCascTextureCmd{std::move(file_result.name), std::move(file_result.data),
                                           std::move(file_result.payload),
                                           std::move(file_result.paylow), file_result.is_d4_tex});
                }
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
                auto child_cmds = drawTree(child);
                commands.insert(commands.end(), std::make_move_iterator(child_cmds.begin()),
                                std::make_move_iterator(child_cmds.end()));
                ImGui::TreePop();
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

    // Pre-fill the storage path from the most recent entry if the buffer is empty.
    if (storage_path_buf_[0] == '\0' && !recent_paths.paths.empty())
        copyToBuffer(storage_path_buf_, recent_paths.paths.front());

    processFolderResult();

    std::vector<AppCommand> commands;

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("CASC Browser", &show_window_)) {

        // ── Storage path row ───────────────────────────────────────────
        ImGui::Text("Storage Path:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160.0f);
        if (ImGui::BeginCombo("##casc_path", storage_path_buf_, ImGuiComboFlags_HeightLarge)) {
            for (const auto& p : recent_paths.paths) {
                const bool selected = (p == storage_path_buf_);
                if (ImGui::Selectable(p.c_str(), selected)) {
                    copyToBuffer(storage_path_buf_, p);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            SDL_ShowOpenFolderDialog(folderDialogCallback, &folder_state_, window,
                                     storage_path_buf_[0] ? storage_path_buf_ : nullptr, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            openStorage();
            if (casc_service_.isOpen()) {
                recent_paths.push(std::string(storage_path_buf_));
            }
        }

        // ── Status bar ─────────────────────────────────────────────────
        if (!status_.empty()) {
            ImGui::TextWrapped("%s", status_.c_str());
        }

        if (!casc_service_.isOpen()) {
            ImGui::End();
            return {};
        }

        // ── Storage info ───────────────────────────────────────────────
        ImGui::Separator();
        if (!product_name_.empty()) {
            ImGui::Text("Product: %s", product_name_.c_str());
        }
        ImGui::Text("Textures: %zu",
                    casc_service_.files().size() + casc_service_.d4Entries().size());
        if (casc_service_.isD4()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu D4 TEX)", casc_service_.d4Entries().size());
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
        auto tree_cmds = drawTree(root_);
        commands.insert(commands.end(), std::make_move_iterator(tree_cmds.begin()),
                        std::make_move_iterator(tree_cmds.end()));
        ImGui::EndChild();
    }
    ImGui::End();

    return commands;
}

} // namespace whiteout::textool
