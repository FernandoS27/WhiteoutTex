// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "batch_convert.h"

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

namespace {

// ── Output format table ────────────────────────────────────────────────

constexpr const char* OUTPUT_FORMAT_NAMES[] = {"BLP", "BMP", "DDS", "JPEG", "PNG", "TGA"};
constexpr int OUTPUT_FORMAT_COUNT = static_cast<int>(std::size(OUTPUT_FORMAT_NAMES));
constexpr const char* OUTPUT_EXTENSIONS[] = {".blp", ".bmp", ".dds", ".jpg", ".png", ".tga"};

// ── BLP encoding names (identical to save_dialog.cpp) ──────────────────

constexpr const char* BLP_ENCODING_NAMES[] = {
    "Infer (Auto)", "True Color (BGRA)", "Paletted (256 colors)", "JPEG",
    "BC1 (DXT1)",   "BC2 (DXT3)",        "BC3 (DXT5)"};

// ── DDS format names (identical to save_dialog.cpp) ────────────────────

constexpr const char* DDS_FORMAT_NAMES[] = {
    "True Color (RGBA8)", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "BC3N"};

// DDS format presets by texture-kind group (mirrors save dialog).
constexpr int DDS_ALL[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr int DDS_ALL_COUNT = static_cast<int>(std::size(DDS_ALL));

constexpr int DDS_NORMAL[] = {0, 5, 8};
constexpr int DDS_NORMAL_COUNT = static_cast<int>(std::size(DDS_NORMAL));

constexpr int DDS_CHANNEL[] = {0, 4};
constexpr int DDS_CHANNEL_COUNT = static_cast<int>(std::size(DDS_CHANNEL));

constexpr int DDS_OTHER[] = {0, 1, 2, 3, 6, 7};
constexpr int DDS_OTHER_COUNT = static_cast<int>(std::size(DDS_OTHER));

// ── Helpers ────────────────────────────────────────────────────────────

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

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

static void validateDdsFormat(int& fmt, const int* allowed, int count) {
    for (int i = 0; i < count; ++i)
        if (allowed[i] == fmt) return;
    fmt = allowed[0];
}

static void drawDdsFormatCombo(const char* label, int& fmt, const int* allowed, int count) {
    validateDdsFormat(fmt, allowed, count);
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
        std::strncpy(input_dir_buf_, input_folder_state_.pending_path.c_str(),
                     sizeof(input_dir_buf_) - 1);
        input_dir_buf_[sizeof(input_dir_buf_) - 1] = '\0';
        input_folder_state_.has_pending.store(false);
    }
    if (output_folder_state_.has_pending.load()) {
        std::lock_guard lock(output_folder_state_.mtx);
        std::strncpy(output_dir_buf_, output_folder_state_.pending_path.c_str(),
                     sizeof(output_dir_buf_) - 1);
        output_dir_buf_[sizeof(output_dir_buf_) - 1] = '\0';
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
    {
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
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
    case 0: { // BLP
        ImGui::SeparatorText("BLP Options");
        ImGui::Combo("BLP Version", &blp_version_,
                     "BLP1 (Warcraft 3 Classic)\0BLP2 (WoW)\0");
        {
            static constexpr int BLP1_ENC[] = {2, 3};
            static constexpr int BLP2_ENC[] = {0, 1, 2, 3, 4, 5, 6};
            const bool is_blp1 = (blp_version_ == 0);
            const int* enc_allowed = is_blp1 ? BLP1_ENC : BLP2_ENC;
            const int enc_count = is_blp1 ? 2 : 7;

            bool enc_valid = false;
            for (int i = 0; i < enc_count; ++i)
                if (enc_allowed[i] == blp_encoding_) {
                    enc_valid = true;
                    break;
                }
            if (!enc_valid)
                blp_encoding_ = enc_allowed[0];

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
            if (blp_dither_) {
                ImGui::SliderFloat("Dither Strength", &blp_dither_strength_, 0.0f, 1.0f);
            }
        }
        if (blp_encoding_ == 3) {
            ImGui::SliderInt("JPEG Quality", &jpeg_quality_, 1, 100);
        }
        break;
    }

    case 2: { // DDS
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
            drawDdsFormatCombo("Pixel Format##normal", dds_format_normal_, DDS_NORMAL,
                               DDS_NORMAL_COUNT);
            ImGui::Checkbox("Invert Y Channel##normal", &dds_invert_y_normal_);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::TextDisabled("Channel Maps (Roughness, Metalness, AO, Gloss):");
            ImGui::Indent();
            drawDdsFormatCombo("Pixel Format##channel", dds_format_channel_, DDS_CHANNEL,
                               DDS_CHANNEL_COUNT);
            ImGui::Unindent();

            ImGui::Spacing();
            ImGui::TextDisabled("Other:");
            ImGui::Indent();
            drawDdsFormatCombo("Pixel Format##other", dds_format_other_, DDS_OTHER,
                               DDS_OTHER_COUNT);
            ImGui::Unindent();
        }
        break;
    }

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
        if (err.empty()) {
            show_dialog_ = false;
            ImGui::CloseCurrentPopup();
        } else {
            status = std::move(err);
            show_dialog_ = false;
            ImGui::CloseCurrentPopup();
        }
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
// Progress dialog
// ============================================================================

void BatchConvert::drawProgressDialog(std::string& status) {
    if (converting_) {
        ImGui::OpenPopup("##BatchProgress");
    }
    {
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 0));
    }
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

    while (true) {
        const int idx = work_index_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total)
            break;

        const auto& file = pending_files_[idx];

        TFF file_fmt = TC::classifyPath(file);
        std::optional<tex::Texture> loaded;

        if (file_fmt == TFF::TEX && TC::isD4Tex(file)) {
            std::string payload = file;
            auto pos = payload.find("meta");
            if (pos != std::string::npos) {
                payload.replace(pos, 4, "payload");
                std::string paylow = file;
                auto paylow_pos = paylow.find("meta");
                if (paylow_pos != std::string::npos)
                    paylow.replace(paylow_pos, 4, "paylow");
                else
                    paylow.clear();

                const bool payload_exists = fs::exists(payload);
                const bool paylow_exists = !paylow.empty() && fs::exists(paylow);

                if (payload_exists && paylow_exists)
                    loaded = converter.loadTexD4(file, payload, paylow);
                else if (payload_exists)
                    loaded = converter.loadTexD4(file, payload);
                else if (paylow_exists)
                    loaded = converter.loadTexD4(file, paylow);
            }
        } else {
            loaded = converter.load(file);
        }

        if (!loaded) {
            batch_fail_.fetch_add(1, std::memory_order_relaxed);
            batch_processed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        auto kind = TC::guessTextureKind(file, loaded->format());
        loaded->setKind(kind);

        auto loaded_fmt = loaded->format();
        tex::PixelFormat working;
        if (loaded_fmt == tex::PixelFormat::BC4)
            working = tex::PixelFormat::R32F;
        else if (loaded_fmt == tex::PixelFormat::BC5)
            working = tex::PixelFormat::RG32F;
        else
            working = tex::PixelFormat::RGBA32F;

        auto preserved_kind = loaded->kind();
        bool preserved_srgb = loaded->isSrgb();
        *loaded = loaded->copyAsFormat(working);
        loaded->setKind(preserved_kind);
        loaded->setSrgb(preserved_srgb);

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
        batch_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    // Last thread to finish signals done
    int processed = batch_processed_.load(std::memory_order_relaxed);
    if (processed >= total) {
        batch_done_.store(true, std::memory_order_release);
    }
}

// ============================================================================
// Single-file save (mirrors SaveDialog::performSave)
// ============================================================================

bool BatchConvert::saveOne(TC& converter, tex::Texture tex_copy, const std::string& out_path,
                            tex::TextureKind kind) {
    if (generate_mipmaps_) {
        bool is_bcn = tex_copy.format() >= tex::PixelFormat::BC1 &&
                      tex_copy.format() <= tex::PixelFormat::BC7;
        if (is_bcn) {
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
        }
        tex_copy.generateMipmaps();
    }

    switch (output_format_) {
    case 0: { // BLP
        tex::blp::SaveOptions blp;
        blp.version =
            blp_version_ == 0 ? tex::blp::BlpVersion::BLP1 : tex::blp::BlpVersion::BLP2;
        switch (blp_encoding_) {
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
        blp.dither = blp_dither_;
        blp.ditherStrength = blp_dither_strength_;
        blp.jpegQuality = jpeg_quality_;

        if (blp.encoding == tex::blp::BlpEncoding::JPEG ||
            blp.encoding == tex::blp::BlpEncoding::Palettized) {
            blp.version = tex::blp::BlpVersion::BLP1;
        }

        if (blp.encoding == tex::blp::BlpEncoding::JPEG ||
            blp.encoding == tex::blp::BlpEncoding::Palettized ||
            blp.encoding == tex::blp::BlpEncoding::BGRA ||
            blp.encoding == tex::blp::BlpEncoding::Infer) {
            if (tex_copy.format() != tex::PixelFormat::RGBA8)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
        } else if (blp_encoding_ == 4) {
            if (tex_copy.format() != tex::PixelFormat::BC1)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC1);
        } else if (blp_encoding_ == 5) {
            if (tex_copy.format() != tex::PixelFormat::BC2)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC2);
        } else if (blp_encoding_ == 6) {
            if (tex_copy.format() != tex::PixelFormat::BC3)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::BC3);
        }
        return converter.save(tex_copy, out_path, blp);
    }

    case 2: { // DDS
        int dds_fmt;
        bool invert_y;

        if (dds_mode_ == 1) {
            int k = static_cast<int>(kind);
            if (k == 2) { // Normal
                dds_fmt = dds_format_normal_;
                invert_y = dds_invert_y_normal_;
            } else if (k == 6 || k == 7 || k == 8 || k == 9) { // Channel
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

        constexpr tex::PixelFormat DDS_PIXEL_FORMATS[] = {
            tex::PixelFormat::RGBA8, tex::PixelFormat::BC1, tex::PixelFormat::BC2,
            tex::PixelFormat::BC3,   tex::PixelFormat::BC4, tex::PixelFormat::BC5,
            tex::PixelFormat::BC6H,  tex::PixelFormat::BC7,
        };
        const bool is_bc3n = (dds_fmt == 8);
        auto target = is_bc3n ? tex::PixelFormat::BC3 : DDS_PIXEL_FORMATS[dds_fmt];

        if (is_bc3n) {
            if (tex_copy.format() != tex::PixelFormat::RGBA8)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
            if (invert_y)
                tex_copy.invertChannel(tex::Channel::G);
            tex_copy.swapChannels(tex::Channel::R, tex::Channel::A);
        } else if (invert_y) {
            bool is_bcn = tex_copy.format() >= tex::PixelFormat::BC1 &&
                          tex_copy.format() <= tex::PixelFormat::BC7;
            if (is_bcn)
                tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8);
            tex_copy.invertChannel(tex::Channel::G);
        }
        if (tex_copy.format() != target)
            tex_copy = tex_copy.copyAsFormat(target);
        return converter.save(tex_copy, out_path);
    }

    case 3: // JPEG
        return converter.save(tex_copy, out_path, jpeg_quality_);

    default: // BMP (1), PNG (4), TGA (5)
        return converter.save(tex_copy, out_path);
    }
}

} // namespace whiteout::gui
