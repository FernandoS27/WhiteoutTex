// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_details.h"
#include "save_helpers.h"

#include <imgui.h>

namespace tex = whiteout::textures;
using TC = tex::TextureConverter;

namespace whiteout::gui {

// ============================================================================
// Details panel
// ============================================================================

std::vector<AppCommand> ImageDetails::drawDetailsPanel(tex::Texture* texture,
                                                       const std::string& path,
                                                       tex::TextureFileFormat file_format,
                                                       tex::PixelFormat source_fmt, f32 width,
                                                       f32 height) {

    std::vector<AppCommand> commands;

    ImGui::BeginChild("##TextPanel", ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Image Details");

    if (texture) {
        const auto& t = *texture;

        ImGui::SeparatorText("File");
        ImGui::Text("Path: %s", path.c_str());
        ImGui::Text("File Format: %s", TC::fileFormatName(file_format));

        ImGui::SeparatorText("Texture");
        ImGui::Text("Dimensions: %u x %u", t.width(), t.height());
        if (t.depth() > 1) {
            ImGui::Text("Depth: %u", t.depth());
        }
        ImGui::Text("Type: %s", TC::textureTypeName(t.type()));
        ImGui::Text("Pixel Format: %s", TC::pixelFormatName(source_fmt));

        {
            auto cur_kind = t.kind();
            const char* preview = textureKindName(cur_kind);
            if (ImGui::BeginCombo("Kind", preview)) {
                for (i32 i = 0; i < kSelectableKindCount; ++i) {
                    bool selected = (kSelectableKinds[i].kind == cur_kind);
                    if (ImGui::Selectable(kSelectableKinds[i].name, selected)) {
                        texture->setKind(kSelectableKinds[i].kind);
                        commands.push_back(RefreshDisplayCmd{});
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (texture->kind() == tex::TextureKind::Multikind) {
            static const char* kChLabels[] = {"R Channel", "G Channel", "B Channel", "A Channel"};
            for (i32 ci = 0; ci < 4; ++ci) {
                auto ch = kRGBAChannels[ci];
                auto ch_kind = texture->channelKind(ch);
                const char* ch_preview = textureKindName(ch_kind);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
                if (ImGui::BeginCombo(kChLabels[ci], ch_preview)) {
                    for (i32 ki = 0; ki < kChannelKindCount; ++ki) {
                        bool sel = (kChannelKinds[ki].kind == ch_kind);
                        if (ImGui::Selectable(kChannelKinds[ki].name, sel)) {
                            texture->setChannelKind(ch, kChannelKinds[ki].kind);
                            commands.push_back(RefreshDisplayCmd{});
                        }
                        if (sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::Text("sRGB: %s", t.isSrgb() ? "Yes" : "No");

        ImGui::SeparatorText("Mip Chain");
        ImGui::Text("Mip Levels: %u", t.mipCount());
        ImGui::Text("Layers: %u", t.layerCount());

        {
            const i32 maxMips = static_cast<i32>(tex::computeMaxMipCount(t.width(), t.height()));
            drawMipmapModeUI(generate_mips_, mipmap_mode_, mipmap_custom_count_, maxMips);
        }
        if (ImGui::Button("Regenerate Mipmaps")) {
            const auto mipCount = effectiveMipCount(mipmap_mode_, mipmap_custom_count_, *texture);
            commands.push_back(RegenerateMipmapsCmd{mipCount});
        }

        if (t.mipCount() > 0 && ImGui::TreeNode("Mip Level Details")) {
            for (u32 mip = 0; mip < t.mipCount(); ++mip) {
                const auto& ml = t.mipLevel(mip);
                ImGui::Text("Mip %u: %u x %u  (%llu bytes)", mip, ml.width, ml.height,
                            static_cast<unsigned long long>(ml.size));
            }
            ImGui::TreePop();
        }

        ImGui::SeparatorText("Downscale");
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            ImGui::Combo("##DownscaleLevel", &downscale_level_, kDownscaleOptions,
                         kDownscaleOptionCount);
            const u32 levels = static_cast<u32>(downscale_level_) + 1;
            const u32 new_w = t.width() >> levels;
            const u32 new_h = t.height() >> levels;
            const bool can_downscale = new_w >= 1 && new_h >= 1;
            if (!can_downscale)
                ImGui::BeginDisabled();
            if (ImGui::Button("Downscale")) {
                commands.push_back(DownscaleCmd{levels});
            }
            if (!can_downscale) {
                ImGui::EndDisabled();
                ImGui::TextWrapped("Image is too small to downscale further.");
            }
        }

#ifdef WHITEOUT_HAS_UPSCALER
        if (!upscale_models_.empty()) {
            ImGui::SeparatorText("Upscale");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (upscale_in_progress_)
                ImGui::BeginDisabled();
            if (ImGui::BeginCombo("##UpscaleModel",
                                  upscale_models_[upscale_model_index_].display_name.c_str())) {
                for (i32 i = 0; i < static_cast<i32>(upscale_models_.size()); ++i) {
                    bool selected = (i == upscale_model_index_);
                    std::string label = upscale_models_[i].label();
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        upscale_model_index_ = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Upscale Alpha", &upscale_alpha_);
            ImGui::SameLine();
            if (ImGui::Button("Upscale")) {
                commands.push_back(StartUpscaleCmd{upscale_model_index_, upscale_alpha_});
            }
            if (upscale_in_progress_) {
                ImGui::EndDisabled();
                ImGui::TextUnformatted("Upscaling...");
            }
        }
#endif
    } else {
        ImGui::TextWrapped("No image loaded. Use File > Open to load a texture.");
    }
    ImGui::EndChild();

    return commands;
}

// ============================================================================
// Mip list
// ============================================================================

std::vector<AppCommand> ImageDetails::drawMipList(const tex::Texture& texture, i32 selected_mip,
                                                  f32 width, f32 height) {

    std::vector<AppCommand> commands;

    ImGui::BeginChild("##MipList", ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Mip Levels");
    for (u32 mip = 0; mip < texture.mipCount(); ++mip) {
        const auto& ml = texture.mipLevel(mip);
        char label[64];
        std::snprintf(label, sizeof(label), "Mip %u  (%u x %u)", mip, ml.width, ml.height);
        if (ImGui::Selectable(label, selected_mip == static_cast<i32>(mip))) {
            commands.push_back(SelectMipCmd{static_cast<i32>(mip)});
        }
    }
    ImGui::EndChild();

    return commands;
}

#ifdef WHITEOUT_HAS_UPSCALER
void ImageDetails::setUpscalerModels(std::vector<UpscalerModel> models) {
    upscale_models_ = std::move(models);
    if (upscale_model_index_ >= static_cast<i32>(upscale_models_.size())) {
        upscale_model_index_ = 0;
    }
}

void ImageDetails::setUpscaleInProgress(bool in_progress) {
    upscale_in_progress_ = in_progress;
}
#endif

} // namespace whiteout::gui
