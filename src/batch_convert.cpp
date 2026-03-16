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
#include <vector>

#include <whiteout/textures/blp/types.h>

#include <imgui.h>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;
using whiteout::gui::BLP_ENCODING_NAMES;
using whiteout::gui::DDS_FORMAT_NAMES;

namespace {

// ── Output format table ────────────────────────────────────────────────

constexpr const char* OUTPUT_FORMAT_NAMES[] = {"BLP", "BMP", "DDS", "JPEG", "PNG", "TGA"};
constexpr int OUTPUT_FORMAT_COUNT = static_cast<int>(std::size(OUTPUT_FORMAT_NAMES));
constexpr const char* OUTPUT_EXTENSIONS[] = {".blp", ".bmp", ".dds", ".jpg", ".png", ".tga"};

// ── BLP / DDS name arrays live in save_dialog.h

// DDS format presets by texture-kind group
constexpr int DDS_ALL[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr int DDS_ALL_COUNT = static_cast<int>(std::size(DDS_ALL));

// ── Helpers ────────────────────────────────────────────────────────────

static bool matchesFilter(const std::string& ext, bool blp, bool bmp, bool dds, bool jpeg,
                           bool png, bool tex_f, bool tga) {
    if (ext == ".blp") return blp;
    if (ext == ".bmp") return bmp;
    if (ext == ".dds") return dds;
    if (ext == ".jpg" || ext == ".jpeg") return jpeg;
    if (ext == ".png") return png;
    if (ext == ".tex") return tex_f;
    if (ext == ".tga") return tga;
    return false;
}

static void drawDdsFormatCombo(const char* label, int& fmt, const int* allowed, int count) {
    whiteout::gui::validateDdsFormatRaw(fmt, allowed, count);
    if (ImGui::BeginCombo(label, DDS_FORMAT_NAMES[fmt])) {
        for (int i = 0; i < count; ++i) {
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
                                                  int /*filter*/) {
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

void BatchConvert::open() {
    show_dialog_ = true;
}

// ============================================================================
// Folder result processing
// ============================================================================

void BatchConvert::processFolderResults() {
    if (input_folder_state_.has_pending.load()) {
        std::lock_guard lock(input_folder_state_.mtx);
        copyToBuffer(input_dir_buf_, input_folder_state_.pending_path);
        input_folder_state_.has_pending.store(false);
    }
    if (output_folder_state_.has_pending.load()) {
        std::lock_guard lock(output_folder_state_.mtx);
        copyToBuffer(output_dir_buf_, output_folder_state_.pending_path);
        output_folder_state_.has_pending.store(false);
    }
}

// ============================================================================
// Draw
// ============================================================================

std::string BatchConvert::draw(SDL_Window* window) {
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
    ImGui::InputText("##input_dir", input_dir_buf_, sizeof(input_dir_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##input")) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &input_folder_state_, window,
                                 input_dir_buf_[0] ? input_dir_buf_ : nullptr, false);
    }

    ImGui::Text("Read Formats:");
    ImGui::Checkbox("BLP", &filter_blp_);
    ImGui::SameLine();
    ImGui::Checkbox("BMP", &filter_bmp_);
    ImGui::SameLine();
    ImGui::Checkbox("DDS", &filter_dds_);
    ImGui::SameLine();
    ImGui::Checkbox("JPEG", &filter_jpeg_);
    ImGui::Checkbox("PNG", &filter_png_);
    ImGui::SameLine();
    ImGui::Checkbox("TEX", &filter_tex_);
    ImGui::SameLine();
    ImGui::Checkbox("TGA", &filter_tga_);

    ImGui::Spacing();
    ImGui::Checkbox("Scan subdirectories", &recursive_);
    ImGui::Checkbox("Keep directory layout", &keep_layout_);

    // ── Output ─────────────────────────────────────────────────────────

    ImGui::SeparatorText("Output");

    ImGui::Text("Directory:");
    ImGui::SetNextItemWidth(500.0f);
    ImGui::InputText("##output_dir", output_dir_buf_, sizeof(output_dir_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##output")) {
        SDL_ShowOpenFolderDialog(folderDialogCallback, &output_folder_state_, window,
                                 output_dir_buf_[0] ? output_dir_buf_ : nullptr, false);
    }

    ImGui::Combo("Format", &output_format_, OUTPUT_FORMAT_NAMES, OUTPUT_FORMAT_COUNT);

    // ── Format-specific options ────────────────────────────────────────

    switch (output_format_) {
    case 0: // BLP
        drawBlpOptions();
        break;

    case 2: // DDS
        drawDdsOptions();
        break;

    case 3: // JPEG
        ImGui::SeparatorText("JPEG Options");
        ImGui::SliderInt("Quality", &jpeg_quality_, 1, 100);
        break;

    default:
        break;
    }

    // ── Common options ─────────────────────────────────────────────────

    ImGui::SeparatorText("Common Options");
    ImGui::Checkbox("Generate Mipmaps", &generate_mipmaps_);

    // ── Buttons ────────────────────────────────────────────────────────

    ImGui::Separator();
    const bool can_convert = input_dir_buf_[0] != '\0' && output_dir_buf_[0] != '\0';
    if (!can_convert)
        ImGui::BeginDisabled();
    if (ImGui::Button("Convert", ImVec2(120, 0))) {
        std::string err = beginBatch();
        if (!err.empty())
            status = std::move(err);
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
    ImGui::SeparatorText("BLP Options");
    ImGui::Combo("BLP Version", &blp_version_,
                 "BLP1 (Warcraft 3 Classic)\0BLP2 (WoW)\0");
    {
        const int* enc_allowed;
        int enc_count;
        blpAllowedEncodings(blp_version_, enc_allowed, enc_count);
        validateBlpEncoding(blp_version_, blp_encoding_);

        if (ImGui::BeginCombo("Encoding", BLP_ENCODING_NAMES[blp_encoding_])) {
            for (int i = 0; i < enc_count; ++i) {
                bool selected = (blp_encoding_ == enc_allowed[i]);
                if (ImGui::Selectable(BLP_ENCODING_NAMES[enc_allowed[i]], selected))
                    blp_encoding_ = enc_allowed[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    if (blp_encoding_ == 2) {
        ImGui::Checkbox("Dither", &blp_dither_);
        if (blp_dither_)
            ImGui::SliderFloat("Dither Strength", &blp_dither_strength_, 0.0f, 1.0f);
    }
    if (blp_encoding_ == 3) {
        ImGui::SliderInt("JPEG Quality", &jpeg_quality_, 1, 100);
    }
}

void BatchConvert::drawDdsOptions() {
    ImGui::SeparatorText("DDS Options");
    ImGui::RadioButton("General", &dds_mode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Per Kind Group", &dds_mode_, 1);
    ImGui::Spacing();

    if (dds_mode_ == 0) {
        drawDdsFormatCombo("Pixel Format", dds_format_general_, DDS_ALL, DDS_ALL_COUNT);
        ImGui::Checkbox("Invert Y Channel", &dds_invert_y_general_);
    } else {
        ImGui::TextDisabled("Normal Maps:");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##normal", dds_format_normal_,
                           DDS_PRESET_NORMAL, DDS_PRESET_NORMAL_COUNT);
        ImGui::Checkbox("Invert Y Channel##normal", &dds_invert_y_normal_);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("Channel Maps (Roughness, Metalness, AO, Gloss):");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##channel", dds_format_channel_,
                           DDS_PRESET_CHANNEL, DDS_PRESET_CHANNEL_COUNT);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::TextDisabled("Other:");
        ImGui::Indent();
        drawDdsFormatCombo("Pixel Format##other", dds_format_other_,
                           DDS_PRESET_OTHER, DDS_PRESET_OTHER_COUNT);
        ImGui::Unindent();
    }
}

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
        const int total = static_cast<int>(pending_files_.size());
        const int processed = batch_processed_.load(std::memory_order_relaxed);
        const float fraction = total > 0 ? static_cast<float>(processed) / total : 0.0f;

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
            if (matchesFilter(ext, filter_blp_, filter_bmp_, filter_dds_, filter_jpeg_,
                              filter_png_, filter_tex_, filter_tga_)) {
                pending_files_.push_back(entry.path().string());
            }
        }
    };
    if (recursive_) {
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
    const int thread_count =
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    workers_.clear();
    workers_.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this]() { workerFunc(); });
    }

    return {};
}

void BatchConvert::workerFunc() {
    namespace fs = std::filesystem;

    // Each thread gets its own converter to avoid shared mutable state
    TC converter;
    const int total = static_cast<int>(pending_files_.size());

    // Signal progress and detect completion atomically via the return value
    // of fetch_add, avoiding the race in a separate load-after-loop check.
    const auto signalProgress = [&] {
        if (batch_processed_.fetch_add(1, std::memory_order_acq_rel) + 1 >= total) {
            batch_done_.store(true, std::memory_order_release);
        }
    };

    while (true) {
        const int idx = work_index_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total)
            break;

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

        auto kind = TC::guessTextureKind(file, loaded->format());
        loaded->setKind(kind);

        const auto loaded_fmt = loaded->format();
        if (tex::isBcn(loaded_fmt)) {
            const auto preserved_kind = loaded->kind();
            const bool preserved_srgb = loaded->isSrgb();
            auto pool = threadPoolManager().borrow();
            *loaded = loaded->copyAsFormat(tex::workingFormatFor(loaded_fmt), pool.get());
            loaded->setKind(preserved_kind);
            loaded->setSrgb(preserved_srgb);
        }

        std::error_code ec;
        fs::path out_base;
        if (keep_layout_) {
            auto rel = fs::relative(fs::path(file).parent_path(), batch_input_dir_, ec);
            out_base = fs::path(batch_output_dir_) / rel;
            fs::create_directories(out_base, ec);
        } else {
            out_base = fs::path(batch_output_dir_);
        }
        auto stem = fs::path(file).stem().string();
        auto out_path = (out_base / (stem + OUTPUT_EXTENSIONS[output_format_])).string();

        if (saveOne(converter, std::move(*loaded), out_path, kind)) {
            batch_success_.fetch_add(1, std::memory_order_relaxed);
        } else {
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
    auto pool = threadPoolManager().borrow();

    if (generate_mipmaps_) {
        if (tex::isBcn(tex_copy.format())) {
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool.get());
        }
        if (auto err = tex_copy.generateMipmaps(pool.get()))
            return false;
    }

    switch (output_format_) {
    case 0: { // BLP
        auto blp = buildBlpSaveOptions(blp_version_, blp_encoding_,
                                       blp_dither_, blp_dither_strength_, jpeg_quality_);
        coerceBlpFormat(tex_copy, blp_encoding_, blp.encoding, pool.get());
        return converter.save(tex_copy, out_path, blp);
    }

    case 2: { // DDS
        int dds_fmt;
        bool invert_y;

        if (dds_mode_ == 1) {
            if (kind == tex::TextureKind::Normal) {
                dds_fmt = dds_format_normal_;
                invert_y = dds_invert_y_normal_;
            } else if (isChannelMapKind(kind)) {
                dds_fmt = dds_format_channel_;
                invert_y = false;
            } else {
                dds_fmt = dds_format_other_;
                invert_y = false;
            }
        } else {
            dds_fmt = dds_format_general_;
            invert_y = dds_invert_y_general_;
        }

        coerceDdsFormat(tex_copy, dds_fmt, invert_y, pool.get());
        return converter.save(tex_copy, out_path);
    }

    case 3: // JPEG
        return converter.save(tex_copy, out_path, jpeg_quality_);

    default: // BMP (1), PNG (4), TGA (5)
        return converter.save(tex_copy, out_path);
    }
}

} // namespace whiteout::gui
