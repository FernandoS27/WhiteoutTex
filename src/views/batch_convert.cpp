// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "batch_convert.h"
#include "common_types.h"
#include "localization.h"
#include "save_dialog.h"
#include "save_helpers.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

using whiteout::textool::views::BLP_ENCODING_NAMES;
using whiteout::textool::views::DDS_FORMAT_NAMES;

namespace {

using whiteout::i32;

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;

// ── Output format table (registry-driven) ──────────────────────────────

/// Output formats offered in batch mode, in registry (FmtCap::BatchOut) order.
/// The index lines up with BatchPrefs::output_format, which is persisted in the
/// INI — so the slice order must stay stable (enforced by format_registry.cpp).
const std::vector<const char*>& outputFormatNames() {
    static const std::vector<const char*> names = [] {
        std::vector<const char*> v;
        for (const auto& row : tex::formatTable())
            if (row.has(tex::FmtCap::BatchOut))
                v.push_back(row.shortName);
        return v;
    }();
    return names;
}

// ── BLP / DDS name arrays live in save_dialog.h

// ── Helpers ────────────────────────────────────────────────────────────

bool matchesFilter(const std::string& ext, const whiteout::textool::BatchPrefs& p) {
    const auto fmt = tex::classifyExtension(ext);
    if (!tex::formatHasCap(fmt, tex::FmtCap::BatchIn))
        return false;
    const bool* flag =
        whiteout::textool::batch_filter_flag(const_cast<whiteout::textool::BatchPrefs&>(p), fmt);
    return flag && *flag;
}

void drawDdsFormatCombo(const char* label, i32& fmt, const i32* allowed, i32 count) {
    whiteout::textool::views::validateDdsFormatRaw(fmt, allowed, count);
    if (ImGui::BeginCombo(label, DDS_FORMAT_NAMES[fmt])) {
        for (i32 i = 0; i < count; ++i) {
            bool selected = (fmt == allowed[i]);
            if (ImGui::Selectable(DDS_FORMAT_NAMES[allowed[i]], selected))
                fmt = allowed[i];
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

} // anonymous namespace

namespace whiteout::textool::views {

using namespace models;
using namespace services;
using i18n::tr;

// ============================================================================
// Static callback
// ============================================================================

void SDLCALL BatchConvert::folderDialogCallback(void* userdata, const char* const* filelist,
                                                i32 /*filter*/) {
    if (!filelist || !filelist[0])
        return;
    auto* state = static_cast<FolderState*>(userdata);
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->has_pending.store(true);
}

// ============================================================================
// Open
// ============================================================================

void BatchConvert::open(const BatchPrefs& prefs) {
    applyPrefs(prefs);
    show_dialog_ = true;
}

// ============================================================================
// Folder result processing
// ============================================================================

void BatchConvert::processFolderResults() {
    consumeFolderResult(input_folder_state_, input_dir_buf_);
    consumeFolderResult(output_folder_state_, output_dir_buf_);
}

// ============================================================================
// Draw
// ============================================================================

std::vector<AppCommand> BatchConvert::draw(SDL_Window* window, BatchPrefs& prefs,
                                           RecentPaths& recent_input_dirs,
                                           RecentPaths& recent_output_dirs) {
    std::vector<AppCommand> commands;

    processFolderResults();

    if (show_dialog_) {
        ImGui::OpenPopup(tr("batch.title"));
    }
    centerNextWindow();
    if (!ImGui::BeginPopupModal(tr("batch.title"), &show_dialog_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        drawProgressDialog(commands);
        return commands;
    }

    // ── Input ──────────────────────────────────────────────────────────

    ImGui::SeparatorText(tr("batch.input"));

    ImGui::TextUnformatted(tr("batch.directory"));
    ImGui::SetNextItemWidth(500.0f);
    if (ImGui::BeginCombo("##input_dir", input_dir_buf_, ImGuiComboFlags_HeightLarge)) {
        for (const auto& p : recent_input_dirs.paths) {
            const bool selected = (p == input_dir_buf_);
            if (ImGui::Selectable(p.c_str(), selected))
                copyToBuffer(input_dir_buf_, p);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("batch.browse##input"))) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &input_folder_state_, window,
                                 input_dir_buf_[0] ? input_dir_buf_ : nullptr, false);
    }

    ImGui::TextUnformatted(tr("batch.read_formats"));
    // One checkbox per BatchIn-capable format, 4 per row, driven by the registry.
    i32 shown = 0;
    for (const auto& row : tex::formatTable()) {
        if (!row.has(tex::FmtCap::BatchIn))
            continue;
        bool* flag = whiteout::textool::batch_filter_flag(prefs_, row.fmt);
        if (!flag)
            continue;
        if (shown % 4 != 0)
            ImGui::SameLine();
        ImGui::Checkbox(row.shortName, flag);
        ++shown;
    }

    ImGui::Spacing();
    ImGui::Checkbox(tr("batch.scan_subdirectories"), &prefs_.recursive);
    ImGui::Checkbox(tr("batch.keep_directory_layout"), &prefs_.keep_layout);

    // ── Output ─────────────────────────────────────────────────────────

    ImGui::SeparatorText(tr("batch.output"));

    ImGui::TextUnformatted(tr("batch.directory"));
    ImGui::SetNextItemWidth(500.0f);
    if (ImGui::BeginCombo("##output_dir", output_dir_buf_, ImGuiComboFlags_HeightLarge)) {
        for (const auto& p : recent_output_dirs.paths) {
            const bool selected = (p == output_dir_buf_);
            if (ImGui::Selectable(p.c_str(), selected))
                copyToBuffer(output_dir_buf_, p);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("batch.browse##output"))) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &output_folder_state_, window,
                                 output_dir_buf_[0] ? output_dir_buf_ : nullptr, false);
    }
    if (output_dir_buf_[0] != '\0' && std::strcmp(input_dir_buf_, output_dir_buf_) == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.0f, 1.0f));
        ImGui::TextWrapped("%s", tr("batch.same_folder_warning"));
        ImGui::PopStyleColor();
    }

    ImGui::Combo(tr("batch.format"), &prefs_.output_format, outputFormatNames().data(),
                 static_cast<i32>(outputFormatNames().size()));

    // ── Format-specific options ────────────────────────────────────────

    switch (tex::capSliceAt(tex::FmtCap::BatchOut, prefs_.output_format)) {
    case TFF::BLP:
        drawBlpOptions();
        break;

    case TFF::DDS:
        drawDdsOptions();
        break;

    case TFF::JPEG:
        ImGui::SeparatorText(tr("batch.jpeg_options"));
        ImGui::SliderInt(tr("batch.quality"), &prefs_.jpeg_quality, 1, 100);
        ImGui::Checkbox(tr("batch.progressive"), &prefs_.jpeg_progressive);
        break;

    default:
        break;
    }

    // ── Common options ─────────────────────────────────────────────────

    ImGui::SeparatorText(tr("batch.common_options"));
    drawMipmapModeUI(prefs_.generate_mipmaps, prefs_.mipmap_mode, prefs_.mipmap_custom_count);

    // ── Transformation pipeline ────────────────────────────────────────

    drawTransformPipeline();

    // ── Buttons ────────────────────────────────────────────────────────

    ImGui::Separator();
    const bool can_convert = input_dir_buf_[0] != '\0' && output_dir_buf_[0] != '\0';
    if (!can_convert)
        ImGui::BeginDisabled();
    if (ImGui::Button(tr("batch.convert"), ImVec2(120, 0))) {
        persistPrefs(prefs);
        std::string err = beginBatch();
        if (err.empty()) {
            recent_input_dirs.push(std::string(input_dir_buf_));
            recent_output_dirs.push(std::string(output_dir_buf_));
        } else {
            commands.push_back(ShowResultPopupCmd{std::move(err), false});
        }
        show_dialog_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (!can_convert)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(tr("batch.cancel"), ImVec2(120, 0))) {
        show_dialog_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    drawProgressDialog(commands);
    return commands;
}

