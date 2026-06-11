// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "models/app_state.h"
#include "preferences.h"
#include "services/texture_service.h"
#include "texture_converter.h"
#include "views/batch_convert.h"
#include "views/casc_browser.h"
#include "views/image_details.h"
#include "views/image_viewer.h"
#include "views/multi_pipeline_dialog.h"
#include "views/pipeline_editor.h"
#include "views/save_dialog.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "services/upscaler_service.h"
#endif

#include <optional>

#include <string>
#include <unordered_map>

#include <SDL3/SDL.h>

namespace whiteout::textool {

/// Application configuration constants.
constexpr i32 WINDOW_WIDTH = 1280;
constexpr i32 WINDOW_HEIGHT = 800;
constexpr i32 MIN_WINDOW_WIDTH = 320;
constexpr i32 MIN_WINDOW_HEIGHT = 240;
constexpr const char* WINDOW_TITLE = "WhiteoutTex";

/// Main application class.  Owns the SDL window, ImGui context,
/// and coordinates the image viewer, save dialog, and preferences.
class App {
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Run the application.  Returns the process exit code.
    i32 run(i32 argc = 0, char** argv = nullptr);

private:
    bool initSDL();
    bool initWindow();
    void initImGui();
    void shutdown();

    void processOpenResult();
    void processSaveResult();

    /// Dispatch a batch of commands from any view.
    void dispatchCommands(std::vector<models::AppCommand>& commands);

#ifdef WHITEOUT_HAS_UPSCALER
    void pollUpscaleResult();
#endif

    /// Open a file by path (used by Open Recent and the file-dialog callback).
    void openFile(const std::string& path);

    /// Apply a TextureLoadResult to the application state.
    void applyLoadResult(const std::string& path, services::TextureLoadResult result);

    /// Run a standard pipeline (file name within pipelines_dir_) on the current
    /// image, replacing it with the result.  Prompts for Real Input parameters
    /// first if the pipeline has any.
    void runPipeline(const std::string& name);

    /// Execute an already-loaded pipeline graph on the current image.
    void executePipelineGraph(pipeline::NodeGraph& graph);

    /// Modal that collects Real Input parameter values before running.
    void drawPipelineParamDialog();

    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    // Paths & preferences
    std::string imgui_ini_path_;
    std::string lang_dir_;
    std::string pipelines_dir_;                       ///< resources/pipelines next to the exe.
    std::vector<models::PipelineInfo> pipeline_files_; ///< All pipelines found in pipelines_dir_.
    /// Standard-type pipelines only — the subset runnable on the current image
    /// from Preview (the Run Pipeline menu / combobox).
    std::vector<models::PipelineInfo> standard_pipeline_files_;
    /// Varying-type pipelines — run via the MultiPipelines dialog (file in/out).
    std::vector<models::PipelineInfo> varying_pipeline_files_;
    /// Each pipeline's interface, keyed by file (for the MultiPipeline dialog).
    std::unordered_map<std::string, pipeline::PipelineInterface> pipeline_interfaces_;

    /// (Re)scan pipelines_dir_ and refresh every pipeline list/interface.
    void scanPipelines();
    /// Load a Varying pipeline and open the MultiPipeline dialog for it.
    void openMultiPipeline(const std::string& name);
    /// Execute the MultiPipeline dialog's run request (load inputs, run, save).
    void runMultiPipeline();

    // Pending parameterized pipeline run (Real Input prompt).
    std::optional<pipeline::NodeGraph> param_graph_;
    bool param_dialog_request_ = false;
    AppPrefs app_prefs_;
    SavePrefs save_prefs_;
    BatchPrefs batch_prefs_;
    RecentFiles recent_files_;
    RecentPaths recent_casc_paths_;
    RecentPaths recent_batch_input_dirs_;
    RecentPaths recent_batch_output_dirs_;

    // Loaded image data
    models::TextureState tex_state_;

    // Components
    views::ImageViewer viewer_;
    views::ImageDetails image_details_;
    views::SaveDialog save_dialog_;
    views::BatchConvert batch_convert_;
    views::CascBrowser casc_browser_;
    views::PipelineEditor pipeline_editor_;
    views::MultiPipelineDialog multi_pipeline_dialog_;
    whiteout::textures::TextureConverter converter_;

    // Services
    services::TextureService texture_service_{converter_};

    // File dialog state
    models::FileDialogState open_dialog_state_;
    models::FileDialogState save_dialog_state_;

    // UI flags & dialog state
    models::UIFlags ui_;

#ifdef WHITEOUT_HAS_UPSCALER
    // Upscaler service
    services::UpscalerService upscaler_service_;
    i32 upscale_model_index_ = 0;
    std::vector<UpscalerModel> upscale_models_;
#endif
};

/// Convenience wrapper matching the old API.
inline i32 run(i32 argc = 0, char** argv = nullptr) {
    App app;
    return app.run(argc, argv);
}

} // namespace whiteout::textool
