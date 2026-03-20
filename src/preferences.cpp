// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "preferences.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace whiteout::textool {
namespace {

// ============================================================================
// INI section visitor
// ============================================================================

/// Open @p ini_path and call @p visitor for every key=value line inside
/// @p section.  Stops when the section ends or the file is exhausted.
template <typename Visitor>
void visitIniSection(const std::string& ini_path, const char* section, Visitor&& visitor) {
    std::ifstream in(ini_path);
    if (!in.is_open())
        return;

    bool in_section = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == section) {
            in_section = true;
            continue;
        }
        if (!in_section)
            continue;
        if (!line.empty() && line.front() == '[') {
            in_section = false;
            continue;
        }
        visitor(line);
    }
}

} // anonymous namespace

// ============================================================================
// Load helpers
// ============================================================================

MainWindowIniRect load_main_window_ini_rect(const std::string& ini_path) {
    MainWindowIniRect rect;
    visitIniSection(ini_path, "[Window][##MainWindow]", [&](const std::string& line) {
        i32 a = 0, b = 0;
        if (std::sscanf(line.c_str(), "Pos=%d,%d", &a, &b) == 2) {
            rect.pos_x = a;
            rect.pos_y = b;
            rect.has_pos = true;
        } else if (std::sscanf(line.c_str(), "Size=%d,%d", &a, &b) == 2) {
            rect.width = a;
            rect.height = b;
            rect.has_size = true;
        }
    });
    return rect;
}

SavedHostWindowSize load_saved_host_window_size(const std::string& ini_path) {
    SavedHostWindowSize saved;
    visitIniSection(ini_path, "[WhiteoutTex][SDLWindow]", [&](const std::string& line) {
        i32 w = 0, h = 0;
        if (std::sscanf(line.c_str(), "Size=%d,%d", &w, &h) == 2) {
            saved.width = w;
            saved.height = h;
            saved.has_size = true;
        }
    });
    return saved;
}

void append_saved_host_window_size(const std::string& ini_path, i32 width, i32 height) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    out << "\n[WhiteoutTex][SDLWindow]\n";
    out << "Size=" << width << ',' << height << "\n";
}

SavePrefs load_save_prefs(const std::string& ini_path) {
    SavePrefs prefs;
    visitIniSection(ini_path, "[WhiteoutTex][SavePrefs]", [&](const std::string& line) {
        i32 iv = 0;
        f32 fv = 0.0f;
        if (std::sscanf(line.c_str(), "LastFilter=%d", &iv) == 1)
            prefs.last_filter = iv;
        else if (line.starts_with("LastOpenDir="))
            prefs.last_open_dir = line.substr(12);
        else if (line.starts_with("LastSaveDir="))
            prefs.last_save_dir = line.substr(12);
        else if (line.starts_with("LastCascDir="))
            prefs.last_casc_dir = line.substr(12);
        else if (std::sscanf(line.c_str(), "BlpVersion=%d", &iv) == 1)
            prefs.blp_version = iv;
        else if (std::sscanf(line.c_str(), "BlpEncoding=%d", &iv) == 1)
            prefs.blp_encoding = iv;
        else if (std::sscanf(line.c_str(), "BlpDither=%d", &iv) == 1)
            prefs.blp_dither = iv != 0;
        else if (std::sscanf(line.c_str(), "BlpDitherStrength=%f", &fv) == 1)
            prefs.blp_dither_strength = fv;
        else if (std::sscanf(line.c_str(), "DdsFormat=%d", &iv) == 1)
            prefs.dds_format = iv;
        else if (std::sscanf(line.c_str(), "DdsInvertY=%d", &iv) == 1)
            prefs.dds_invert_y = iv != 0;
        else if (std::sscanf(line.c_str(), "JpegQuality=%d", &iv) == 1)
            prefs.jpeg_quality = iv;
        else if (std::sscanf(line.c_str(), "JpegProgressive=%d", &iv) == 1)
            prefs.jpeg_progressive = iv != 0;
        else if (std::sscanf(line.c_str(), "GenerateMipmaps=%d", &iv) == 1)
            prefs.generate_mipmaps = iv != 0;
        else if (std::sscanf(line.c_str(), "MipmapMode=%d", &iv) == 1)
            prefs.mipmap_mode = static_cast<MipmapMode>(std::clamp(iv, 0, 2));
        else if (std::sscanf(line.c_str(), "MipmapCustomCount=%d", &iv) == 1)
            prefs.mipmap_custom_count = std::max(1, iv);
    });
    return prefs;
}

