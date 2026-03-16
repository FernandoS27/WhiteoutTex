// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "batch_convert.h"
#include "casc_browser.h"
#include "image_viewer.h"
#include "preferences.h"
#include "save_dialog.h"
#include "texture_converter.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>

namespace whiteout::gui {

/// Application configuration constants.
constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 800;
constexpr int MIN_WINDOW_WIDTH = 320;
constexpr int MIN_WINDOW_HEIGHT = 240;
constexpr const char* WINDOW_TITLE = "WhiteoutTex";

/// File dialog state shared between the OS callback and the main loop.
struct FileDialogState {
    std::mutex mtx;
    std::string pending_path;
    int pending_filter = -1;
    std::atomic<bool> has_pending{false};
};

/// Main application class.  Owns the SDL window, ImGui context,
/// and coordinates the image viewer, save dialog, and preferences.
class App {
public:
    App() = default;
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Run the application.  Returns the process exit code.
    int run();

private:
    bool initSDL();
    bool initWindow();
    void initImGui();
    void shutdown();

    void processOpenResult();
    void processSaveResult();

    void drawMenuBar();
    void drawDetailsPanel(float width, float height);
    void drawMipList(float width, float height);
    void drawAboutDialog();
    void drawResultDialog();
    void drawBC3NDialog();
    void drawD4PayloadDialog();

    /// Apply a successfully loaded texture and update all dependent state.
    void applyLoadedTexture(const std::string& path, whiteout::textures::Texture texture);

    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    // Paths & preferences
    std::string imgui_ini_path_;
    SavePrefs save_prefs_;

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
};

/// Convenience wrapper matching the old API.
inline int run() {
    App app;
    return app.run();
}

} // namespace whiteout::gui
