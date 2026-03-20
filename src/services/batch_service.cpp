// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/batch_service.h"
#include "thread_pool_manager.h"
#include "views/save_helpers.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

namespace {

using whiteout::i32;

} // anonymous namespace

namespace whiteout::gui {

BatchService::~BatchService() {
    joinWorkers();
}

// ============================================================================
// Start
// ============================================================================

std::string BatchService::start(BatchJob job) {
    namespace fs = std::filesystem;

    if (converting_)
        return "Error: A batch conversion is already in progress.";

    if (job.input_dir.empty())
        return "Error: No input directory specified.";
    if (job.output_dir.empty())
        return "Error: No output directory specified.";

    std::error_code ec;
    if (!fs::is_directory(job.input_dir, ec))
        return "Error: Input directory does not exist.";
    if (fs::equivalent(job.input_dir, job.output_dir, ec))
        return "Error: Input and output directories must be different.";

    fs::create_directories(job.output_dir, ec);
    if (!fs::is_directory(job.output_dir, ec))
        return "Error: Could not create output directory.";

    if (job.files.empty())
        return "No matching files found in input directory.";

    job_ = std::move(job);
    batch_processed_.store(0, std::memory_order_relaxed);
    batch_success_.store(0, std::memory_order_relaxed);
    batch_fail_.store(0, std::memory_order_relaxed);
    batch_done_.store(false, std::memory_order_relaxed);
    work_index_.store(0, std::memory_order_relaxed);
    converting_ = true;

    // If the pipeline contains upscale steps, use one thread to avoid GPU memory issues.
    bool has_upscale = false;
    for (const auto& step : job_.prefs.transform_pipeline) {
        if (step.type == TransformType::Upscale) {
            has_upscale = true;
            break;
        }
    }
    const i32 thread_count =
        has_upscale ? 1 : std::max(1, static_cast<i32>(std::thread::hardware_concurrency()));
    workers_.clear();
    workers_.reserve(thread_count);
    for (i32 i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this]() { workerFunc(); });
    }

    return {};
}

// ============================================================================
// Progress
// ============================================================================

BatchProgress BatchService::progress() const {
    BatchProgress p;
    p.total = static_cast<i32>(job_.files.size());
    p.processed = batch_processed_.load(std::memory_order_relaxed);
    p.success = batch_success_.load(std::memory_order_relaxed);
    p.fail = batch_fail_.load(std::memory_order_relaxed);
    p.done = batch_done_.load(std::memory_order_acquire);
    return p;
}

void BatchService::joinWorkers() {
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
    converting_ = false;
}

// ============================================================================
// Worker
// ============================================================================

void BatchService::workerFunc() {
    namespace fs = std::filesystem;

    TC converter;
    auto* pool = threadPoolManager().get();
    const i32 total = static_cast<i32>(job_.files.size());

    const auto pipeline = job_.prefs.transform_pipeline;

#ifdef WHITEOUT_HAS_UPSCALER
    std::unordered_map<i32, std::unique_ptr<Upscaler>> upscalers;
    const auto model_dir = Upscaler::defaultModelDir();

    auto getUpscaler = [&](i32 model_index) -> Upscaler* {
        auto it = upscalers.find(model_index);
        if (it != upscalers.end())
            return it->second.get();
        if (model_index >= static_cast<i32>(job_.upscale_models.size()))
            return nullptr;
        auto up = std::make_unique<Upscaler>();
        if (!up->init(model_dir, job_.upscale_models[model_index]))
            return nullptr;
        auto* ptr = up.get();
        upscalers[model_index] = std::move(up);
        return ptr;
    };
#endif

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
            const auto& file = job_.files[idx];

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

            // Apply transformation pipeline.
            bool pipeline_failed = false;
            for (const auto& step : pipeline) {
                if (step.type == TransformType::Downscale) {
                    tex::Texture out;
                    if (auto err = withBcnRoundtrip(
                            *loaded, pool, out,
                            [&step](tex::Texture& work,
                                    interfaces::WorkerPool* p) -> std::optional<std::string> {
                                const u32 new_w = work.width() >> step.downscale_levels;
                                const u32 new_h = work.height() >> step.downscale_levels;
                                if (new_w < 1 || new_h < 1)
                                    return std::string("Image too small to downscale");
                                return work.downscale(static_cast<u32>(step.downscale_levels), p);
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
            if (job_.prefs.keep_layout) {
                auto rel = fs::relative(fs::path(file).parent_path(), job_.input_dir, ec);
                out_base = fs::path(job_.output_dir) / rel;
                fs::create_directories(out_base, ec);
            } else {
                out_base = fs::path(job_.output_dir);
            }
            auto stem = fs::path(file).stem().string();
            auto out_path =
                (out_base / (stem + kOutputExtensions[job_.prefs.output_format])).string();

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
// Single-file save
// ============================================================================

bool BatchService::saveOne(TC& converter, tex::Texture tex_copy, const std::string& out_path,
                           tex::TextureKind kind) {
    auto* pool = threadPoolManager().get();

    if (job_.prefs.generate_mipmaps) {
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        const auto mipCount =
            effectiveMipCount(job_.prefs.mipmap_mode, job_.prefs.mipmap_custom_count, tex_copy);
        if (auto err = tex_copy.generateMipmaps(mipCount, pool))
            return false;
    }

    switch (job_.prefs.output_format) {
    case 0: { // BLP
        auto blp = buildBlpSaveOptions(job_.prefs.blp_version, job_.prefs.blp_encoding,
                                       job_.prefs.blp_dither, job_.prefs.blp_dither_strength,
                                       job_.prefs.jpeg_quality);
        coerceBlpFormat(tex_copy, job_.prefs.blp_encoding, blp.encoding, pool);
        return converter.save(tex_copy, out_path, blp);
    }

    case 2: { // DDS
        i32 dds_fmt;
        bool invert_y;

        if (job_.prefs.dds_mode == 1) {
            if (kind == tex::TextureKind::Normal) {
                dds_fmt = job_.prefs.dds_format_normal;
                invert_y = job_.prefs.dds_invert_y_normal;
            } else if (isChannelMapKind(kind)) {
                dds_fmt = job_.prefs.dds_format_channel;
                invert_y = false;
            } else {
                dds_fmt = job_.prefs.dds_format_other;
                invert_y = false;
            }
        } else {
            dds_fmt = job_.prefs.dds_format_general;
            invert_y = job_.prefs.dds_invert_y_general;
        }

        coerceDdsFormat(tex_copy, dds_fmt, invert_y, pool);
        return converter.save(tex_copy, out_path);
    }

    case 3: // JPEG
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        return converter.save(tex_copy, out_path, job_.prefs.jpeg_quality);

    default: // BMP (1), PNG (4), TGA (5)
        if (tex::isBcn(tex_copy.format()))
            tex_copy = tex_copy.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        return converter.save(tex_copy, out_path);
    }
}

} // namespace whiteout::gui
