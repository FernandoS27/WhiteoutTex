// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "upscaler.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <whiteout/common_types.h>

#include <gpu.h>
#include <realesrgan.h>

#include <SDL3/SDL.h>

#include "thread_pool_manager.h"
#include "views/save_helpers.h"

namespace whiteout::gui {

namespace tex = whiteout::textures;

namespace {

// ============================================================================
// ncnn GPU instance management
// ============================================================================

std::atomic<bool> s_gpu_instance_created{false};

void ensureGpuInstance() {
    bool expected = false;
    if (s_gpu_instance_created.compare_exchange_strong(expected, true)) {
        ncnn::create_gpu_instance();
    }
}

// ============================================================================
// Built-in model definitions
// ============================================================================

const UpscalerModel kBuiltinModels[] = {
    {"Real-ESRGAN x4+", "realesrgan-x4plus", 4, 10},
    {"Real-ESRGAN x4+ Anime", "realesrgan-x4plus-anime", 4, 10},
    {"Real-ESRGAN x2+", "RealESRGAN_x2plus", 2, 10},
    {"Real-ESRGAN General WDN x4", "realesr-general-wdn-x4v3", 4, 10},
    {"Real-ESRGAN AnimVideo v3 x4", "realesr-animevideov3-x4", 4, 10},
    {"Real-ESRGAN AnimVideo v3 x3", "realesr-animevideov3-x3", 3, 10},
    {"Real-ESRGAN AnimVideo v3 x2", "realesr-animevideov3-x2", 2, 10},
    {"Real-ESRNet x4+", "realesrnet-x4plus", 4, 10},
};

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct Upscaler::Impl {
    std::unique_ptr<RealESRGAN> esrgan;
    i32 scale = 4;
};

// ============================================================================
// Upscaler
// ============================================================================

Upscaler::Upscaler() : impl_(std::make_unique<Impl>()) {}

Upscaler::~Upscaler() {
    // Release the model before any GPU teardown.
    impl_->esrgan.reset();
}

bool Upscaler::isGpuAvailable() {
    ensureGpuInstance();
    return ncnn::get_gpu_count() > 0;
}

std::filesystem::path Upscaler::defaultModelDir() {
    if (const char* base = SDL_GetBasePath()) {
        return std::filesystem::path(base) / "models";
    }
    return "models";
}

std::vector<UpscalerModel> Upscaler::availableModels(const std::filesystem::path& model_dir) {
    std::vector<UpscalerModel> found;
    if (!std::filesystem::is_directory(model_dir)) {
        return found;
    }
    for (const auto& m : kBuiltinModels) {
        auto param = model_dir / (m.file_stem + ".param");
        auto bin = model_dir / (m.file_stem + ".bin");
        if (std::filesystem::exists(param) && std::filesystem::exists(bin)) {
            found.push_back(m);
        }
    }
    return found;
}

i32 Upscaler::bestGpuIndex() {
    ensureGpuInstance();
    const i32 count = ncnn::get_gpu_count();
    if (count == 0) {
        return 0;
    }

    i32 best = 0;
    u64 best_score = 0;

    for (i32 i = 0; i < count; ++i) {
        const auto& info = ncnn::get_gpu_info(i);
        // Prefer discrete GPUs (type 0) over integrated (1), virtual (2), cpu (3).
        u64 score = (info.type() == 0) ? 0x80000000u : 0u;
        // Cooperative matrix accelerates inference significantly.
        if (info.support_cooperative_matrix()) {
            score += 0x40000000u;
        }
        score += ncnn::get_gpu_device(i)->get_heap_budget();
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

bool Upscaler::init(const std::filesystem::path& model_dir, const UpscalerModel& model, i32 gpu_id,
                    i32 tile_size) {
    impl_->esrgan.reset();
    ensureGpuInstance();

    if (ncnn::get_gpu_count() == 0) {
        return false;
    }

    if (gpu_id < 0) {
        gpu_id = bestGpuIndex();
    }

    auto esrgan = std::make_unique<RealESRGAN>(gpu_id, /*tta_mode=*/false);
    esrgan->scale = model.scale;
    esrgan->prepadding = model.prepadding;

    // Auto tile size based on GPU VRAM.
    if (tile_size <= 0) {
        u64 heap_budget = ncnn::get_gpu_device(gpu_id)->get_heap_budget();
        if (heap_budget > 2000)
            tile_size = 256;
        else if (heap_budget > 600)
            tile_size = 128;
        else if (heap_budget > 190)
            tile_size = 64;
        else
            tile_size = 32;
    }
    esrgan->tilesize = tile_size;

    auto param_path = model_dir / (model.file_stem + ".param");
    auto bin_path = model_dir / (model.file_stem + ".bin");

#if _WIN32
    i32 ret = esrgan->load(param_path.wstring(), bin_path.wstring());
#else
    i32 ret = esrgan->load(param_path.string(), bin_path.string());
#endif

    if (ret != 0) {
        return false;
    }

    impl_->esrgan = std::move(esrgan);
    impl_->scale = model.scale;
    return true;
}

std::optional<tex::Texture> Upscaler::process(const tex::Texture& input, bool upscale_alpha) {
    if (!impl_->esrgan) {
        return std::nullopt;
    }

    // Convert to RGBA8 if needed; avoid a copy when already in the right format.
    const bool needs_conversion = (input.format() != tex::PixelFormat::RGBA8);
    std::optional<tex::Texture> converted;
    if (needs_conversion) {
        converted = input.copyAsFormat(tex::PixelFormat::RGBA8);
    }
    const tex::Texture& work = needs_conversion ? *converted : input;

    const u32 mip_count = work.mipCount();
    const i32 w = static_cast<i32>(work.width());
    const i32 h = static_cast<i32>(work.height());
    constexpr i32 c = 4; // RGBA
    const i32 scale = impl_->scale;
    const i32 outw = w * scale;
    const i32 outh = h * scale;
    const u32 out_mip_count = mip_count > 1 ? mip_count + static_cast<u32>(std::log2(scale)) : 1;

    // The RealESRGAN::process() reads the channel count from inimage.elempack.
    // It handles BGR↔RGB conversion internally (via from_pixels / to_pixels on
    // the non-int8 path, and via shader specialization on the int8 path).
    auto src_span = work.mipData(0);
    std::vector<u8> inbuf(src_span.begin(), src_span.end());

    const size_t in_pixels = static_cast<size_t>(w) * h;
    const size_t out_pixels = static_cast<size_t>(outw) * outh;
    std::vector<u8> outbuf(out_pixels * c);

    // Helper: upscale a single channel by broadcasting it to grayscale RGB,
    // running through the model, and averaging the output RGB back.
    auto upscaleChannel = [&](const u8* src_rgba, size_t src_pixels, i32 src_w, i32 src_h,
                              i32 dst_w, i32 dst_h, size_t dst_pixels, i32 channel_index,
                              u8* dst_rgba) -> bool {
        std::vector<u8> ch_rgb(src_pixels * c);
        for (size_t px = 0; px < src_pixels; ++px) {
            const u8 val = src_rgba[px * 4 + channel_index];
            ch_rgb[px * 4 + 0] = val;
            ch_rgb[px * 4 + 1] = val;
            ch_rgb[px * 4 + 2] = val;
            ch_rgb[px * 4 + 3] = 255;
        }
        ncnn::Mat ch_in(src_w, src_h, static_cast<void*>(ch_rgb.data()), static_cast<size_t>(c), c);
        std::vector<u8> ch_out(dst_pixels * c);
        ncnn::Mat ch_out_mat(dst_w, dst_h, static_cast<void*>(ch_out.data()),
                             static_cast<size_t>(c), c);
        if (impl_->esrgan->process(ch_in, ch_out_mat) != 0)
            return false;
        for (size_t px = 0; px < dst_pixels; ++px) {
            const u8 r = ch_out[px * 4 + 0];
            const u8 g = ch_out[px * 4 + 1];
            const u8 b = ch_out[px * 4 + 2];
            dst_rgba[px * 4 + channel_index] =
                static_cast<u8>((static_cast<unsigned>(r) + g + b + 1) / 3);
        }
        return true;
    };

    const bool is_multikind = (input.kind() == tex::TextureKind::Multikind);

    if (is_multikind) {
        // Multikind: each channel has independent semantic meaning, so we
        // upscale every used channel independently through the model.
        // Unused channels are filled with their default value.
        for (i32 ch_idx = 0; ch_idx < 4; ++ch_idx) {
            const tex::TextureKind ck = input.channelKind(kRGBAChannels[ch_idx]);
            if (ck == tex::TextureKind::Unused) {
                const u8 def = static_cast<u8>(std::clamp(
                    input.channelDefault(kRGBAChannels[ch_idx]) * 255.0f + 0.5f, 0.0f, 255.0f));
                for (size_t px = 0; px < out_pixels; ++px) {
                    outbuf[px * 4 + ch_idx] = def;
                }
                continue;
            }

            if (!upscaleChannel(inbuf.data(), in_pixels, w, h, outw, outh, out_pixels, ch_idx,
                                outbuf.data())) {
                return std::nullopt;
            }
        }
    } else {
        // Non-multikind: run the model on the full RGB image.
        ncnn::Mat inimage(w, h, static_cast<void*>(inbuf.data()), static_cast<size_t>(c), c);
        ncnn::Mat outimage(outw, outh, static_cast<void*>(outbuf.data()), static_cast<size_t>(c),
                           c);

        i32 ret = impl_->esrgan->process(inimage, outimage);
        if (ret != 0) {
            return std::nullopt;
        }

        if (upscale_alpha) {
            // Upscale the alpha channel through the model as grayscale.
            if (!upscaleChannel(inbuf.data(), in_pixels, w, h, outw, outh, out_pixels, 3,
                                outbuf.data())) {
                // Fall back to opaque if alpha upscale fails.
                for (size_t i = 3; i < outbuf.size(); i += 4) {
                    outbuf[i] = 255;
                }
            }
        } else {
            // No AI alpha upscale — force fully opaque.
            for (size_t i = 3; i < outbuf.size(); i += 4) {
                outbuf[i] = 255;
            }
        }
    }

    // Build the result texture.
    auto result = tex::Texture::create2D(tex::PixelFormat::RGBA8, static_cast<u32>(outw),
                                         static_cast<u32>(outh), 1);
    auto dst = result.mipData(0);
    std::memcpy(dst.data(), outbuf.data(), std::min(dst.size(), outbuf.size()));

    copyKindMetadata(result, input);

    result.generateMipmaps(out_mip_count, threadPoolManager().get());
    return result;
}

bool Upscaler::isReady() const {
    return impl_->esrgan != nullptr;
}

} // namespace whiteout::gui
