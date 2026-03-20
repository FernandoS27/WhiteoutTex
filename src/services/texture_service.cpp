// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/texture_service.h"
#include "thread_pool_manager.h"
#include "views/save_helpers.h"

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

namespace whiteout::gui {

TextureService::TextureService(tex::TextureConverter& converter) : converter_(converter) {}

// ============================================================================
// File-based loading
// ============================================================================

TextureLoadResult TextureService::loadFromFile(const std::string& path) {
    if (TC::classifyPath(path) == TFF::TEX && TC::isD4Tex(path)) {
        auto result = loadD4TexWithFallback(converter_, path);
        if (result) {
            return prepare(path, std::move(*result));
        }
        if (converter_.hasIssues()) {
            TextureLoadResult r;
            r.error_message = "Failed to load D4 TEX: " + path;
            appendIssues(r.error_message, converter_.getIssues());
            return r;
        }
        // Needs manual payload specification.
        TextureLoadResult r;
        r.needs_d4_payload_dialog = true;
        r.d4_meta_path = path;
        r.d4_payload_prefill = replaceMetaSegment(path, "payload");
        if (r.d4_payload_prefill.empty())
            r.d4_payload_prefill = path;
        return r;
    }

    auto loaded = converter_.load(path);
    if (loaded) {
        return prepare(path, std::move(*loaded));
    }

    TextureLoadResult r;
    r.error_message = "Failed to load: " + path;
    if (converter_.hasIssues())
        appendIssues(r.error_message, converter_.getIssues());
    return r;
}

TextureLoadResult TextureService::loadD4WithPayload(const std::string& meta_path,
                                                    const std::string& payload_path,
                                                    const std::string& paylow_path) {
    std::optional<tex::Texture> loaded;
    if (!paylow_path.empty())
        loaded = converter_.loadTexD4(meta_path, payload_path, paylow_path);
    else
        loaded = converter_.loadTexD4(meta_path, payload_path);

    if (loaded)
        return prepare(meta_path, std::move(*loaded));

    TextureLoadResult r;
    r.error_message = "Failed to load D4 TEX: " + meta_path;
    if (converter_.hasIssues())
        appendIssues(r.error_message, converter_.getIssues());
    return r;
}

// ============================================================================
// Memory-based loading (used by CASC browser)
// ============================================================================

TextureLoadResult TextureService::loadFromMemory(const std::string& name, std::span<const u8> data,
                                                 tex::TextureFileFormat fmt) {
    auto loaded = converter_.load(data, fmt);
    if (loaded)
        return prepare(name, std::move(*loaded));

    TextureLoadResult r;
    r.error_message = "Failed to load: " + name;
    if (converter_.hasIssues())
        appendIssues(r.error_message, converter_.getIssues());
    return r;
}

TextureLoadResult TextureService::loadD4FromMemory(const std::string& name,
                                                   std::span<const u8> meta,
                                                   std::span<const u8> payload,
                                                   std::span<const u8> paylow) {
    std::optional<tex::Texture> loaded;
    if (!paylow.empty())
        loaded = converter_.loadTexD4(meta, payload, paylow);
    else
        loaded = converter_.loadTexD4(meta, payload);

    if (loaded)
        return prepare(name, std::move(*loaded));

    TextureLoadResult r;
    r.error_message = "Failed to load CASC file: " + name;
    if (converter_.hasIssues())
        appendIssues(r.error_message, converter_.getIssues());
    return r;
}

// ============================================================================
// Preparation (kind guessing, BCn decompression)
// ============================================================================

TextureLoadResult TextureService::prepare(const std::string& path, tex::Texture texture) {
    TextureLoadResult r;
    applyGuessedKind(texture, path);

    r.source_fmt = texture.format();
    r.file_format = TC::classifyPath(path);

    if (tex::isBcn(r.source_fmt)) {
        auto* pool = threadPoolManager().get();
        auto decompressed = texture.copyAsFormat(tex::workingFormatFor(r.source_fmt), pool);
        copyKindMetadata(decompressed, texture);
        texture = std::move(decompressed);
    }

    if (tex::isBcn(r.source_fmt) && r.source_fmt == tex::PixelFormat::BC3 &&
        texture.kind() == tex::TextureKind::Normal) {
        r.needs_bc3n_dialog = true;
    }

    r.texture = std::move(texture);
    return r;
}

// ============================================================================
// Texture transform operations
// ============================================================================

TextureOpResult TextureService::regenerateMipmaps(tex::Texture& texture, u32 mip_count) {
    namespace interfaces = whiteout::interfaces;
    auto* pool = threadPoolManager().get();
    tex::Texture out;
    if (auto err = withBcnRoundtrip(texture, pool, out,
                                    [mip_count](tex::Texture& work, interfaces::WorkerPool* p) {
                                        return work.generateMipmaps(mip_count, p);
                                    })) {
        return {false, "Mipmap generation failed: " + *err};
    }
    texture = std::move(out);
    return {true, "Mipmaps regenerated successfully."};
}

TextureOpResult TextureService::downscale(tex::Texture& texture, u32 levels) {
    namespace interfaces = whiteout::interfaces;
    auto* pool = threadPoolManager().get();
    tex::Texture out;
    if (auto err = withBcnRoundtrip(texture, pool, out,
                                    [levels](tex::Texture& work, interfaces::WorkerPool* p) {
                                        return work.downscale(levels, p);
                                    })) {
        return {false, "Downscale failed: " + *err};
    }
    texture = std::move(out);
    return {true, "Image downscaled successfully."};
}

void TextureService::applyBC3NSwap(tex::Texture& texture) {
    texture.swapChannels(tex::Channel::R, tex::Channel::A);
    texture.invertChannel(tex::Channel::G);
}

// ============================================================================
// Display pixel operations (moved from ImageViewer)
// ============================================================================

tex::Texture TextureService::makeDisplayTexture(const tex::Texture& texture,
                                                whiteout::interfaces::WorkerPool* pool) {
    if (texture.kind() == tex::TextureKind::Normal) {
        if (auto expanded = texture.copyFromNormalToRGBA(pool)) {
            return std::move(*expanded);
        }
    }
    auto result = texture.copyAsFormat(tex::PixelFormat::RGBA8, pool);
    const auto src_fmt = texture.format();
    if (src_fmt == tex::PixelFormat::R8 || src_fmt == tex::PixelFormat::R16 ||
        src_fmt == tex::PixelFormat::R32F || src_fmt == tex::PixelFormat::BC4) {
        auto span = result.data();
        for (size_t i = 0; i + 3 < span.size(); i += 4) {
            span[i + 1] = span[i]; // G = R
            span[i + 2] = span[i]; // B = R
        }
    }
    return result;
}

std::vector<u8> TextureService::applyChannelFilter(const u8* data, i32 width, i32 height,
                                                   bool show_r, bool show_g, bool show_b,
                                                   bool show_a) {
    const i32 count = width * height;
    std::vector<u8> out(static_cast<size_t>(count) * 4);
    const i32 active = (show_r ? 1 : 0) + (show_g ? 1 : 0) + (show_b ? 1 : 0) + (show_a ? 1 : 0);
    for (i32 i = 0; i < count; ++i) {
        const u8 r = data[i * 4 + 0];
        const u8 g = data[i * 4 + 1];
        const u8 b = data[i * 4 + 2];
        const u8 a = data[i * 4 + 3];
        if (active == 1) {
            u8 val = show_r ? r : (show_g ? g : (show_b ? b : a));
            out[i * 4 + 0] = val;
            out[i * 4 + 1] = val;
            out[i * 4 + 2] = val;
            out[i * 4 + 3] = 255;
        } else {
            out[i * 4 + 0] = show_r ? r : 0;
            out[i * 4 + 1] = show_g ? g : 0;
            out[i * 4 + 2] = show_b ? b : 0;
            out[i * 4 + 3] = show_a ? a : 255;
        }
    }
    return out;
}

} // namespace whiteout::gui