// ============================================================================
// Format-specific option panels
// ============================================================================

void BatchConvert::drawBlpOptions() {
    drawBlpOptionsUI(prefs_.blp_version, prefs_.blp_encoding, prefs_.blp_dither,
                     prefs_.blp_dither_strength, prefs_.jpeg_quality, prefs_.jpeg_progressive);
}

void BatchConvert::drawDdsOptions() {
    ImGui::SeparatorText(tr("batch.dds_options"));
    ImGui::RadioButton(tr("batch.general"), &prefs_.dds_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(tr("batch.per_kind_group"), &prefs_.dds_mode, 1);
    ImGui::Spacing();

    if (prefs_.dds_mode == 0) {
        drawDdsFormatCombo(tr("batch.pixel_format"), prefs_.dds_format_general, DDS_ALL,
                           static_cast<i32>(std::size(DDS_ALL)));
        ImGui::Checkbox(tr("batch.invert_y_channel"), &prefs_.dds_invert_y_general);
    } else {
        ImGui::TextDisabled("%s", tr("batch.normal_maps"));
        ImGui::Indent();
        drawDdsFormatCombo(tr("batch.pixel_format##normal"), prefs_.dds_format_normal, DDS_PRESET_NORMAL,
                           static_cast<i32>(std::size(DDS_PRESET_NORMAL)));
        ImGui::Checkbox(tr("batch.invert_y_channel##normal"), &prefs_.dds_invert_y_normal);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("batch.channel_maps"));
        ImGui::Indent();
        drawDdsFormatCombo(tr("batch.pixel_format##channel"), prefs_.dds_format_channel, DDS_PRESET_CHANNEL,
                           static_cast<i32>(std::size(DDS_PRESET_CHANNEL)));
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("batch.other"));
        ImGui::Indent();
        drawDdsFormatCombo(tr("batch.pixel_format##other"), prefs_.dds_format_other, DDS_PRESET_OTHER,
                           static_cast<i32>(std::size(DDS_PRESET_OTHER)));
        ImGui::Unindent();
    }
}

