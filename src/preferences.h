// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>

namespace whiteout::gui {

/// Saved host (SDL) window size persisted in the INI file.
struct SavedHostWindowSize {
    int width = 0;
    int height = 0;
    bool has_size = false;
};

/// ImGui main-window position/size read back from the INI file.
struct MainWindowIniRect {
    int pos_x = 0;
    int pos_y = 0;
    int width = 0;
    int height = 0;
    bool has_pos = false;
    bool has_size = false;
};

/// Save preferences persisted in the [WhiteoutTex][SavePrefs] INI section.
struct SavePrefs {
    int last_filter = 0;
    int blp_version = 1;
    int blp_encoding = 0;
    bool blp_dither = false;
    float blp_dither_strength = 0.8f;
    int dds_format = 0;
    int jpeg_quality = 75;
    bool generate_mipmaps = false;
};

/// Load the [Window][##MainWindow] section from an ImGui INI file.
MainWindowIniRect load_main_window_ini_rect(const std::string& ini_path);

/// Load the [WhiteoutTex][SDLWindow] section.
SavedHostWindowSize load_saved_host_window_size(const std::string& ini_path);

/// Append the [WhiteoutTex][SDLWindow] section.
void append_saved_host_window_size(const std::string& ini_path, int width, int height);

/// Load the [WhiteoutTex][SavePrefs] section.
SavePrefs load_save_prefs(const std::string& ini_path);

/// Append the [WhiteoutTex][SavePrefs] section.
void append_save_prefs(const std::string& ini_path, const SavePrefs& prefs);

} // namespace whiteout::gui
