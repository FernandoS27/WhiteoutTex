// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "batch_convert.h"
#include "casc_browser.h"
#include "image_details.h"
#include "image_viewer.h"
#include "preferences.h"
#include "save_dialog.h"
#include "texture_converter.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>

namespace whiteout::gui {

/// Application configuration constants.
constexpr i32 WINDOW_WIDTH = 1280;
constexpr i32 WINDOW_HEIGHT = 800;
constexpr i32 MIN_WINDOW_WIDTH = 320;
constexpr i32 MIN_WINDOW_HEIGHT = 240;
constexpr const char* WINDOW_TITLE = "WhiteoutTex";

/// File dialog state shared between the OS callback and the main loop.
struct FileDialogState {
    std::mutex mtx;
    std::string pending_path;
    i32 pending_filter = -1;
    std::atomic<bool> has_pending{false};
};

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

    void drawMenuBar();
    void drawAboutDialog();
    void drawResultDialog();
    void drawBC3NDialog();
    void drawD4PayloadDialog();
#ifdef WHITEOUT_HAS_UPSCALER
    void drawUpscaleDialog();

    /// Launch the upscale process on a background thread (safe: result applied on main thread).
    void startUpscaleThread(const UpscalerModel& model,
                            const whiteout::textures::Texture& source, bool upscale_alpha);
#endif

    /// Apply a successfully loaded texture and update all dependent state.
    void applyLoadedTexture(const std::string& path, whiteout::textures::Texture texture);

    /// Open a file by path (used by Open Recent and the file-dialog callback).
    void openFile(const std::string& path);

    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    // Paths & preferences
    std::string imgui_ini_path_;
    SavePrefs save_prefs_;
    BatchPrefs batch_prefs_;
    RecentFiles recent_files_;
    RecentPaths recent_casc_paths_;
    RecentPaths recent_batch_input_dirs_;
    RecentPaths recent_batch_output_dirs_;

    // Loaded image data
    std::optional<whiteout::textures::Texture> loaded_texture_;
    std::string loaded_path_;
    std::string status_message_;
    whiteout::textures::TextureFileFormat loaded_file_format_ =
        whiteout::textures::TextureFileFormat::Unknown;
    whiteout::textures::PixelFormat loaded_source_fmt_ =
        whiteout::textures::PixelFormat::RGBA8;

    // Components
    ImageViewer viewer_;
    ImageDetails image_details_;
    SaveDialog save_dialog_;
    BatchConvert batch_convert_;
    CascBrowser casc_browser_;
    whiteout::textures::TextureConverter converter_;

    // File dialog state
    FileDialogState open_dialog_state_;
    FileDialogState save_dialog_state_;

    // UI flags
    bool show_about_ = false;
    bool show_result_popup_ = false;
    bool result_popup_success_ = false;
    std::string result_popup_message_;
    bool show_bc3n_dialog_ = false;

    // Diablo IV TEX payload dialog
    bool show_d4_payload_dialog_ = false;
    std::string pending_d4_meta_path_;
    char d4_payload_path_buf_[PATH_BUFFER_SIZE] = {};
    char d4_paylow_path_buf_[PATH_BUFFER_SIZE] = {};

#ifdef WHITEOUT_HAS_UPSCALER
    // Upscaler state
    Upscaler upscaler_;
    bool show_upscale_dialog_ = false;
    bool upscale_in_progress_ = false;
    i32 upscale_model_index_ = 0;
    i32 upscale_gpu_id_ = 0;
    std::vector<UpscalerModel> upscale_models_;
    std::string upscale_status_;
    mutable std::mutex upscale_status_mtx_; ///< Guards upscale_status_ across threads.
    std::atomic<bool> upscale_done_{false};
    bool upscale_success_ = false;
    std::optional<textures::Texture> upscale_result_;
    std::thread upscale_thread_;
#endif
};

/// Convenience wrapper matching the old API.
inline i32 run(i32 argc = 0, char** argv = nullptr) {
    App app;
    return app.run(argc, argv);
}

} // namespace whiteout::gui
