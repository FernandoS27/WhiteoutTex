// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "save_dialog.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include <whiteout/textures/blp/types.h>

#include <imgui.h>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

namespace {

constexpr const char* BLP_ENCODING_NAMES[] = {
    "Infer (Auto)", "True Color (BGRA)", "Paletted (256 colors)", "JPEG", "BC1 (DXT1)",
    "BC2 (DXT3)",   "BC3 (DXT5)"};
constexpr int BLP_ENCODING_COUNT = static_cast<int>(std::size(BLP_ENCODING_NAMES));

constexpr const char* DDS_FORMAT_NAMES[] = {
    "True Color (RGBA8)", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7"};

} // anonymous namespace

namespace whiteout_tex {

// ============================================================================
// Filter ordering
// ============================================================================

void SaveDialog::buildFilterOrder(const SavePrefs& prefs) {
    const int preferred = std::clamp(prefs.last_filter, 0, SAVE_FILTER_COUNT - 1);
    filter_map_[0] = preferred;
    int slot = 1;
    for (int i = 0; i < SAVE_FILTER_COUNT; ++i) {
        if (i != preferred) {
            filter_map_[slot++] = i;
        }
    }
    for (int i = 0; i < SAVE_FILTER_COUNT; ++i) {
        active_filters_[i] = SAVE_FILTERS[filter_map_[i]];
    }
}

// ============================================================================
// File chosen callback
// ============================================================================

void SaveDialog::onFileChosen(const std::string& path, int filter_idx, SavePrefs& prefs,
                              const tex::Texture* loaded_texture) {
    // Append extension if missing
    std::string final_path = path;
    if (std::filesystem::path(final_path).extension().empty() && filter_idx >= 0 &&
        filter_idx < SAVE_FILTER_COUNT) {
        std::string_view pat = active_filters_[filter_idx].pattern;
        auto sep = pat.find(';');
        final_path += '.';
        final_path += (sep == std::string_view::npos) ? pat : pat.substr(0, sep);
    }

    opts_.save_path = final_path;
    opts_.target_format = TC::classifyPath(final_path);

    // Map reordered index back to original
    if (filter_idx >= 0 && filter_idx < SAVE_FILTER_COUNT) {
        prefs.last_filter = filter_map_[filter_idx];
    }

    if (std::filesystem::exists(final_path)) {
        opts_.confirm_overwrite = true;
    } else {
        opts_.show_dialog = true;
    }

    // Restore last-used options
    opts_.blp_version = prefs.blp_version;
    opts_.blp_encoding = prefs.blp_encoding;
    opts_.blp_dither = prefs.blp_dither;
    opts_.blp_dither_strength = prefs.blp_dither_strength;
    opts_.dds_format = prefs.dds_format;
    opts_.jpeg_quality = prefs.jpeg_quality;
    opts_.generate_mipmaps = prefs.generate_mipmaps;
    opts_.texture_kind =
        static_cast<int>(loaded_texture ? loaded_texture->kind() : tex::TextureKind::Other);
}

// ============================================================================
// Draw
// ============================================================================

std::string SaveDialog::draw(TC& converter, const tex::Texture* loaded_texture, SavePrefs& prefs) {
    std::string status;

    // Overwrite confirmation
    if (opts_.confirm_overwrite) {
        ImGui::OpenPopup("Overwrite?");
    }
    if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("File already exists:\n  %s\n\nOverwrite?", opts_.save_path.c_str());
        ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            opts_.confirm_overwrite = false;
            opts_.show_dialog = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) {
            opts_.confirm_overwrite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Save options dialog
    if (opts_.show_dialog) {
        ImGui::OpenPopup("Save Options");
    }
    if (ImGui::BeginPopupModal("Save Options", &opts_.show_dialog,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saving to: %s", opts_.save_path.c_str());
        ImGui::Text("Format: %s", TC::fileFormatName(opts_.target_format));
        ImGui::Separator();

        // Format-specific options
        switch (opts_.target_format) {
        case TFF::BLP:
            ImGui::SeparatorText("BLP Options");
            ImGui::Combo("BLP Version", &opts_.blp_version,
                         "BLP1 (Warcraft 3 Classic)\0BLP2 (WoW)\0");
            {
                // BLP1 only supports Paletted (2) and JPEG (3)
                static constexpr int BLP1_ENC[] = {2, 3};
                static constexpr int BLP2_ENC[] = {0, 1, 2, 3, 4, 5, 6};
                const bool is_blp1 = (opts_.blp_version == 0);
                const int* enc_allowed = is_blp1 ? BLP1_ENC : BLP2_ENC;
                const int enc_count = is_blp1 ? 2 : 7;

                bool enc_valid = false;
                for (int i = 0; i < enc_count; ++i)
                    if (enc_allowed[i] == opts_.blp_encoding) {
                        enc_valid = true;
                        break;
                    }
                if (!enc_valid)
                    opts_.blp_encoding = enc_allowed[0];

                if (ImGui::BeginCombo("Encoding", BLP_ENCODING_NAMES[opts_.blp_encoding])) {
                    for (int i = 0; i < enc_count; ++i) {
                        bool selected = (opts_.blp_encoding == enc_allowed[i]);
                        if (ImGui::Selectable(BLP_ENCODING_NAMES[enc_allowed[i]], selected))
                            opts_.blp_encoding = enc_allowed[i];
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            if (opts_.blp_encoding == 2) {
                ImGui::Checkbox("Dither", &opts_.blp_dither);
                if (opts_.blp_dither) {
                    ImGui::SliderFloat("Dither Strength", &opts_.blp_dither_strength, 0.0f, 1.0f);
                }
            }
            if (opts_.blp_encoding == 3) {
                ImGui::SliderInt("JPEG Quality", &opts_.jpeg_quality, 1, 100);
            }
            break;

        case TFF::DDS: {
            ImGui::SeparatorText("DDS Options");

            static constexpr int PRESET_NORMAL[] = {0, 5};
            static constexpr int PRESET_CHANNEL[] = {0, 4};
            static constexpr int PRESET_OTHER[] = {0, 1, 2, 3, 6, 7};

            const int tk = opts_.texture_kind;
            const int* allowed;
            int allowed_count;
            if (tk == 2) {
                allowed = PRESET_NORMAL;
                allowed_count = 2;
            } else if (tk == 6 || tk == 7 || tk == 8 || tk == 9) {
                allowed = PRESET_CHANNEL;
                allowed_count = 2;
            } else {
                allowed = PRESET_OTHER;
                allowed_count = 6;
            }

            bool in_allowed = false;
            for (int i = 0; i < allowed_count; ++i)
                if (allowed[i] == opts_.dds_format) {
                    in_allowed = true;
                    break;
                }
            if (!in_allowed)
                opts_.dds_format = allowed[0];

            char preset_label[128];
            {
                int n = std::snprintf(preset_label, sizeof(preset_label), "Preset: %s — ",
                                      TEXTURE_KIND_NAMES[tk]);
                for (int i = 0; i < allowed_count && n < (int)sizeof(preset_label) - 1; ++i) {
                    if (i > 0)
                        n += std::snprintf(preset_label + n, sizeof(preset_label) - n, ", ");
                    n += std::snprintf(preset_label + n, sizeof(preset_label) - n, "%s",
                                       DDS_FORMAT_NAMES[allowed[i]]);
                }
            }
            ImGui::TextDisabled("%s", preset_label);

            if (ImGui::BeginCombo("Pixel Format", DDS_FORMAT_NAMES[opts_.dds_format])) {
                for (int i = 0; i < allowed_count; ++i) {
                    bool selected = (opts_.dds_format == allowed[i]);
                    if (ImGui::Selectable(DDS_FORMAT_NAMES[allowed[i]], selected))
                        opts_.dds_format = allowed[i];
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }

        case TFF::JPEG:
            ImGui::SeparatorText("JPEG Options");
            ImGui::SliderInt("Quality", &opts_.jpeg_quality, 1, 100);
            break;

        default:
            break;
        }

        // Common options
        ImGui::SeparatorText("Common Options");
        ImGui::Combo("Texture Kind", &opts_.texture_kind, TEXTURE_KIND_NAMES, TEXTURE_KIND_COUNT);
        ImGui::Checkbox("Generate Mipmaps", &opts_.generate_mipmaps);

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0)) && loaded_texture) {
            status = performSave(converter, *loaded_texture, prefs);
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            opts_.show_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return status;
}

// ============================================================================
// Save execution
// ============================================================================

std::string SaveDialog::performSave(TC& converter, const tex::Texture& source, SavePrefs& prefs) {
    auto tex_copy = source;
    tex_copy.setKind(static_cast<tex::TextureKind>(opts_.texture_kind));

    if (opts_.generate_mipmaps) {
        bool is_bcn = tex_copy.format() >= tex::PixelFormat::BC1 &&
                      tex_copy.format() <= tex::PixelFormat::BC7;
        if (is_bcn) {
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
        }
        tex_copy.generateMipmaps();
    }

    bool ok = false;
    switch (opts_.target_format) {
    case TFF::BLP: {
        tex::blp::SaveOptions blp;
        blp.version =
            opts_.blp_version == 0 ? tex::blp::BlpVersion::BLP1 : tex::blp::BlpVersion::BLP2;
        switch (opts_.blp_encoding) {
        case 0:
            blp.encoding = tex::blp::BlpEncoding::Infer;
            break;
        case 1:
            blp.encoding = tex::blp::BlpEncoding::BGRA;
            break;
        case 2:
            blp.encoding = tex::blp::BlpEncoding::Palettized;
            break;
        case 3:
            blp.encoding = tex::blp::BlpEncoding::JPEG;
            break;
        case 4:
            blp.encoding = tex::blp::BlpEncoding::DXT;
            break;
        case 5:
            blp.encoding = tex::blp::BlpEncoding::DXT;
            break;
        case 6:
            blp.encoding = tex::blp::BlpEncoding::DXT;
            break;
        default:
            break;
        }
        blp.dither = opts_.blp_dither;
        blp.ditherStrength = opts_.blp_dither_strength;
        blp.jpegQuality = opts_.jpeg_quality;

        if (blp.encoding == tex::blp::BlpEncoding::JPEG ||
            blp.encoding == tex::blp::BlpEncoding::Palettized) {
            blp.version = tex::blp::BlpVersion::BLP1;
        }

        if (blp.encoding == tex::blp::BlpEncoding::JPEG ||
            blp.encoding == tex::blp::BlpEncoding::Palettized ||
            blp.encoding == tex::blp::BlpEncoding::BGRA ||
            blp.encoding == tex::blp::BlpEncoding::Infer) {
            if (tex_copy.format() != tex::PixelFormat::RGBA8) {
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
            }
        } else if (opts_.blp_encoding == 4) {
            if (tex_copy.format() != tex::PixelFormat::BC1)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC1);
        } else if (opts_.blp_encoding == 5) {
            if (tex_copy.format() != tex::PixelFormat::BC2)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC2);
        } else if (opts_.blp_encoding == 6) {
            if (tex_copy.format() != tex::PixelFormat::BC3)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC3);
        }
        ok = converter.save(tex_copy, opts_.save_path, blp);
        break;
    }
    case TFF::DDS: {
        constexpr tex::PixelFormat DDS_FORMATS[] = {
            tex::PixelFormat::RGBA8, tex::PixelFormat::BC1, tex::PixelFormat::BC2,
            tex::PixelFormat::BC3,   tex::PixelFormat::BC4, tex::PixelFormat::BC5,
            tex::PixelFormat::BC6H,  tex::PixelFormat::BC7,
        };
        auto target = DDS_FORMATS[opts_.dds_format];
        if (tex_copy.format() != target) {
            tex_copy = tex_copy.copyAsFormat(target);
        }
        ok = converter.save(tex_copy, opts_.save_path);
        break;
    }
    case TFF::JPEG:
        ok = converter.save(tex_copy, opts_.save_path, opts_.jpeg_quality);
        break;
    default:
        ok = converter.save(tex_copy, opts_.save_path);
        break;
    }

    std::string status;
    if (ok) {
        status = "Saved: " + opts_.save_path;
        prefs.blp_version = opts_.blp_version;
        prefs.blp_encoding = opts_.blp_encoding;
        prefs.blp_dither = opts_.blp_dither;
        prefs.blp_dither_strength = opts_.blp_dither_strength;
        prefs.dds_format = opts_.dds_format;
        prefs.jpeg_quality = opts_.jpeg_quality;
        prefs.generate_mipmaps = opts_.generate_mipmaps;
    } else {
        status = "Failed to save: " + opts_.save_path;
        if (converter.hasIssues()) {
            for (const auto& issue : converter.getIssues()) {
                status += "\n  " + issue;
            }
        }
    }
    return status;
}

} // namespace whiteout_tex