// ============================================================================
// Transformation pipeline UI
// ============================================================================

void BatchConvert::drawTransformPipeline() {
    ImGui::SeparatorText(tr("batch.transformations"));
    ImGui::TextDisabled("%s", tr("batch.steps_applied_in_order"));

    // Draw each step with controls
    i32 remove_idx = -1;
    i32 swap_up_idx = -1;
    i32 swap_down_idx = -1;

    for (i32 i = 0; i < static_cast<i32>(prefs_.transform_pipeline.size()); ++i) {
        auto& step = prefs_.transform_pipeline[i];
        ImGui::PushID(i);

        // Step header with reorder/remove buttons
        char label[64];
        std::snprintf(label, sizeof(label), tr("batch.step"), i + 1);
        ImGui::Text("%s:", label);
        ImGui::SameLine();

        if (i == 0)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton(tr("batch.up")))
            swap_up_idx = i;
        if (i == 0)
            ImGui::EndDisabled();

        ImGui::SameLine();

        if (i == static_cast<i32>(prefs_.transform_pipeline.size()) - 1)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton(tr("batch.down")))
            swap_down_idx = i;
        if (i == static_cast<i32>(prefs_.transform_pipeline.size()) - 1)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::SmallButton(tr("batch.remove")))
            remove_idx = i;

        ImGui::Indent();

        // Transform type selector
        i32 type_int = static_cast<i32>(step.type);
        const char* type_names[] = {tr("batch.upscale"), tr("batch.downscale")};

#ifdef WHITEOUT_HAS_UPSCALER
        const i32 type_count = 2;
#else
        const i32 type_count = 1; // Only Downscale available without upscaler
        if (type_int == 0)
            type_int = 1; // Force to Downscale
#endif

        ImGui::SetNextItemWidth(200.0f);

#ifdef WHITEOUT_HAS_UPSCALER
        if (ImGui::Combo(tr("batch.type"), &type_int, type_names, type_count))
            step.type = static_cast<TransformType>(type_int);
#else
        // Only show Downscale
        if (ImGui::Combo(tr("batch.type"), &type_int, &type_names[1], 1))
            step.type = TransformType::Downscale;
#endif

        // Type-specific options
        if (step.type == TransformType::Downscale) {
            i32 lvl_idx = step.downscale_levels - 1;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo(tr("batch.scale"), &lvl_idx, kDownscaleOptions, kDownscaleOptionCount)) {
                step.downscale_levels = lvl_idx + 1;
            }
        }

#ifdef WHITEOUT_HAS_UPSCALER
        if (step.type == TransformType::Upscale) {
            if (upscale_models_.empty()) {
                ImGui::TextWrapped("%s", tr("batch.no_upscaler_models"));
            } else {
                if (step.upscale_model_index >= static_cast<i32>(upscale_models_.size()))
                    step.upscale_model_index = 0;
                ImGui::SetNextItemWidth(300.0f);
                const auto& model = upscale_models_[step.upscale_model_index];
                std::string combo_label = model.label();
                if (ImGui::BeginCombo(tr("batch.model"), combo_label.c_str())) {
                    for (i32 m = 0; m < static_cast<i32>(upscale_models_.size()); ++m) {
                        bool selected = (m == step.upscale_model_index);
                        std::string ml = upscale_models_[m].label();
                        if (ImGui::Selectable(ml.c_str(), selected))
                            step.upscale_model_index = m;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::Checkbox(tr("batch.upscale_alpha"), &step.upscale_alpha);
            }
        }
#endif

        ImGui::Unindent();
        ImGui::PopID();

        if (i < static_cast<i32>(prefs_.transform_pipeline.size()) - 1)
            ImGui::Spacing();
    }

    // Apply deferred operations
    if (remove_idx >= 0)
        prefs_.transform_pipeline.erase(prefs_.transform_pipeline.begin() + remove_idx);
    if (swap_up_idx > 0)
        std::swap(prefs_.transform_pipeline[swap_up_idx],
                  prefs_.transform_pipeline[swap_up_idx - 1]);
    if (swap_down_idx >= 0 &&
        swap_down_idx < static_cast<i32>(prefs_.transform_pipeline.size()) - 1)
        std::swap(prefs_.transform_pipeline[swap_down_idx],
                  prefs_.transform_pipeline[swap_down_idx + 1]);

    // Add step button
    if (ImGui::Button(tr("batch.add_transformation"))) {
        TransformStep new_step;
#ifdef WHITEOUT_HAS_UPSCALER
        new_step.type = TransformType::Upscale;
#else
        new_step.type = TransformType::Downscale;
#endif
        prefs_.transform_pipeline.push_back(new_step);
    }
}

