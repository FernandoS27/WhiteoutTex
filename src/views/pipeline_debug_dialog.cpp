// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/pipeline_debug_dialog.h"

#include "localization.h"
#include "save_dialog.h"   // centerNextWindow
#include "save_helpers.h"  // dialogFiltersFor

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tex = whiteout::textures;

namespace whiteout::textool::views {

using i18n::tr;

namespace {

void SDLCALL dbgDialogCallback(void* userdata, const char* const* filelist, int filter) {
    auto* st = static_cast<models::FileDialogState*>(userdata);
    std::lock_guard<std::mutex> lk(st->mtx);
    if (filelist && filelist[0]) {
        st->pending_path = filelist[0];
        st->pending_filter = filter;
        st->has_pending = true;
    }
}

// Upload an RGBA8/R8 texture's mip 0 to an SDL texture for preview.
SDL_Texture* makePreview(SDL_Renderer* renderer, const tex::Texture& image) {
    tex::Texture rgba =
        image.format() == tex::PixelFormat::RGBA8 ? image : image.copyAsFormat(tex::PixelFormat::RGBA8);
    const int w = static_cast<int>(rgba.width()), h = static_cast<int>(rgba.height());
    if (w <= 0 || h <= 0)
        return nullptr;
    SDL_Texture* t =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        std::span<const u8> d = rgba.mipData(0);
        SDL_UpdateTexture(t, nullptr, d.data(), w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

} // namespace

void PipelineDebugDialog::open(std::string display_name, const pipeline::PipelineInterface& iface) {
    display_name_ = std::move(display_name);
    inputs_.clear();
    for (const auto& p : iface.inputs) {
        DebugInputRow row;
        row.name = p.name;
        row.type = p.type;
        inputs_.push_back(std::move(row));
    }
    has_result_ = false;
    result_ = {};
    releasePreviews();
    run_requested_ = false;
    active_input_ = -1;
    open_ = true;
    show_requested_ = true;
}

void PipelineDebugDialog::close() {
    open_ = false;
    active_input_ = -1;
    releasePreviews();
}

void PipelineDebugDialog::releasePreviews() {
    for (SDL_Texture* t : previews_)
        if (t)
            SDL_DestroyTexture(t);
    previews_.clear();
    previews_built_ = false;
}

bool PipelineDebugDialog::takeRunRequest() {
    if (!run_requested_)
        return false;
    run_requested_ = false;
    return true;
}

void PipelineDebugDialog::setResult(services::PipelineDebugResult result) {
    releasePreviews();
    result_ = std::move(result);
    has_result_ = true;
}

void PipelineDebugDialog::consumeDialogResult() {
    if (!dialog_state_.has_pending.load())
        return;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(dialog_state_.mtx);
        path = dialog_state_.pending_path;
        dialog_state_.has_pending = false;
    }
    if (active_input_ >= 0 && active_input_ < static_cast<int>(inputs_.size()))
        inputs_[active_input_].path = path;
    active_input_ = -1;
}

void PipelineDebugDialog::draw(SDL_Window* window, SDL_Renderer* renderer) {
    if (!open_)
        return;
    consumeDialogResult();

    constexpr const char* kId = "Debug Pipeline###pipelinedebug";
    if (show_requested_) {
        ImGui::OpenPopup(kId);
        show_requested_ = false;
    }
    centerNextWindow();
    ImGui::SetNextWindowSize(ImVec2(540.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kId, &open_, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!open_) {
            active_input_ = -1;
            releasePreviews();
        }
        return;
    }

    ImGui::TextUnformatted(display_name_.empty() ? "(unnamed)" : display_name_.c_str());

    // ── Inputs ──────────────────────────────────────────────────────────
    if (!inputs_.empty())
        ImGui::SeparatorText(tr("debug.inputs"));
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        DebugInputRow& row = inputs_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::SameLine(180.0f);
        ImGui::SetNextItemWidth(240.0f);
        if (isImagePinType(row.type)) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", row.path.c_str());
            if (ImGui::InputText("##p", buf, sizeof(buf)))
                row.path = buf;
            ImGui::SameLine();
            if (ImGui::Button(tr("multipipeline.browse"))) {
                active_input_ = static_cast<int>(i);
                const auto& filters = dialogFiltersFor(tex::FmtCap::Read, true, true);
                SDL_ShowOpenFileDialog(dbgDialogCallback, &dialog_state_, window, filters.data(),
                                       static_cast<int>(filters.size()), nullptr, false);
            }
        } else if (row.type == pipeline::PinType::Int) {
            int v = static_cast<int>(row.integer);
            if (ImGui::InputInt("##i", &v))
                row.integer = v;
        } else { // Real / Number
            float v = static_cast<float>(row.real);
            if (ImGui::InputFloat("##r", &v))
                row.real = v;
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button(tr("pipeline.debug"), ImVec2(120.0f, 0.0f)))
        run_requested_ = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("multipipeline.close"), ImVec2(120.0f, 0.0f))) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    // ── Results ─────────────────────────────────────────────────────────
    if (has_result_) {
        // Build image previews once (main thread; renderer available).
        if (!previews_built_) {
            previews_built_ = true;
            previews_.assign(result_.outputs.size(), nullptr);
            for (std::size_t i = 0; i < result_.outputs.size(); ++i)
                if (result_.outputs[i].second.image)
                    previews_[i] = makePreview(renderer, *result_.outputs[i].second.image);
        }

        ImGui::SeparatorText(tr("debug.results"));
        if (result_.outputs.empty())
            ImGui::TextDisabled("%s", tr("debug.no_outputs"));
        for (std::size_t i = 0; i < result_.outputs.size(); ++i) {
            const auto& [name, val] = result_.outputs[i];
            if (val.integer) {
                ImGui::Text("%s = %lld", name.c_str(), static_cast<long long>(*val.integer));
            } else if (val.real) {
                ImGui::Text("%s = %g", name.c_str(), *val.real);
            } else if (val.image) {
                const auto& img = *val.image;
                ImGui::Text("%s: %u x %u", name.c_str(), img.width(), img.height());
                if (SDL_Texture* t = previews_[i]) {
                    const float maxw = 220.0f;
                    const float aspect =
                        img.height() > 0 ? static_cast<float>(img.width()) / img.height() : 1.0f;
                    const float w = std::min(maxw, static_cast<float>(img.width()));
                    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(t)),
                                 ImVec2(w, w / std::max(0.01f, aspect)));
                }
            } else {
                ImGui::Text("%s: %s", name.c_str(), tr("debug.no_value"));
            }
        }
        for (const auto& e : result_.errors)
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.4f, 1.0f), "- %s", e.c_str());
    }

    ImGui::EndPopup();
    if (!open_) {
        active_input_ = -1;
        releasePreviews();
    }
}

} // namespace whiteout::textool::views