void append_save_prefs(const std::string& ini_path, const SavePrefs& prefs) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << "\n[WhiteoutTex][SavePrefs]\n";
    out << "LastFilter=" << prefs.last_filter << "\n";
    out << "LastOpenDir=" << prefs.last_open_dir << "\n";
    out << "LastSaveDir=" << prefs.last_save_dir << "\n";
    out << "LastCascDir=" << prefs.last_casc_dir << "\n";
    out << "BlpVersion=" << prefs.blp_version << "\n";
    out << "BlpEncoding=" << prefs.blp_encoding << "\n";
    out << "BlpDither=" << (prefs.blp_dither ? 1 : 0) << "\n";
    out << "BlpDitherStrength=" << prefs.blp_dither_strength << "\n";
    out << "DdsFormat=" << prefs.dds_format << "\n";
    out << "DdsInvertY=" << (prefs.dds_invert_y ? 1 : 0) << "\n";
    out << "JpegQuality=" << prefs.jpeg_quality << "\n";
    out << "JpegProgressive=" << (prefs.jpeg_progressive ? 1 : 0) << "\n";
    out << "GenerateMipmaps=" << (prefs.generate_mipmaps ? 1 : 0) << "\n";
    out << "MipmapMode=" << static_cast<i32>(prefs.mipmap_mode) << "\n";
    out << "MipmapCustomCount=" << prefs.mipmap_custom_count << "\n";
}

BatchPrefs load_batch_prefs(const std::string& ini_path) {
    BatchPrefs prefs;
    visitIniSection(ini_path, "[WhiteoutTex][BatchPrefs]", [&](const std::string& line) {
        i32 iv = 0;
        f32 fv = 0.0f;
        if (line.starts_with("LastInputDir="))
            prefs.last_input_dir = line.substr(13);
        else if (line.starts_with("LastOutputDir="))
            prefs.last_output_dir = line.substr(14);
        else if (std::sscanf(line.c_str(), "FilterBlp=%d", &iv) == 1)
            prefs.filter_blp = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterBmp=%d", &iv) == 1)
            prefs.filter_bmp = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterDds=%d", &iv) == 1)
            prefs.filter_dds = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterJpeg=%d", &iv) == 1)
            prefs.filter_jpeg = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterPng=%d", &iv) == 1)
            prefs.filter_png = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterTex=%d", &iv) == 1)
            prefs.filter_tex = iv != 0;
        else if (std::sscanf(line.c_str(), "FilterTga=%d", &iv) == 1)
            prefs.filter_tga = iv != 0;
        else if (std::sscanf(line.c_str(), "Recursive=%d", &iv) == 1)
            prefs.recursive = iv != 0;
        else if (std::sscanf(line.c_str(), "KeepLayout=%d", &iv) == 1)
            prefs.keep_layout = iv != 0;
        else if (std::sscanf(line.c_str(), "OutputFormat=%d", &iv) == 1)
            prefs.output_format = iv;
        else if (std::sscanf(line.c_str(), "BlpVersion=%d", &iv) == 1)
            prefs.blp_version = iv;
        else if (std::sscanf(line.c_str(), "BlpEncoding=%d", &iv) == 1)
            prefs.blp_encoding = iv;
        else if (std::sscanf(line.c_str(), "BlpDither=%d", &iv) == 1)
            prefs.blp_dither = iv != 0;
        else if (std::sscanf(line.c_str(), "BlpDitherStrength=%f", &fv) == 1)
            prefs.blp_dither_strength = fv;
        else if (std::sscanf(line.c_str(), "DdsMode=%d", &iv) == 1)
            prefs.dds_mode = iv;
        else if (std::sscanf(line.c_str(), "DdsFormatGeneral=%d", &iv) == 1)
            prefs.dds_format_general = iv;
        else if (std::sscanf(line.c_str(), "DdsInvertYGeneral=%d", &iv) == 1)
            prefs.dds_invert_y_general = iv != 0;
        else if (std::sscanf(line.c_str(), "DdsFormatNormal=%d", &iv) == 1)
            prefs.dds_format_normal = iv;
        else if (std::sscanf(line.c_str(), "DdsInvertYNormal=%d", &iv) == 1)
            prefs.dds_invert_y_normal = iv != 0;
        else if (std::sscanf(line.c_str(), "DdsFormatChannel=%d", &iv) == 1)
            prefs.dds_format_channel = iv;
        else if (std::sscanf(line.c_str(), "DdsFormatOther=%d", &iv) == 1)
            prefs.dds_format_other = iv;
        else if (std::sscanf(line.c_str(), "JpegQuality=%d", &iv) == 1)
            prefs.jpeg_quality = iv;
        else if (std::sscanf(line.c_str(), "JpegProgressive=%d", &iv) == 1)
            prefs.jpeg_progressive = iv != 0;
        else if (std::sscanf(line.c_str(), "GenerateMipmaps=%d", &iv) == 1)
            prefs.generate_mipmaps = iv != 0;
        else if (std::sscanf(line.c_str(), "MipmapMode=%d", &iv) == 1)
            prefs.mipmap_mode = static_cast<MipmapMode>(std::clamp(iv, 0, 2));
        else if (std::sscanf(line.c_str(), "MipmapCustomCount=%d", &iv) == 1)
            prefs.mipmap_custom_count = std::max(1, iv);
        else if (line.starts_with("Transform=")) {
            TransformStep step;
            const char* p = line.c_str() + 10;
            i32 type_int = 0;
            i32 n = 0;
            if (std::sscanf(p, "%d%n", &type_int, &n) == 1) {
                step.type = static_cast<TransformType>(std::clamp(type_int, 0, 1));
                p += n;
                if (*p == ',')
                    ++p;
                if (step.type == TransformType::Downscale) {
                    i32 lvl = 1;
                    if (std::sscanf(p, "%d", &lvl) == 1)
                        step.downscale_levels = std::clamp(lvl, 1, 2);
                } else {
                    i32 mi = 0, ua = 0;
                    if (std::sscanf(p, "%d,%d", &mi, &ua) >= 1) {
                        step.upscale_model_index = std::max(0, mi);
                        step.upscale_alpha = ua != 0;
                    }
                }
                prefs.transform_pipeline.push_back(step);
            }
        }
    });
    return prefs;
}

