// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/multi_pipeline_dialog.h"

#include "localization.h"
#include "save_dialog.h"
#include "save_helpers.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

namespace whiteout::textool::views {

using i18n::tr;

namespace {

// SDL file-dialog callback: stash the chosen path for the next frame to consume.
void SDLCALL mpDialogCallback(void* userdata, const char* const* filelist, int filter) {
    auto* st = static_cast<models::FileDialogState*>(userdata);
    std::lock_guard<std::mutex> lk(st->mtx);
    if (filelist && filelist[0]) {
        st->pending_path = filelist[0];
        st->pending_filter = filter;
        st->has_pending = true;
    }
}

// Per-output format-specific options (mirrors the Save dialog, compactly).
void drawOutputOptions(MultiOutputRow& row, const std::string& id_suffix) {
    const TFF fmt = TC::classifyPath(row.path);
    ImGui::TextDisabled("%s", TC::fileFormatName(fmt));

    ImGui::PushID(id_suffix.c_str());
    switch (fmt) {
    case TFF::BLP:
        drawBlpOptionsUI(row.prefs.blp_version, row.prefs.blp_encoding, row.prefs.blp_dither,
                         row.prefs.blp_dither_strength, row.prefs.jpeg_quality,
                         row.prefs.jpeg_progressive);
        break;
    case TFF::DDS: {
        ImGui::SeparatorText(tr("save.dds_options"));
        const auto kind = static_cast<tex::TextureKind>(row.kind);
        const i32* allowed;
        i32 count;
        ddsPresetForKind(kind, allowed, count);
        validateDdsFormat(kind, row.prefs.dds_format);
        if (ImGui::BeginCombo(tr("save.pixel_format"), DDS_FORMAT_NAMES[row.prefs.dds_format])) {
            for (i32 i = 0; i < count; ++i) {
                const bool sel = row.prefs.dds_format == allowed[i];
                if (ImGui::Selectable(DDS_FORMAT_NAMES[allowed[i]], sel))
                    row.prefs.dds_format = allowed[i];
            }
            ImGui::EndCombo();
        }
        if (kind == tex::TextureKind::Normal)
            ImGui::Checkbox(tr("save.invert_y"), &row.prefs.dds_invert_y);
        break;
    }
    case TFF::JPEG:
        ImGui::SeparatorText(tr("save.jpeg_options"));
        ImGui::SliderInt(tr("save.quality"), &row.prefs.jpeg_quality, 1, 100);
        ImGui::Checkbox(tr("save.progressive"), &row.prefs.jpeg_progressive);
        break;
    default:
        break;
    }

    // Common options: texture kind + mipmaps.
    ImGui::SeparatorText(tr("save.common_options"));
    {
        const auto cur = static_cast<tex::TextureKind>(row.kind);
        if (ImGui::BeginCombo(tr("save.texture_kind"), textureKindName(cur))) {
            for (i32 i = 0; i < kSelectableKindCount; ++i) {
                const bool sel = kSelectableKinds[i].kind == cur;
                if (ImGui::Selectable(kindLabel(kSelectableKinds[i]), sel))
                    row.kind = static_cast<i32>(kSelectableKinds[i].kind);
            }
            ImGui::EndCombo();
        }
    }
    drawMipmapModeUI(row.prefs.generate_mipmaps, row.prefs.mipmap_mode,
                     row.prefs.mipmap_custom_count);
    ImGui::PopID();
}

} // namespace

void MultiPipelineDialog::open(
    std::string file, std::string display_name,
    const std::vector<std::pair<std::string, std::string>>& image_inputs,
    const std::vector<std::pair<std::string, std::string>>& image_outputs,
    const SavePrefs& default_prefs) {
    file_ = std::move(file);
    display_name_ = std::move(display_name);
    inputs_.clear();
    outputs_.clear();
    for (const auto& [name, label] : image_inputs)
        inputs_.push_back({name, label, {}});
    for (const auto& [name, label] : image_outputs)
        outputs_.push_back({name, label, {}, default_prefs, 0});
    result_message_.clear();
    result_ok_ = false;
    run_requested_ = false;
    active_target_ = -1;
    open_ = true;
    show_requested_ = true;
}

void MultiPipelineDialog::close() {
    open_ = false;
    active_target_ = -1;
}

bool MultiPipelineDialog::allPathsSet() const {
    for (const auto& i : inputs_)
        if (i.path.empty())
            return false;
    for (const auto& o : outputs_)
        if (o.path.empty())
            return false;
    return !inputs_.empty() || !outputs_.empty();
}

bool MultiPipelineDialog::takeRunRequest() {
    if (!run_requested_)
        return false;
    run_requested_ = false;
    return true;
}

void MultiPipelineDialog::finishRun(std::string message, bool ok) {
    result_message_ = std::move(message);
    result_ok_ = ok;
}

void MultiPipelineDialog::consumeDialogResult() {
    if (!dialog_state_.has_pending.load())
        return;
    std::string path;
    int filter = -1;
    {
        std::lock_guard<std::mutex> lk(dialog_state_.mtx);
        path = dialog_state_.pending_path;
        filter = dialog_state_.pending_filter;
        dialog_state_.has_pending = false;
    }
    if (active_target_ < 0)
        return;
    const int in_count = static_cast<int>(inputs_.size());
    if (active_target_ < in_count) {
        inputs_[active_target_].path = path;
    } else if (active_target_ - in_count < static_cast<int>(outputs_.size())) {
        // Append the chosen save filter's extension when the path lacks one
        // (matches the Save dialog's behaviour).
        if (std::filesystem::path(path).extension().empty()) {
            const auto& filters = dialogFiltersFor(tex::FmtCap::Write, false, false);
            if (filter >= 0 && filter < static_cast<int>(filters.size())) {
                std::string_view pat = filters[filter].pattern;
                const auto sep = pat.find(';');
                path += '.';
                path += std::string(sep == std::string_view::npos ? pat : pat.substr(0, sep));
            }
        }
        outputs_[active_target_ - in_count].path = path;
    }
    active_target_ = -1;
}

void MultiPipelineDialog::draw(SDL_Window* window) {
    if (!open_)
        return;
    consumeDialogResult();

    constexpr const char* kId = "MultiPipeline###multipipeline";
    if (show_requested_) {
        ImGui::OpenPopup(kId);
        show_requested_ = false;
    }
    centerNextWindow();
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kId, &open_, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!open_)
            active_target_ = -1;
        return;
    }

    ImGui::TextUnformatted(display_name_.c_str());
    ImGui::Separator();

    const auto pathField = [&](const char* label, std::string& path, int target, bool save) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(160.0f);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", path.c_str());
        ImGui::SetNextItemWidth(280.0f);
        ImGui::PushID(target);
        if (ImGui::InputText("##path", buf, sizeof(buf)))
            path = buf;
        ImGui::SameLine();
        if (ImGui::Button(tr("multipipeline.browse"))) {
            active_target_ = target;
            const auto& filters =
                save ? dialogFiltersFor(tex::FmtCap::Write, false, false)
                     : dialogFiltersFor(tex::FmtCap::Read, true, true);
            if (save)
                SDL_ShowSaveFileDialog(mpDialogCallback, &dialog_state_, window, filters.data(),
                                       static_cast<int>(filters.size()), nullptr);
            else
                SDL_ShowOpenFileDialog(mpDialogCallback, &dialog_state_, window, filters.data(),
                                       static_cast<int>(filters.size()), nullptr, false);
        }
        ImGui::PopID();
    };

    if (!inputs_.empty())
        ImGui::SeparatorText(tr("multipipeline.inputs"));
    for (std::size_t i = 0; i < inputs_.size(); ++i)
        pathField(inputs_[i].label.empty() ? inputs_[i].port.c_str() : inputs_[i].label.c_str(),
                  inputs_[i].path, static_cast<int>(i), /*save=*/false);

    const int in_count = static_cast<int>(inputs_.size());
    if (!outputs_.empty())
        ImGui::SeparatorText(tr("multipipeline.outputs"));
    for (std::size_t j = 0; j < outputs_.size(); ++j) {
        pathField(outputs_[j].label.empty() ? outputs_[j].port.c_str()
                                            : outputs_[j].label.c_str(),
                  outputs_[j].path, in_count + static_cast<int>(j),
                  /*save=*/true);
        if (!outputs_[j].path.empty()) {
            ImGui::Indent();
            drawOutputOptions(outputs_[j], "out" + std::to_string(j));
            ImGui::Unindent();
        }
    }

    ImGui::Separator();
    const bool can_run = allPathsSet();
    if (!can_run)
        ImGui::BeginDisabled();
    if (ImGui::Button(tr("multipipeline.run"), ImVec2(120, 0))) {
        result_message_.clear();
        run_requested_ = true;
    }
    if (!can_run)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("multipipeline.close"), ImVec2(120, 0))) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    if (!result_message_.empty()) {
        ImGui::Separator();
        const ImVec4 col = result_ok_ ? ImVec4(0.4f, 0.85f, 0.45f, 1.0f)
                                      : ImVec4(0.9f, 0.45f, 0.4f, 1.0f);
        ImGui::TextColored(col, "%s", result_message_.c_str());
    }

    ImGui::EndPopup();
    if (!open_)
        active_target_ = -1;
}

} // namespace whiteout::textool::views
