// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file pipeline_debug_dialog.h
/// @brief Debug-run dialog for the pipeline editor.  Collects a value for each
///        of the graph's input ports (image via file picker, scalar via a
///        field) and shows every output port's value (numeric or image preview).

#include <string>
#include <vector>

#include "common_types.h"
#include "models/app_state.h"
#include "pipeline/node_graph.h"
#include "services/pipeline_executor.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace whiteout::textool::views {

/// True for image-carrying pin types (provided via a file picker in debug).
inline bool isImagePinType(pipeline::PinType t) {
    return t == pipeline::PinType::RGBA || t == pipeline::PinType::RGB ||
           t == pipeline::PinType::R;
}

/// One input port to fill before a debug run.
struct DebugInputRow {
    std::string name;
    pipeline::PinType type = pipeline::PinType::Real;
    std::string path;   ///< Source image file (image types).
    f64 real = 0.0;     ///< Value (Real / Number).
    i64 integer = 0;    ///< Value (Integer).
};

/// Modal that drives a debug run of the editor's current graph.  Collects inputs
/// and shows results; the App performs the actual run and reports back.
class PipelineDebugDialog {
public:
    /// Populate from a graph's interface inputs and show the dialog.
    void open(std::string display_name, const pipeline::PipelineInterface& iface);
    bool isOpen() const { return open_; }
    void close();

    /// Render the modal (call every frame).  @p renderer backs image previews.
    void draw(SDL_Window* window, SDL_Renderer* renderer);

    /// Returns true exactly once after the user clicks Run.
    bool takeRunRequest();

    const std::vector<DebugInputRow>& inputs() const { return inputs_; }
    /// Report the outcome of a run back to the dialog (for display).
    void setResult(services::PipelineDebugResult result);

private:
    void consumeDialogResult();
    void releasePreviews();

    bool open_ = false;
    bool show_requested_ = false;
    bool run_requested_ = false;
    std::string display_name_;
    std::vector<DebugInputRow> inputs_;

    bool has_result_ = false;
    services::PipelineDebugResult result_;
    std::vector<SDL_Texture*> previews_; ///< Parallel to result_.outputs (null if not an image).
    bool previews_built_ = false;

    models::FileDialogState dialog_state_;
    int active_input_ = -1; ///< Index of the image input currently picking a file.
};

} // namespace whiteout::textool::views
