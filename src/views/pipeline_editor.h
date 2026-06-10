// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <unordered_set>

#include "common_types.h"
#include "pipeline/node_graph.h"

struct SDL_Window;

namespace ax::NodeEditor {
struct EditorContext;
} // namespace ax::NodeEditor

namespace whiteout::textool::views {

/// One selectable AI upscaler model for the Upscale node's model combobox.
/// `id` is the stable value stored in the param (the model file stem); `label`
/// is the human-readable combobox entry.
struct ModelOption {
    std::string id;
    std::string label;
};

/// Hosts the node-graph canvas for the Pipelines tab.
///
/// Owns the imgui-node-editor EditorContext and the pipeline::NodeGraph model.
/// Graph state is persisted by WhiteoutTex (model -> JSON), NOT by the
/// node-editor library, so the library's settings file is disabled and node
/// positions are pushed from / read back into the model each frame.
class PipelineEditor {
public:
    PipelineEditor();
    ~PipelineEditor();

    PipelineEditor(const PipelineEditor&) = delete;
    PipelineEditor& operator=(const PipelineEditor&) = delete;

    /// Draw the canvas.  Must be called inside an ImGui child/window/tab.
    /// @param window  Parent window for the save/load file dialogs.
    void draw(SDL_Window* window);

    pipeline::NodeGraph& graph() noexcept { return graph_; }

    /// Provide the AI upscaler models shown by the Upscale node's combobox.
    void setUpscalerModels(std::vector<ModelOption> models) {
        upscaler_models_ = std::move(models);
    }

    /// Provide the runnable pipeline file names shown by the Subpipeline node's
    /// combobox.
    void setPipelines(std::vector<std::string> pipelines) {
        pipelines_ = std::move(pipelines);
    }

    /// Spawn a node of @p type_id at a cascading default position (used by a
    /// palette click).  Returns the new node id, or 0 if the type is unknown.
    pipeline::NodeId spawnNode(const char* type_id);

    /// Spawn a node of @p type_id at an explicit canvas position (used by a
    /// palette drag-and-drop).  Returns the new node id, or 0 if unknown.
    pipeline::NodeId spawnNodeAt(const char* type_id, pipeline::Vec2 pos);

private:
    // Seed a small demo graph the first time the canvas is drawn (the registry
    // is only populated after construction, so this can't run in the ctor).
    // Temporary until graph load/save is wired into the UI.
    void seedDemo();

    // Node Palette: a Save/Load toolbar over a panel listing registered node
    // types (grouped by category); click an entry to spawn it onto the canvas.
    void drawPalette(f32 width, SDL_Window* window);

    // Open the save/load file dialogs, and consume their async results
    // (serialize / deserialize the graph as JSON).
    void requestSave(SDL_Window* window);
    void requestLoad(SDL_Window* window);
    void processDialogs();
    void savePipeline(const std::string& path);
    bool loadPipeline(const std::string& path);

    // Render the model's nodes + links and handle create/delete interaction.
    void drawNodes();
    void drawLinks();
    void handleCreate();
    void handleDelete();
    void syncPositions();

    ax::NodeEditor::EditorContext* context_ = nullptr;
    pipeline::NodeGraph graph_;

    // Nodes whose initial position has been pushed into the editor.  Tracked so
    // SetNodePosition runs once per node (subsequent frames let the user drag).
    std::unordered_set<pipeline::NodeId> placed_;
    bool seeded_ = false;

    // Async results of the save / load file dialogs (filled on a callback that
    // may run on another thread — hence the mutex/atomic in FolderState).
    FolderState save_dialog_result_;
    FolderState load_dialog_result_;

    std::vector<ModelOption> upscaler_models_;
    std::vector<std::string> pipelines_; ///< Pipeline file names for Subpipeline nodes.

    // Pending position for the next spawned node (view-space), so palette spawns
    // don't stack on top of each other.
    pipeline::Vec2 next_spawn_pos_{40.0f, 40.0f};
};

} // namespace whiteout::textool::views
