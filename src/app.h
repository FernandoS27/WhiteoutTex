// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "batch_convert.h"
#include "casc_browser.h"
#include "image_details.h"
#include "image_viewer.h"
#include "models/app_state.h"
#include "preferences.h"
#include "save_dialog.h"
#include "texture_converter.h"

#include <string>

#include <SDL3/SDL.h>

namespace whiteout::gui {

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
    TextureState tex_state_;

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

    // UI flags & dialog state
    UIFlags ui_;

#ifdef WHITEOUT_HAS_UPSCALER
    // Upscaler state
    UpscaleState upscale_;
#endif
};

/// Convenience wrapper matching the old API.
inline i32 run(i32 argc = 0, char** argv = nullptr) {
    App app;
    return app.run(argc, argv);
}

} // namespace whiteout::gui
