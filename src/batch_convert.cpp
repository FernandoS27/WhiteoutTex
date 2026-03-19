// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "batch_convert.h"
#include "common_types.h"
#include "save_dialog.h"
#include "save_helpers.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <whiteout/textures/blp/types.h>

#include <imgui.h>

namespace tex = whiteout::textures;
namespace interfaces = whiteout::interfaces;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;
using whiteout::gui::BLP_ENCODING_NAMES;
using whiteout::gui::DDS_FORMAT_NAMES;

namespace {

using whiteout::i32;

// ── Output format table ────────────────────────────────────────────────

constexpr const char* OUTPUT_FORMAT_NAMES[] = {"BLP", "BMP", "DDS", "JPEG", "PNG", "TGA"};
constexpr const char* OUTPUT_EXTENSIONS[] = {".blp", ".bmp", ".dds", ".jpg", ".png", ".tga"};

// ── BLP / DDS name arrays live in save_dialog.h

// ── Helpers ────────────────────────────────────────────────────────────

static bool matchesFilter(const std::string& ext, const whiteout::gui::BatchPrefs& p) {
    if (ext == ".blp") return p.filter_blp;
    if (ext == ".bmp") return p.filter_bmp;
    if (ext == ".dds") return p.filter_dds;
    if (ext == ".jpg" || ext == ".jpeg") return p.filter_jpeg;
    if (ext == ".png") return p.filter_png;
    if (ext == ".tex") return p.filter_tex;
    if (ext == ".tga") return p.filter_tga;
    return false;
}

static void drawDdsFormatCombo(const char* label, i32& fmt, const i32* allowed, i32 count) {
    whiteout::gui::validateDdsFormatRaw(fmt, allowed, count);
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

namespace whiteout::gui {

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

std::string BatchConvert::draw(SDL_Window* window, BatchPrefs& prefs,
                               RecentPaths& recent_input_dirs,
                               RecentPaths& recent_output_dirs) {
    std::string status;

    processFolderResults();

    if (show_dialog_) {
        ImGui::OpenPopup("Batch Convert");
    }
    centerNextWindow();
    if (!ImGui::BeginPopupModal("Batch Convert", &show_dialog_,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        drawProgressDialog(status);
        return status;
    }

    // ── Input ──────────────────────────────────────────────────────────

    ImGui::SeparatorText("Input");

    ImGui::Text("Directory:");
    ImGui::SetNextItemWidth(500.0f);
    if (ImGui::BeginCombo("##input_dir", input_dir_buf_,
                          ImGuiComboFlags_HeightLarge)) {
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
    if (ImGui::Button("Browse##input")) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &input_folder_state_, window,
                                 input_dir_buf_[0] ? input_dir_buf_ : nullptr, false);
    }

    ImGui::Text("Read Formats:");
    ImGui::Checkbox("BLP", &prefs_.filter_blp);
    ImGui::SameLine();
    ImGui::Checkbox("BMP", &prefs_.filter_bmp);
    ImGui::SameLine();
    ImGui::Checkbox("DDS", &prefs_.filter_dds);
    ImGui::SameLine();
    ImGui::Checkbox("JPEG", &prefs_.filter_jpeg);
    ImGui::Checkbox("PNG", &prefs_.filter_png);
    ImGui::SameLine();
    ImGui::Checkbox("TEX", &prefs_.filter_tex);
    ImGui::SameLine();
    ImGui::Checkbox("TGA", &prefs_.filter_tga);

    ImGui::Spacing();
    ImGui::Checkbox("Scan subdirectories", &prefs_.recursive);
    ImGui::Checkbox("Keep directory layout", &prefs_.keep_layout);

    // ── Output ─────────────────────────────────────────────────────────

    ImGui::SeparatorText("Output");

    ImGui::Text("Directory:");
    ImGui::SetNextItemWidth(500.0f);
    if (ImGui::BeginCombo("##output_dir", output_dir_buf_,
                          ImGuiComboFlags_HeightLarge)) {
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
    if (ImGui::Button("Browse##output")) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &output_folder_state_, window,
                                 output_dir_buf_[0] ? output_dir_buf_ : nullptr, false);
    }

    ImGui::Combo("Format", &prefs_.output_format, OUTPUT_FORMAT_NAMES,
                 static_cast<i32>(std::size(OUTPUT_FORMAT_NAMES)));

    // ── Format-specific options ────────────────────────────────────────

    switch (prefs_.output_format) {
    case 0: // BLP
        drawBlpOptions();
        break;

    case 2: // DDS
        drawDdsOptions();
        break;

    case 3: // JPEG
        ImGui::SeparatorText("JPEG Options");
        ImGui::SliderInt("Quality", &prefs_.jpeg_quality, 1, 100);
        break;

    default:
        break;
    }

    // ── Common options ─────────────────────────────────────────────────

    ImGui::SeparatorText("Common Options");
    drawMipmapModeUI(prefs_.generate_mipmaps, prefs_.mipmap_mode, prefs_.mipmap_custom_count);

    // ── Transformation pipeline ────────────────────────────────────────

    drawTransformPipeline();

    // ── Buttons ────────────────────────────────────────────────────────

    ImGui::Separator();
    const bool can_convert = input_dir_buf_[0] != '\0' && output_dir_buf_[0] != '\0';
    if (!can_convert)
        ImGui::BeginDisabled();
    if (ImGui::Button("Convert", ImVec2(120, 0))) {
        persistPrefs(prefs);
        std::string err = beginBatch();
        if (err.empty()) {
            recent_input_dirs.push(std::string(input_dir_buf_));
            recent_output_dirs.push(std::string(output_dir_buf_));
        } else {
            status = std::move(err);
        }
        show_dialog_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (!can_convert)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        show_dialog_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    drawProgressDialog(status);
    return status;
}

// ============================================================================
// Format-specific option panels
// ============================================================================

void BatchConvert::drawBlpOptions() {
    drawBlpOptionsUI(prefs_.blp_version, prefs_.blp_encoding,
                     prefs_.blp_dither, prefs_.blp_dither_strength,
                     prefs_.jpeg_quality);
}

void BatchConvert::drawDdsOptions() {
    ImGui::SeparatorText("DDS Options");
    ImGui::RadioButton("General", &prefs_.dds_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Per Kind Group", &prefs_.dds_mode, 1);
    ImGui::Spacing();

    if (prefs_.dds_mode == 0) {
        drawDdsFormatCombo("Pixel Format", prefs_.dds_format_general,
                           DDS_ALL, static_cast<i32>(std::size(DDS_ALL)));
        ImGui::Checkbox("Invert Y Channel", &prefs_.dds_invert_y_general);
    } else {
        ImGui::TextDisabled("Normal Maps:");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##normal", prefs_.dds_format_normal,
                           DDS_PRESET_NORMAL, static_cast<i32>(std::size(DDS_PRESET_NORMAL)));
        ImGui::Checkbox("Invert Y Channel##normal", &prefs_.dds_invert_y_normal);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("Channel Maps (Roughness, Metalness, AO, Gloss):");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##channel", prefs_.dds_format_channel,
                           DDS_PRESET_CHANNEL, static_cast<i32>(std::size(DDS_PRESET_CHANNEL)));
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("Other:");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##other", prefs_.dds_format_other,
                           DDS_PRESET_OTHER, static_cast<i32>(std::size(DDS_PRESET_OTHER)));
        ImGui::Unindent();
    }
}

// ============================================================================
// Transformation pipeline UI
// ============================================================================

void BatchConvert::drawTransformPipeline() {
    ImGui::SeparatorText("Transformations");
    ImGui::TextDisabled("Steps are applied in order after loading each texture.");

    // Draw each step with controls
    i32 remove_idx = -1;
    i32 swap_up_idx = -1;
    i32 swap_down_idx = -1;

    for (i32 i = 0; i < static_cast<i32>(prefs_.transform_pipeline.size()); ++i) {
        auto& step = prefs_.transform_pipeline[i];
        ImGui::PushID(i);

        // Step header with reorder/remove buttons
        char label[64];
        std::snprintf(label, sizeof(label), "Step %d", i + 1);
        ImGui::Text("%s:", label);
        ImGui::SameLine();

        if (i == 0) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Up"))
            swap_up_idx = i;
        if (i == 0) ImGui::EndDisabled();

        ImGui::SameLine();

        if (i == static_cast<i32>(prefs_.transform_pipeline.size()) - 1) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Down"))
            swap_down_idx = i;
        if (i == static_cast<i32>(prefs_.transform_pipeline.size()) - 1) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            remove_idx = i;

        ImGui::Indent();

        // Transform type selector
        i32 type_int = static_cast<i32>(step.type);
        const char* type_names[] = {"Upscale", "Downscale"};

#ifdef WHITEOUT_HAS_UPSCALER
        const i32 type_count = 2;
#else
        const i32 type_count = 1; // Only Downscale available without upscaler
        if (type_int == 0) type_int = 1; // Force to Downscale
#endif

        ImGui::SetNextItemWidth(200.0f);

#ifdef WHITEOUT_HAS_UPSCALER
        if (ImGui::Combo("Type", &type_int, type_names, type_count))
            step.type = static_cast<TransformType>(type_int);
#else
        // Only show Downscale
        if (ImGui::Combo("Type", &type_int, &type_names[1], 1))
            step.type = TransformType::Downscale;
#endif

        // Type-specific options
        if (step.type == TransformType::Downscale) {
            i32 lvl_idx = step.downscale_levels - 1;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Scale", &lvl_idx, kDownscaleOptions,
                             kDownscaleOptionCount)) {
                step.downscale_levels = lvl_idx + 1;
            }
        }

#ifdef WHITEOUT_HAS_UPSCALER
        if (step.type == TransformType::Upscale) {
            if (upscale_models_.empty()) {
                ImGui::TextWrapped("No upscaler models found. Place model files in the "
                                   "models/ directory next to the executable.");
            } else {
                if (step.upscale_model_index >= static_cast<i32>(upscale_models_.size()))
                    step.upscale_model_index = 0;
                ImGui::SetNextItemWidth(300.0f);
                const auto& model = upscale_models_[step.upscale_model_index];
                std::string combo_label = model.label();
                if (ImGui::BeginCombo("Model", combo_label.c_str())) {
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
                ImGui::Checkbox("Upscale Alpha", &step.upscale_alpha);
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
        std::swap(prefs_.transform_pipeline[swap_up_idx], prefs_.transform_pipeline[swap_up_idx - 1]);
    if (swap_down_idx >= 0 &&
        swap_down_idx < static_cast<i32>(prefs_.transform_pipeline.size()) - 1)
        std::swap(prefs_.transform_pipeline[swap_down_idx], prefs_.transform_pipeline[swap_down_idx + 1]);

    // Add step button
    if (ImGui::Button("+ Add Transformation")) {
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

void BatchConvert::drawProgressDialog(std::string& status) {
    if (converting_) {
        ImGui::OpenPopup("##BatchProgress");
    }
    centerNextWindow();
    ImGui::SetNextWindowSize(ImVec2(500, 0));
    if (ImGui::BeginPopupModal("##BatchProgress", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize)) {
        const i32 total = static_cast<i32>(pending_files_.size());
        const i32 processed = batch_processed_.load(std::memory_order_relaxed);
        const f32 fraction = total > 0 ? static_cast<f32>(processed) / total : 0.0f;

        ImGui::Text("Converting... (%d / %d)", processed, total);
        ImGui::ProgressBar(fraction, ImVec2(-1, 0));

        if (batch_done_.load(std::memory_order_acquire)) {
            // Join all worker threads
            for (auto& t : workers_)
                if (t.joinable()) t.join();
            workers_.clear();

            converting_ = false;
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "Batch complete: %d succeeded, %d failed out of %d files.",
                          batch_success_.load(), batch_fail_.load(), total);
            status = msg;
            pending_files_.clear();
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

    if (converting_)
        return "Error: A batch conversion is already in progress.";

    const std::string input_dir(input_dir_buf_);
    const std::string output_dir(output_dir_buf_);

    if (input_dir.empty())
        return "Error: No input directory specified.";
    if (output_dir.empty())
        return "Error: No output directory specified.";

    std::error_code ec;
    if (!fs::is_directory(input_dir, ec))
        return "Error: Input directory does not exist.";
    if (fs::equivalent(input_dir, output_dir, ec))
        return "Error: Input and output directories must be different.";

    fs::create_directories(output_dir, ec);
    if (!fs::is_directory(output_dir, ec))
        return "Error: Could not create output directory.";

    // Collect matching files
    pending_files_.clear();
    auto collect = [&](auto& it) {
        for (const auto& entry : it) {
            if (!entry.is_regular_file())
                continue;
            std::string ext = to_lower(entry.path().extension().string());
            if (matchesFilter(ext, prefs_)) {
                pending_files_.push_back(entry.path().string());
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

    if (pending_files_.empty())
        return "No matching files found in input directory.";

    batch_input_dir_ = input_dir;
    batch_output_dir_ = output_dir;
    batch_processed_.store(0, std::memory_order_relaxed);
    batch_success_.store(0, std::memory_order_relaxed);
    batch_fail_.store(0, std::memory_order_relaxed);
    batch_done_.store(false, std::memory_order_relaxed);
    work_index_.store(0, std::memory_order_relaxed);
    converting_ = true;

    // Launch worker threads
    // If the pipeline contains upscale steps, use one thread to avoid GPU memory issues.
    bool has_upscale = false;
    for (const auto& step : prefs_.transform_pipeline) {
        if (step.type == TransformType::Upscale) {
            has_upscale = true;
            break;
        }
    }
    const i32 thread_count = has_upscale
        ? 1
        : std::max(1, static_cast<i32>(std::thread::hardware_concurrency()));
    workers_.clear();
    workers_.reserve(thread_count);
    for (i32 i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this]() { workerFunc(); });
    }

    return {};
}

void BatchConvert::workerFunc() {
    namespace fs = std::filesystem;

    // Each thread gets its own converter to avoid shared mutable state
    TC converter;
    auto* pool = threadPoolManager().get();
    const i32 total = static_cast<i32>(pending_files_.size());

    // Take a snapshot of the pipeline for this worker
    const auto pipeline = prefs_.transform_pipeline;

#ifdef WHITEOUT_HAS_UPSCALER
    // Initialize one Upscaler per required model (lazily)
    // Map model_index -> initialized Upscaler
    std::unordered_map<i32, std::unique_ptr<Upscaler>> upscalers;
    const auto model_dir = Upscaler::defaultModelDir();

    auto getUpscaler = [&](i32 model_index) -> Upscaler* {
        auto it = upscalers.find(model_index);
        if (it != upscalers.end())
            return it->second.get();
        if (model_index >= static_cast<i32>(upscale_models_.size()))
            return nullptr;
        auto up = std::make_unique<Upscaler>();
        if (!up->init(model_dir, upscale_models_[model_index]))
            return nullptr;
        auto* ptr = up.get();
        upscalers[model_index] = std::move(up);
        return ptr;
    };
#endif

    // Signal progress and detect completion atomically via the return value
    // of fetch_add, avoiding the race in a separate load-after-loop check.
    const auto signalProgress = [&] {
        if (batch_processed_.fetch_add(1, std::memory_order_acq_rel) + 1 >= total) {
            batch_done_.store(true, std::memory_order_release);
        }
    };

    while (true) {
        const i32 idx = work_index_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total)
            break;

        try {
            const auto& file = pending_files_[idx];

            TFF file_fmt = TC::classifyPath(file);
            std::optional<tex::Texture> loaded;

            if (file_fmt == TFF::TEX && TC::isD4Tex(file)) {
                loaded = loadD4TexWithFallback(converter, file);
            } else {
                loaded = converter.load(file);
            }

            if (!loaded) {
                batch_fail_.fetch_add(1, std::memory_order_relaxed);
                signalProgress();
                continue;
            }

            applyGuessedKind(*loaded, file);

            // ── Apply transformation pipeline ──────────────────────────
            bool pipeline_failed = false;
            for (const auto& step : pipeline) {
                if (step.type == TransformType::Downscale) {
                    tex::Texture out;
                    if (auto err = withBcnRoundtrip(*loaded, pool, out,
                            [&step](tex::Texture& work, interfaces::WorkerPool* p)
                                -> std::optional<std::string> {
                                const u32 new_w = work.width() >> step.downscale_levels;
                                const u32 new_h = work.height() >> step.downscale_levels;
                                if (new_w < 1 || new_h < 1)
                                    return std::string("Image too small to downscale");
                                return work.downscale(
                                    static_cast<u32>(step.downscale_levels), p);
                            })) {
                        pipeline_failed = true;
                        break;
                    }
                    *loaded = std::move(out);
                }
#ifdef WHITEOUT_HAS_UPSCALER
                else if (step.type == TransformType::Upscale) {
                    auto* upscaler = getUpscaler(step.upscale_model_index);
                    if (!upscaler) {
                        pipeline_failed = true;
                        break;
                    }
                    const auto prev = *loaded;
                    auto result = upscaler->process(*loaded, step.upscale_alpha);
                    if (!result) {
                        pipeline_failed = true;
                        break;
                    }
                    *loaded = std::move(*result);
                    copyKindMetadata(*loaded, prev);
                }
#endif
            }

            if (pipeline_failed) {
                batch_fail_.fetch_add(1, std::memory_order_relaxed);
                signalProgress();
                continue;
            }

            std::error_code ec;
            fs::path out_base;
            if (prefs_.keep_layout) {
                auto rel = fs::relative(fs::path(file).parent_path(), batch_input_dir_, ec);
                out_base = fs::path(batch_output_dir_) / rel;
                fs::create_directories(out_base, ec);
            } else {
                out_base = fs::path(batch_output_dir_);
            }
            auto stem = fs::path(file).stem().string();
            auto out_path = (out_base / (stem + OUTPUT_EXTENSIONS[prefs_.output_format])).string();

            const auto tex_kind = loaded->kind();
            if (saveOne(converter, std::move(*loaded), out_path, tex_kind)) {
                batch_success_.fetch_add(1, std::memory_order_relaxed);
            } else {
                batch_fail_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            batch_fail_.fetch_add(1, std::memory_order_relaxed);
        }
        signalProgress();
    }
}

// ============================================================================
// Single-file save (mirrors SaveDialog::performSave)
// ============================================================================

bool BatchConvert::saveOne(TC& converter, tex::Texture tex_copy, const std::string& out_path,
                            tex::TextureKind kind) {
    auto* pool = threadPoolManager().get();

    if (prefs_.generate_mipmaps) {
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        const auto mipCount = effectiveMipCount(prefs_.mipmap_mode, prefs_.mipmap_custom_count, tex_copy);
        if (auto err = tex_copy.generateMipmaps(mipCount, pool))
            return false;
    }

    switch (prefs_.output_format) {
    case 0: { // BLP
        auto blp = buildBlpSaveOptions(prefs_.blp_version, prefs_.blp_encoding,
                                       prefs_.blp_dither, prefs_.blp_dither_strength,
                                       prefs_.jpeg_quality);
        coerceBlpFormat(tex_copy, prefs_.blp_encoding, blp.encoding, pool);
        return converter.save(tex_copy, out_path, blp);
    }

    case 2: { // DDS
        i32 dds_fmt;
        bool invert_y;

        if (prefs_.dds_mode == 1) {
            if (kind == tex::TextureKind::Normal) {
                dds_fmt = prefs_.dds_format_normal;
                invert_y = prefs_.dds_invert_y_normal;
            } else if (isChannelMapKind(kind)) {
                dds_fmt = prefs_.dds_format_channel;
                invert_y = false;
            } else {
                dds_fmt = prefs_.dds_format_other;
                invert_y = false;
            }
        } else {
            dds_fmt = prefs_.dds_format_general;
            invert_y = prefs_.dds_invert_y_general;
        }

        coerceDdsFormat(tex_copy, dds_fmt, invert_y, pool);
        return converter.save(tex_copy, out_path);
    }

    case 3: // JPEG
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        return converter.save(tex_copy, out_path, prefs_.jpeg_quality);

    default: // BMP (1), PNG (4), TGA (5)
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        return converter.save(tex_copy, out_path);
    }
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

} // namespace whiteout::gui
