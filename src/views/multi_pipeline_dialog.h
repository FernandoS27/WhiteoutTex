// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file multi_pipeline_dialog.h
/// @brief Dialog that runs a Varying pipeline: each image Input is mapped to a
///        source file and each image Output to a save file (with per-output
///        format-specific options).  Execution + saving is driven by App.

#include <string>
#include <vector>

#include "common_types.h"
#include "models/app_state.h"
#include "preferences.h"

#include <whiteout/textures/texture.h>

struct SDL_Window;

namespace whiteout::textool::views {

/// One image input port mapped to a source file.
struct MultiInputRow {
    std::string port; ///< Input node name (the bound port).
    std::string path; ///< Chosen source image file.
};

/// One image output port mapped to a destination file plus save options.
struct MultiOutputRow {
    std::string port; ///< Output node name (the captured port).
    std::string path; ///< Chosen destination file.
    SavePrefs prefs;  ///< Per-format encoding + mipmap options.
    i32 kind = 0;     ///< TextureKind value to tag the saved image with.
};

/// Modal for running a multi-input/output (Varying) pipeline against files.
/// The dialog only collects paths/options and signals a run request; the App
/// performs the load/execute/save and reports back via finishRun().
class MultiPipelineDialog {
public:
    /// Populate the dialog from a pipeline's image ports and show it.
    void open(std::string file, std::string display_name,
              const std::vector<std::string>& image_inputs,
              const std::vector<std::string>& image_outputs, const SavePrefs& default_prefs);

    bool isOpen() const { return open_; }
    void close();

    /// Render the modal (call every frame).  @p window backs the OS file dialogs.
    void draw(SDL_Window* window);

    /// Returns true exactly once after the user clicks Run with all paths set.
    bool takeRunRequest();

    const std::string& file() const { return file_; }
    const std::vector<MultiInputRow>& inputs() const { return inputs_; }
    const std::vector<MultiOutputRow>& outputs() const { return outputs_; }

    /// Report the outcome of a run back to the dialog (shown to the user).
    void finishRun(std::string message, bool ok);

private:
    void consumeDialogResult();
    bool allPathsSet() const;

    bool open_ = false;
    bool show_requested_ = false; ///< Pending ImGui::OpenPopup.
    std::string file_;
    std::string display_name_;
    std::vector<MultiInputRow> inputs_;
    std::vector<MultiOutputRow> outputs_;

    bool run_requested_ = false; ///< Set on Run click; drained by takeRunRequest().
    std::string result_message_;
    bool result_ok_ = false;

    // OS file-dialog plumbing.  active_target_ indexes inputs_ [0..N) then
    // outputs_ [N..N+M); -1 when no picker is in flight.
    models::FileDialogState dialog_state_;
    int active_target_ = -1;
};

} // namespace whiteout::textool::views
