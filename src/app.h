// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

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

    // Components
    ImageViewer viewer_;
    SaveDialog save_dialog_;
    whiteout::textures::TextureConverter converter_;

    // File dialog state
    FileDialogState open_dialog_state_;
    FileDialogState save_dialog_state_;

    // UI flags
    bool show_about_ = false;
    bool show_result_popup_ = false;
    std::string result_popup_message_;
};

/// Convenience wrapper matching the old API.
inline int run() {
    App app;
    return app.run();
}

} // namespace whiteout::gui
