// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "save_helpers.h"

#include <filesystem>

namespace whiteout::textool::views {

namespace blp = whiteout::textures::blp;
namespace texn = whiteout::textures;

// ============================================================================
// BLP save preparation
// ============================================================================

blp::SaveOptions buildBlpSaveOptions(i32 blp_version, i32 blp_encoding, bool dither,
                                     f32 dither_strength, i32 jpeg_quality) noexcept {
    blp::SaveOptions opts;
    opts.version = blp_version == 0 ? blp::BlpVersion::BLP1 : blp::BlpVersion::BLP2;
    opts.encoding = toBlpEncoding(blp_encoding);
    opts.dither = dither;
    opts.ditherStrength = dither_strength;
    opts.jpegQuality = jpeg_quality;

    // Force BLP1 for encodings that require it.
    if (opts.encoding == blp::BlpEncoding::JPEG || opts.encoding == blp::BlpEncoding::Palettized)
        opts.version = blp::BlpVersion::BLP1;

    return opts;
}

void coerceBlpFormat(texn::Texture& tex, i32 blp_encoding, blp::BlpEncoding enc,
                     interfaces::WorkerPool* pool) {
    if (enc == blp::BlpEncoding::JPEG || enc == blp::BlpEncoding::Palettized ||
        enc == blp::BlpEncoding::BGRA || enc == blp::BlpEncoding::Infer) {
        if (tex.format() != texn::PixelFormat::RGBA8)
            tex = tex.copyAsFormat(texn::PixelFormat::RGBA8, pool);
    } else { // DXT subtype: pick BC pixel format by index (4→BC1, 5→BC2, 6→BC3)
        const auto dxt_fmt = blpDxtPixelFormat(blp_encoding);
        if (tex.format() != dxt_fmt)
            tex = tex.copyAsFormat(dxt_fmt, pool);
    }
}

// ============================================================================
// DDS save preparation
// ============================================================================

void coerceDdsFormat(texn::Texture& tex, i32 dds_format, bool invert_y,
                     interfaces::WorkerPool* pool) {
    const bool is_bc3n = (dds_format == DDS_FORMAT_BC3N);
    const auto target = is_bc3n ? texn::PixelFormat::BC3 : DDS_PIXEL_FORMATS[dds_format];

    if (is_bc3n) {
        if (tex.format() != texn::PixelFormat::RGBA8)
            tex = tex.copyAsFormat(texn::PixelFormat::RGBA8, pool);
        if (invert_y)
            tex.invertChannel(texn::Channel::G);
        tex.swapChannels(texn::Channel::R, texn::Channel::A);
    } else if (invert_y) {
        if (texn::isBcn(tex.format()))
            tex = tex.copyAsFormat(texn::PixelFormat::RGBA8, pool);
        tex.invertChannel(texn::Channel::G);
    }
    if (tex.format() != target)
        tex = tex.copyAsFormat(target, pool);
}

// ============================================================================
// D4 TEX loading with payload / paylow fallback
// ============================================================================

std::optional<texn::Texture> loadD4TexWithFallback(texn::TextureConverter& converter,
                                                   const std::string& meta_path) {
    namespace fs = std::filesystem;
    const std::string payload = replaceMetaSegment(meta_path, "payload");
    const std::string paylow = replaceMetaSegment(meta_path, "paylow");

    const bool payload_exists = !payload.empty() && fs::exists(payload);
    const bool paylow_exists = !paylow.empty() && fs::exists(paylow);

    if (payload_exists && paylow_exists)
        return converter.loadTexD4(meta_path, payload, paylow);
    if (payload_exists)
        return converter.loadTexD4(meta_path, payload);
    if (paylow_exists)
        return converter.loadTexD4(meta_path, paylow);
    return std::nullopt;
}

} // namespace whiteout::textool::views
