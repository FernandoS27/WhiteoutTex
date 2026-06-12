// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_details.h"
#include "localization.h"
#include "save_helpers.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include <imgui.h>

namespace tex = whiteout::textures;
using TC = tex::TextureConverter;

namespace whiteout::textool::views {

using namespace models;
using i18n::tr;

namespace {

/// A framed button drawing a curved undo / redo arrow (no font glyph needed,
/// so it renders identically in every language).  Mirrored on X for redo.
bool historyIconButton(const char* id, bool is_redo, bool enabled, f32 width) {
    if (!enabled)
        ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(id, ImVec2(width, ImGui::GetFrameHeight() * 1.25f));

    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const ImVec2 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f};
    const f32 r = (mx.y - mn.y) * 0.28f;
    const ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Arc from just past the right horizon, over the top, to just past the left
    // horizon (screen space, y down) — mirrored on X for the redo arrow.
    const f32 sx = is_redo ? -1.0f : 1.0f;
    const f32 th0 = 0.15f * 3.14159265f;
    const f32 th1 = -1.15f * 3.14159265f;
    const auto pt = [&](f32 th) {
        return ImVec2(c.x + sx * r * std::cos(th), c.y + r * std::sin(th));
    };
    dl->PathClear();
    constexpr int kSegs = 24;
    for (int i = 0; i <= kSegs; ++i)
        dl->PathLineTo(pt(th0 + (th1 - th0) * (static_cast<f32>(i) / kSegs)));
    dl->PathStroke(col, ImDrawFlags_None, std::max(1.5f, r * 0.28f));

    // Arrowhead at the end of the sweep, aligned with the direction of travel.
    const ImVec2 end = pt(th1);
    const ImVec2 tan{sx * std::sin(th1), -std::cos(th1)}; // unit tangent (decreasing angle)
    const ImVec2 nrm{-tan.y, tan.x};
    const f32 ah = r * 0.95f;
    dl->AddTriangleFilled(ImVec2(end.x + tan.x * ah, end.y + tan.y * ah),
                          ImVec2(end.x + nrm.x * ah * 0.55f, end.y + nrm.y * ah * 0.55f),
                          ImVec2(end.x - nrm.x * ah * 0.55f, end.y - nrm.y * ah * 0.55f), col);