#ifdef WHITEOUT_HAS_UPSCALER
void BatchConvert::setUpscalerModels(std::vector<UpscalerModel> models) {
    upscale_models_ = std::move(models);
}
#endif

// ============================================================================
// Progress dialog
// ============================================================================

void BatchConvert::drawProgressDialog(std::vector<AppCommand>& commands) {
    if (batch_service_.isRunning()) {
        ImGui::OpenPopup("##BatchProgress");
    }
    centerNextWindow();
    ImGui::SetNextWindowSize(ImVec2(500, 0));
    if (ImGui::BeginPopupModal("##BatchProgress", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize)) {
        const auto prog = batch_service_.progress();
        const f32 fraction = prog.total > 0 ? static_cast<f32>(prog.processed) / prog.total : 0.0f;

        ImGui::Text(tr("batch.converting_progress"), prog.processed, prog.total);
        ImGui::ProgressBar(fraction, ImVec2(-1, 0));

        if (prog.done) {
            batch_service_.joinWorkers();

            char msg[256];
            std::snprintf(msg, sizeof(msg), tr("batch.complete"), prog.success, prog.fail,
                          prog.total);
            commands.push_back(ShowResultPopupCmd{msg, prog.fail == 0});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// Batch execution
// ============================================================================

std::string BatchConvert::beginBatch() {
    namespace fs = std::filesystem;

    if (batch_service_.isRunning())
        return tr("batch.err_already_running");

    const std::string input_dir(input_dir_buf_);
    const std::string output_dir(output_dir_buf_);

    if (input_dir.empty())
        return tr("batch.err_no_input_dir");
    if (output_dir.empty())
        return tr("batch.err_no_output_dir");

    std::error_code ec;
    if (!fs::is_directory(input_dir, ec))
        return tr("batch.err_input_not_exist");
    fs::create_directories(output_dir, ec);
    if (!fs::is_directory(output_dir, ec))
        return tr("batch.err_create_output");

    // Collect matching files
    std::vector<std::string> files;
    auto collect = [&](auto& it) {
        for (const auto& entry : it) {
            if (!entry.is_regular_file())
                continue;
            std::string ext = to_lower(entry.path().extension().string());
            if (matchesFilter(ext, prefs_)) {
                files.push_back(entry.path().string());
            }
        }
    };
    if (prefs_.recursive) {
        auto it = fs::recursive_directory_iterator(input_dir, ec);
        collect(it);
    } else {
        auto it = fs::directory_iterator(input_dir, ec);
        collect(it);
    }

    if (files.empty())
        return tr("batch.err_no_matching_files");

    BatchJob job;
    job.input_dir = input_dir;
    job.output_dir = output_dir;
    job.files = std::move(files);
    job.prefs = prefs_;
#ifdef WHITEOUT_HAS_UPSCALER
    job.upscale_models = upscale_models_;
#endif

    return batch_service_.start(std::move(job));
}

// ============================================================================
// Preferences
// ============================================================================

void BatchConvert::applyPrefs(const BatchPrefs& prefs) {
    prefs_ = prefs;
    copyToBuffer(input_dir_buf_, prefs.last_input_dir);
    copyToBuffer(output_dir_buf_, prefs.last_output_dir);
}

void BatchConvert::persistPrefs(BatchPrefs& prefs) const {
    prefs = prefs_;
    prefs.last_input_dir = input_dir_buf_;
    prefs.last_output_dir = output_dir_buf_;
}

} // namespace whiteout::textool::views
