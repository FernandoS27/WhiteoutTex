// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "preferences.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace whiteout::gui {

MainWindowIniRect load_main_window_ini_rect(const std::string& ini_path) {
    MainWindowIniRect rect;
    std::ifstream in(ini_path);
    if (!in.is_open()) {
        return rect;
    }

    bool in_main_window_section = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line == "[Window][##MainWindow]") {
            in_main_window_section = true;
            continue;
        }

        if (!in_main_window_section) {
            continue;
        }

        if (!line.empty() && line.front() == '[') {
            break;
        }

        int a = 0;
        int b = 0;
        if (std::sscanf(line.c_str(), "Pos=%d,%d", &a, &b) == 2) {
            rect.pos_x = a;
            rect.pos_y = b;
            rect.has_pos = true;
            continue;
        }
        if (std::sscanf(line.c_str(), "Size=%d,%d", &a, &b) == 2) {
            rect.width = a;
            rect.height = b;
            rect.has_size = true;
            continue;
        }
    }

    return rect;
}

SavedHostWindowSize load_saved_host_window_size(const std::string& ini_path) {
    SavedHostWindowSize saved;
    std::ifstream in(ini_path);
    if (!in.is_open()) {
        return saved;
    }

    bool in_host_window_section = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line == "[WhiteoutTex][SDLWindow]") {
            in_host_window_section = true;
            continue;
        }

        if (!in_host_window_section) {
            continue;
        }

        if (!line.empty() && line.front() == '[') {
            in_host_window_section = false;
            continue;
        }

        int w = 0;
        int h = 0;
        if (std::sscanf(line.c_str(), "Size=%d,%d", &w, &h) == 2) {
            saved.width = w;
            saved.height = h;
            saved.has_size = true;
        }
    }

    return saved;
}

void append_saved_host_window_size(const std::string& ini_path, int width, int height) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    out << "\n[WhiteoutTex][SDLWindow]\n";
    out << "Size=" << width << ',' << height << "\n";
}

SavePrefs load_save_prefs(const std::string& ini_path) {
    SavePrefs prefs;
    std::ifstream in(ini_path);
    if (!in.is_open()) {
        return prefs;
    }

    bool in_section = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "[WhiteoutTex][SavePrefs]") {
            in_section = true;
            continue;
        }
        if (!in_section) {
            continue;
        }
        if (!line.empty() && line.front() == '[') {
            in_section = false;
            continue;
        }
        int iv = 0;
        float fv = 0.0f;
        if (std::sscanf(line.c_str(), "LastFilter=%d", &iv) == 1) {
            prefs.last_filter = iv;
            continue;
        }
        if (std::sscanf(line.c_str(), "BlpVersion=%d", &iv) == 1) {
            prefs.blp_version = iv;
            continue;
        }
        if (std::sscanf(line.c_str(), "BlpEncoding=%d", &iv) == 1) {
            prefs.blp_encoding = iv;
            continue;
        }
        if (std::sscanf(line.c_str(), "BlpDither=%d", &iv) == 1) {
            prefs.blp_dither = iv != 0;
            continue;
        }
        if (std::sscanf(line.c_str(), "BlpDitherStrength=%f", &fv) == 1) {
            prefs.blp_dither_strength = fv;
            continue;
        }
        if (std::sscanf(line.c_str(), "DdsFormat=%d", &iv) == 1) {
            prefs.dds_format = iv;
            continue;
        }
        if (std::sscanf(line.c_str(), "JpegQuality=%d", &iv) == 1) {
            prefs.jpeg_quality = iv;
            continue;
        }
        if (std::sscanf(line.c_str(), "GenerateMipmaps=%d", &iv) == 1) {
            prefs.generate_mipmaps = iv != 0;
            continue;
        }
    }
    return prefs;
}

void append_save_prefs(const std::string& ini_path, const SavePrefs& prefs) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << "\n[WhiteoutTex][SavePrefs]\n";
    out << "LastFilter=" << prefs.last_filter << "\n";
    out << "BlpVersion=" << prefs.blp_version << "\n";
    out << "BlpEncoding=" << prefs.blp_encoding << "\n";
    out << "BlpDither=" << (prefs.blp_dither ? 1 : 0) << "\n";
    out << "BlpDitherStrength=" << prefs.blp_dither_strength << "\n";
    out << "DdsFormat=" << prefs.dds_format << "\n";
    out << "JpegQuality=" << prefs.jpeg_quality << "\n";
    out << "GenerateMipmaps=" << (prefs.generate_mipmaps ? 1 : 0) << "\n";
}

} // namespace whiteout::gui