    if (!enabled)
        ImGui::EndDisabled();
    return clicked;
}

} // namespace

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

    if (texture) {
        const auto& t = *texture;

        // Edit history at the very top: step back / forward through operations
        // applied to the image (mipmaps, downscale, upscale, pipelines, swaps).
        {
            const f32 half = (ImGui::GetContentRegionAvail().x -
                              ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (historyIconButton("##history_undo", /*is_redo=*/false, can_undo_, half))
                commands.push_back(UndoImageCmd{});
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s (Ctrl+Z)", tr("details.undo"));
            ImGui::SameLine();
            if (historyIconButton("##history_redo", /*is_redo=*/true, can_redo_, half))
                commands.push_back(RedoImageCmd{});
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s (Ctrl+Y)", tr("details.redo"));
        }

        ImGui::SeparatorText(tr("details.file"));
        ImGui::Text(tr("details.path"), path.c_str());
        ImGui::Text(tr("details.file_format"), TC::fileFormatName(file_format));

        ImGui::SeparatorText(tr("details.texture"));
        ImGui::Text(tr("details.dimensions"), t.width(), t.height());
        if (t.depth() > 1) {
            ImGui::Text(tr("details.depth"), t.depth());
        }
        ImGui::Text(tr("details.type"), TC::textureTypeName(t.type()));
        ImGui::Text(tr("details.pixel_format"), TC::pixelFormatName(source_fmt));

        {
            auto cur_kind = t.kind();
            const char* preview = textureKindName(cur_kind);
            if (ImGui::BeginCombo(tr("details.kind"), preview)) {
                for (i32 i = 0; i < kSelectableKindCount; ++i) {
                    bool selected = (kSelectableKinds[i].kind == cur_kind);
                    if (ImGui::Selectable(kindLabel(kSelectableKinds[i]), selected)) {
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
            const char* kChLabels[] = {tr("details.channel_r"), tr("details.channel_g"),
                                       tr("details.channel_b"), tr("details.channel_a")};
            for (i32 ci = 0; ci < 4; ++ci) {
                auto ch = kRGBAChannels[ci];
                auto ch_kind = texture->channelKind(ch);
                const char* ch_preview = textureKindName(ch_kind);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
                if (ImGui::BeginCombo(kChLabels[ci], ch_preview)) {
                    for (i32 ki = 0; ki < kChannelKindCount; ++ki) {
                        bool sel = (kChannelKinds[ki].kind == ch_kind);
                        if (ImGui::Selectable(kindLabel(kChannelKinds[ki]), sel)) {
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

        ImGui::Text(tr("details.srgb"), t.isSrgb() ? tr("details.yes") : tr("details.no"));

        ImGui::SeparatorText(tr("details.mip_chain"));
        ImGui::Text(tr("details.mip_levels"), t.mipCount());
        ImGui::Text(tr("details.layers"), t.layerCount());

        {
            const i32 maxMips = static_cast<i32>(tex::computeMaxMipCount(t.width(), t.height()));
            drawMipmapModeUI(generate_mips_, mipmap_mode_, mipmap_custom_count_, maxMips);
        }
        if (ImGui::Button(tr("details.regenerate_mipmaps"))) {
            const auto mipCount = effectiveMipCount(mipmap_mode_, mipmap_custom_count_, *texture);
            commands.push_back(RegenerateMipmapsCmd{mipCount});
        }

        if (t.mipCount() > 0 && ImGui::TreeNode(tr("details.mip_level_details"))) {
            for (u32 mip = 0; mip < t.mipCount(); ++mip) {
                const auto& ml = t.mipLevel(mip);
                ImGui::Text(tr("details.mip_entry"), mip, ml.width, ml.height,
                            static_cast<unsigned long long>(ml.size));
            }
            ImGui::TreePop();
        }

        ImGui::SeparatorText(tr("details.downscale"));
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
            if (ImGui::Button(tr("details.downscale_button"))) {
                commands.push_back(DownscaleCmd{levels});
            }
            if (!can_downscale) {
                ImGui::EndDisabled();
                ImGui::TextWrapped("%s", tr("details.too_small_to_downscale"));
            }
        }

        // Run a standard pipeline (from resources/pipelines) on this image.  The
        // pipelines are shown in a list grouped by category; selecting one reveals
        // its extra (non-Standard) inputs as inline fields plus a Run button.
        if (!pipelines_.empty()) {
            ImGui::SeparatorText(tr("details.pipeline"));

            const float list_h = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
            ImGui::BeginChild("##PipelineList", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders);
            // pipelines_ is sorted by (category, name), so each category is one
            // contiguous run — open a header when the category changes.
            std::string cur_cat;
            bool cat_open = false;
            bool have_cat = false;
            for (i32 i = 0; i < static_cast<i32>(pipelines_.size()); ++i) {
                const auto& p = pipelines_[i];
                const std::string cat =
                    p.category.empty() ? std::string(tr("details.pipeline_uncategorized"))
                                       : p.category;
                if (!have_cat || cat != cur_cat) {
                    cur_cat = cat;
                    have_cat = true;
                    cat_open = ImGui::CollapsingHeader((cat + "##plcat").c_str(),
                                                       ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (!cat_open)
                    continue;
                ImGui::Indent();
                const bool selected = (p.file == selected_pipeline_file_);
                const std::string lbl = p.display_name + "##pl" + std::to_string(i);
                if (ImGui::Selectable(lbl.c_str(), selected))
                    selectPipeline(p.file);
                ImGui::Unindent();
            }
            ImGui::EndChild();

            // Inline parameters + Run for the selected pipeline.
            if (!selected_pipeline_file_.empty()) {
                const auto it = pipeline_params_.find(selected_pipeline_file_);
                const std::vector<models::PipelineParam>* params =
                    it != pipeline_params_.end() ? &it->second : nullptr;
                if (params) {
                    param_values_.resize(params->size());
                    for (std::size_t i = 0; i < params->size(); ++i) {
                        const auto& pp = (*params)[i];
                        const std::string lbl =
                            (pp.name.empty() ? std::string("value") : pp.name) + "##plp" +
                            std::to_string(i);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                        if (pp.is_integer) {
                            int v = static_cast<int>(std::lround(param_values_[i]));
                            if (pp.clamp) {
                                int mn = static_cast<int>(pp.min), mx = static_cast<int>(pp.max);
                                if (mx < mn)
                                    std::swap(mn, mx);
                                if (ImGui::SliderInt(lbl.c_str(), &v, mn, mx))
                                    param_values_[i] = v;
                            } else if (ImGui::InputInt(lbl.c_str(), &v)) {
                                param_values_[i] = v;
                            }
                        } else {
                            float v = static_cast<float>(param_values_[i]);
                            if (pp.clamp) {
                                float mn = static_cast<float>(pp.min), mx = static_cast<float>(pp.max);
                                if (mx < mn)
                                    std::swap(mn, mx);
                                if (ImGui::SliderFloat(lbl.c_str(), &v, mn, mx))
                                    param_values_[i] = v;
                            } else if (ImGui::InputFloat(lbl.c_str(), &v)) {
                                param_values_[i] = v;
                            }
                        }
                    }
                }
                if (ImGui::Button(tr("details.run_pipeline"))) {
                    RunPipelineCmd cmd{selected_pipeline_file_, {}};
                    if (params)
                        for (std::size_t i = 0; i < params->size(); ++i)
                            cmd.overrides.emplace_back((*params)[i].name, param_values_[i]);
                    commands.push_back(std::move(cmd));
                }
            }
        }

#ifdef WHITEOUT_HAS_UPSCALER
        if (!upscale_models_.empty()) {
            ImGui::SeparatorText(tr("details.upscale"));
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
            ImGui::Checkbox(tr("details.upscale_alpha"), &upscale_alpha_);
            ImGui::SameLine();
            if (ImGui::Button(tr("details.upscale_button"))) {
                commands.push_back(StartUpscaleCmd{upscale_model_index_, upscale_alpha_});
            }
            if (upscale_in_progress_) {
                ImGui::EndDisabled();
                ImGui::TextUnformatted(tr("details.upscaling"));
            }
        }
#endif
    } else {
        ImGui::TextWrapped("%s", tr("details.no_image_loaded"));
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
    ImGui::SeparatorText(tr("details.mip_levels_header"));
    for (u32 mip = 0; mip < texture.mipCount(); ++mip) {
        const auto& ml = texture.mipLevel(mip);
        char label[64];
        std::snprintf(label, sizeof(label), tr("details.mip_list_entry"), mip, ml.width, ml.height);
        if (ImGui::Selectable(label, selected_mip == static_cast<i32>(mip))) {
            commands.push_back(SelectMipCmd{static_cast<i32>(mip)});
        }
    }
    ImGui::EndChild();

    return commands;
}

void ImageDetails::setPipelines(
    std::vector<models::PipelineInfo> pipelines,
    std::unordered_map<std::string, std::vector<models::PipelineParam>> params) {
    pipelines_ = std::move(pipelines);
    pipeline_params_ = std::move(params);
    // Keep the current selection if it still exists; otherwise clear it.
    const bool still_present =
        std::any_of(pipelines_.begin(), pipelines_.end(),
                    [&](const models::PipelineInfo& p) { return p.file == selected_pipeline_file_; });
    if (!still_present)
        selectPipeline("");
    else
        selectPipeline(selected_pipeline_file_); // refresh params to new defaults
}

void ImageDetails::selectPipeline(const std::string& file) {
    selected_pipeline_file_ = file;
    param_values_.clear();
    if (const auto it = pipeline_params_.find(file); it != pipeline_params_.end())
        for (const auto& pp : it->second)
            param_values_.push_back(pp.default_value);
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

} // namespace whiteout::textool::views