void append_batch_prefs(const std::string& ini_path, const BatchPrefs& prefs) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << "\n[WhiteoutTex][BatchPrefs]\n";
    out << "LastInputDir=" << prefs.last_input_dir << "\n";
    out << "LastOutputDir=" << prefs.last_output_dir << "\n";
    out << "FilterBlp=" << (prefs.filter_blp ? 1 : 0) << "\n";
    out << "FilterBmp=" << (prefs.filter_bmp ? 1 : 0) << "\n";
    out << "FilterDds=" << (prefs.filter_dds ? 1 : 0) << "\n";
    out << "FilterJpeg=" << (prefs.filter_jpeg ? 1 : 0) << "\n";
    out << "FilterPng=" << (prefs.filter_png ? 1 : 0) << "\n";
    out << "FilterTex=" << (prefs.filter_tex ? 1 : 0) << "\n";
    out << "FilterTga=" << (prefs.filter_tga ? 1 : 0) << "\n";
    out << "Recursive=" << (prefs.recursive ? 1 : 0) << "\n";
    out << "KeepLayout=" << (prefs.keep_layout ? 1 : 0) << "\n";
    out << "OutputFormat=" << prefs.output_format << "\n";
    out << "BlpVersion=" << prefs.blp_version << "\n";
    out << "BlpEncoding=" << prefs.blp_encoding << "\n";
    out << "BlpDither=" << (prefs.blp_dither ? 1 : 0) << "\n";
    out << "BlpDitherStrength=" << prefs.blp_dither_strength << "\n";
    out << "DdsMode=" << prefs.dds_mode << "\n";
    out << "DdsFormatGeneral=" << prefs.dds_format_general << "\n";
    out << "DdsInvertYGeneral=" << (prefs.dds_invert_y_general ? 1 : 0) << "\n";
    out << "DdsFormatNormal=" << prefs.dds_format_normal << "\n";
    out << "DdsInvertYNormal=" << (prefs.dds_invert_y_normal ? 1 : 0) << "\n";
    out << "DdsFormatChannel=" << prefs.dds_format_channel << "\n";
    out << "DdsFormatOther=" << prefs.dds_format_other << "\n";
    out << "JpegQuality=" << prefs.jpeg_quality << "\n";
    out << "JpegProgressive=" << (prefs.jpeg_progressive ? 1 : 0) << "\n";
    out << "GenerateMipmaps=" << (prefs.generate_mipmaps ? 1 : 0) << "\n";
    out << "MipmapMode=" << static_cast<i32>(prefs.mipmap_mode) << "\n";
    out << "MipmapCustomCount=" << prefs.mipmap_custom_count << "\n";
    for (const auto& step : prefs.transform_pipeline) {
        out << "Transform=" << static_cast<i32>(step.type) << ",";
        if (step.type == TransformType::Downscale) {
            out << step.downscale_levels;
        } else {
            out << step.upscale_model_index << "," << (step.upscale_alpha ? 1 : 0);
        }
        out << "\n";
    }
}

RecentFiles load_recent_files(const std::string& ini_path) {
    RecentFiles recent;
    visitIniSection(ini_path, "[WhiteoutTex][RecentFiles]", [&](const std::string& line) {
        if (!line.empty() && static_cast<i32>(recent.paths.size()) < MAX_RECENT_PATHS)
            recent.paths.push_back(line);
    });
    return recent;
}

void append_recent_files(const std::string& ini_path, const RecentFiles& recent) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open())
        return;
    out << "\n[WhiteoutTex][RecentFiles]\n";
    for (const auto& p : recent.paths)
        out << p << "\n";
}

RecentPaths load_recent_paths(const std::string& ini_path, const char* section) {
    RecentPaths recent;
    visitIniSection(ini_path, section, [&](const std::string& line) {
        if (!line.empty() && static_cast<i32>(recent.paths.size()) < MAX_RECENT_PATHS)
            recent.paths.push_back(line);
    });
    return recent;
}

void append_recent_paths(const std::string& ini_path, const char* section,
                         const RecentPaths& recent) {
    std::ofstream out(ini_path, std::ios::app);
    if (!out.is_open())
        return;
    out << "\n" << section << "\n";
    for (const auto& p : recent.paths)
        out << p << "\n";
}

} // namespace whiteout::textool
