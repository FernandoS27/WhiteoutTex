// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_details.h"
#include "save_helpers.h"
#include "thread_pool_manager.h"

#include <cstdio>

#include <imgui.h>

namespace tex = whiteout::textures;
namespace interfaces = whiteout::interfaces;
using TC = tex::TextureConverter;

namespace whiteout::gui {

// ============================================================================
// Details panel
// ============================================================================

ImageDetailsResult ImageDetails::drawDetailsPanel(
    tex::Texture* texture,
    const std::string& path,
    tex::TextureFileFormat file_format,
    tex::PixelFormat source_fmt,
    float width, float height) {

    ImageDetailsResult result;

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
                for (int i = 0; i < kSelectableKindCount; ++i) {
                    bool selected = (kSelectableKinds[i].kind == cur_kind);
                    if (ImGui::Selectable(kSelectableKinds[i].name, selected)) {
                        texture->setKind(kSelectableKinds[i].kind);
                        result.refresh_display = true;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (texture->kind() == tex::TextureKind::Multikind) {
            static const char* kChLabels[] = {"R Channel", "G Channel", "B Channel", "A Channel"};
            static const tex::Channel kChannels[] = {
                tex::Channel::R, tex::Channel::G, tex::Channel::B, tex::Channel::A};
            for (int ci = 0; ci < 4; ++ci) {
                auto ch = kChannels[ci];
                auto ch_kind = texture->channelKind(ch);
                const char* ch_preview = textureKindName(ch_kind);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
                if (ImGui::BeginCombo(kChLabels[ci], ch_preview)) {
                    for (int ki = 0; ki < kChannelKindCount; ++ki) {
                        bool sel = (kChannelKinds[ki].kind == ch_kind);
                        if (ImGui::Selectable(kChannelKinds[ki].name, sel)) {
                            texture->setChannelKind(ch, kChannelKinds[ki].kind);
                            result.refresh_display = true;
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
            const int maxMips = static_cast<int>(
                tex::computeMaxMipCount(t.width(), t.height()));
            drawMipmapModeUI(generate_mips_, mipmap_mode_,
                             mipmap_custom_count_, maxMips);
        }
        if (ImGui::Button("Regenerate Mipmaps")) {
            auto* pool = threadPoolManager().get();
            const auto mipCount = effectiveMipCount(
                mipmap_mode_, mipmap_custom_count_, *texture);
            tex::Texture out;
            if (auto err = withBcnRoundtrip(*texture, pool, out,
                    [mipCount](tex::Texture& work, interfaces::WorkerPool* p) {
                        return work.generateMipmaps(mipCount, p);
                    })) {
                result.result_message = "Mipmap generation failed: " + *err;
                result.result_success = false;
            } else {
                result.updated_texture = std::move(out);
                result.result_message = "Mipmaps regenerated successfully.";
                result.result_success = true;
            }
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
            static const char* kDownscaleOptions[] = {"x2", "x4"};
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            ImGui::Combo("##DownscaleLevel", &downscale_level_,
                         kDownscaleOptions, IM_ARRAYSIZE(kDownscaleOptions));
            const u32 levels = static_cast<u32>(downscale_level_) + 1;
            const u32 new_w = t.width() >> levels;
            const u32 new_h = t.height() >> levels;
            const bool can_downscale = new_w >= 1 && new_h >= 1;
            if (!can_downscale) ImGui::BeginDisabled();
            if (ImGui::Button("Downscale")) {
                auto* pool = threadPoolManager().get();
                tex::Texture out;
                if (auto err = withBcnRoundtrip(*texture, pool, out,
                        [levels](tex::Texture& work, interfaces::WorkerPool* p) {
                            return work.downscale(levels, p);
                        })) {
                    result.result_message = "Downscale failed: " + *err;
                    result.result_success = false;
                } else {
                    result.updated_texture = std::move(out);
                    result.result_message = "Image downscaled successfully.";
                    result.result_success = true;
                }
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
            if (upscale_in_progress_) ImGui::BeginDisabled();
            if (ImGui::BeginCombo("##UpscaleModel",
                                  upscale_models_[upscale_model_index_].display_name.c_str())) {
                for (int i = 0; i < static_cast<int>(upscale_models_.size()); ++i) {
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
                result.upscale_model_index = upscale_model_index_;
                result.upscale_alpha = upscale_alpha_;
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

    return result;
}

// ============================================================================
// Mip list
// ============================================================================

int ImageDetails::drawMipList(const tex::Texture& texture,
                              int selected_mip, float width, float height) {
    int new_selection = -1;

    ImGui::BeginChild("##MipList", ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Mip Levels");
    for (u32 mip = 0; mip < texture.mipCount(); ++mip) {
        const auto& ml = texture.mipLevel(mip);
        char label[64];
        std::snprintf(label, sizeof(label), "Mip %u  (%u x %u)", mip, ml.width, ml.height);
        if (ImGui::Selectable(label, selected_mip == static_cast<int>(mip))) {
            new_selection = static_cast<int>(mip);
        }
    }
    ImGui::EndChild();

    return new_selection;
}

#ifdef WHITEOUT_HAS_UPSCALER
void ImageDetails::setUpscalerModels(std::vector<UpscalerModel> models) {
    upscale_models_ = std::move(models);
    if (upscale_model_index_ >= static_cast<int>(upscale_models_.size())) {
        upscale_model_index_ = 0;
    }
}

void ImageDetails::setUpscaleInProgress(bool in_progress) {
    upscale_in_progress_ = in_progress;
}
#endif

} // namespace whiteout::gui
