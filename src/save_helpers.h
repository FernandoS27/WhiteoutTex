// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file save_helpers.h
/// @brief Shared BLP/DDS save-preparation and D4 TEX loading helpers
///        used by both SaveDialog and BatchConvert.

#include "common_types.h"
#include "save_dialog.h"
#include "texture_converter.h"

#include <filesystem>
#include <optional>

#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::gui {

// ============================================================================
// BLP encoding validation
// ============================================================================

/// Allowed encoding indices for BLP1 (Paletted=2, JPEG=3).
inline constexpr int BLP1_ENCODINGS[]     = {2, 3};
inline constexpr int BLP1_ENCODING_COUNT  = 2;

/// Allowed encoding indices for BLP2 (all seven).
inline constexpr int BLP2_ENCODINGS[]     = {0, 1, 2, 3, 4, 5, 6};
inline constexpr int BLP2_ENCODING_COUNT  = 7;

/// Return the allowed encoding set for the given BLP version combo-box index.
inline void blpAllowedEncodings(int blp_version, const int*& out_allowed, int& out_count) noexcept {
    if (blp_version == 0) { // BLP1
        out_allowed = BLP1_ENCODINGS;
        out_count   = BLP1_ENCODING_COUNT;
    } else {
        out_allowed = BLP2_ENCODINGS;
        out_count   = BLP2_ENCODING_COUNT;
    }
}

/// Clamp @p encoding to the set valid for `blp_version`. Returns true if it was already valid.
inline bool validateBlpEncoding(int blp_version, int& encoding) noexcept {
    const int* allowed;
    int count;
    blpAllowedEncodings(blp_version, allowed, count);
    for (int i = 0; i < count; ++i)
        if (allowed[i] == encoding) return true;
    encoding = allowed[0];
    return false;
}

// ============================================================================
// DDS format presets by texture-kind group
// ============================================================================

inline constexpr int DDS_PRESET_NORMAL[]       = {0, 5, 8};
inline constexpr int DDS_PRESET_NORMAL_COUNT   = 3;

inline constexpr int DDS_PRESET_CHANNEL[]      = {0, 4};
inline constexpr int DDS_PRESET_CHANNEL_COUNT  = 2;

inline constexpr int DDS_PRESET_OTHER[]        = {0, 1, 2, 3, 6, 7};
inline constexpr int DDS_PRESET_OTHER_COUNT    = 6;

/// Returns true for channel-map texture kinds (Roughness, Metalness, AO, Gloss).
inline bool isChannelMapKind(whiteout::textures::TextureKind kind) noexcept {
    return kind == whiteout::textures::TextureKind::Roughness ||
           kind == whiteout::textures::TextureKind::Metalness ||
           kind == whiteout::textures::TextureKind::AmbientOcclusion ||
           kind == whiteout::textures::TextureKind::Gloss;
}

/// Select the DDS format preset array for the given texture kind.
inline void ddsPresetForKind(whiteout::textures::TextureKind kind,
                             const int*& out_allowed, int& out_count) noexcept {
    if (kind == whiteout::textures::TextureKind::Normal) {
        out_allowed = DDS_PRESET_NORMAL;
        out_count   = DDS_PRESET_NORMAL_COUNT;
    } else if (isChannelMapKind(kind)) {
        out_allowed = DDS_PRESET_CHANNEL;
        out_count   = DDS_PRESET_CHANNEL_COUNT;
    } else {
        out_allowed = DDS_PRESET_OTHER;
        out_count   = DDS_PRESET_OTHER_COUNT;
    }
}

/// Clamp @p dds_format to the set valid for @p kind.  Returns true if it was already valid.
inline bool validateDdsFormat(whiteout::textures::TextureKind kind, int& dds_format) noexcept {
    const int* allowed;
    int count;
    ddsPresetForKind(kind, allowed, count);
    for (int i = 0; i < count; ++i)
        if (allowed[i] == dds_format) return true;
    dds_format = allowed[0];
    return false;
}

// ============================================================================
// DDS pixel format table
// ============================================================================

/// Maps DDS combo-box indices 0–7 to PixelFormat values.
inline constexpr whiteout::textures::PixelFormat DDS_PIXEL_FORMATS[] = {
    whiteout::textures::PixelFormat::RGBA8,
    whiteout::textures::PixelFormat::BC1,
    whiteout::textures::PixelFormat::BC2,
    whiteout::textures::PixelFormat::BC3,
    whiteout::textures::PixelFormat::BC4,
    whiteout::textures::PixelFormat::BC5,
    whiteout::textures::PixelFormat::BC6H,
    whiteout::textures::PixelFormat::BC7,
};

// ============================================================================
// BLP save preparation
// ============================================================================

/// Populate a BlpSaveOptions struct from GUI combo-box indices
/// and coerce the texture to the required pixel format.
inline whiteout::textures::blp::SaveOptions buildBlpSaveOptions(
    int blp_version, int blp_encoding,
    bool dither, float dither_strength, int jpeg_quality) noexcept {
    namespace blp = whiteout::textures::blp;
    blp::SaveOptions opts;
    opts.version  = blp_version == 0 ? blp::BlpVersion::BLP1 : blp::BlpVersion::BLP2;
    opts.encoding = toBlpEncoding(blp_encoding);
    opts.dither   = dither;
    opts.ditherStrength = dither_strength;
    opts.jpegQuality    = jpeg_quality;

    // Force BLP1 for encodings that require it.
    if (opts.encoding == blp::BlpEncoding::JPEG ||
        opts.encoding == blp::BlpEncoding::Palettized)
        opts.version = blp::BlpVersion::BLP1;

    return opts;
}

/// Coerce @p tex to the pixel format required by the chosen BLP encoding.
inline void coerceBlpFormat(whiteout::textures::Texture& tex, int blp_encoding,
                            whiteout::textures::blp::BlpEncoding enc) {
    namespace blp = whiteout::textures::blp;
    if (enc == blp::BlpEncoding::JPEG || enc == blp::BlpEncoding::Palettized ||
        enc == blp::BlpEncoding::BGRA || enc == blp::BlpEncoding::Infer) {
        if (tex.format() != whiteout::textures::PixelFormat::RGBA8)
            tex = tex.copyAsFormat(whiteout::textures::PixelFormat::RGBA8);
    } else { // DXT subtype: pick BC pixel format by index (4→BC1, 5→BC2, 6→BC3)
        const auto dxt_fmt = blpDxtPixelFormat(blp_encoding);
        if (tex.format() != dxt_fmt)
            tex = tex.copyAsFormat(dxt_fmt);
    }
}

// ============================================================================
// DDS save preparation
// ============================================================================

/// Apply DDS target-format conversion, including BC3N channel swapping and
/// optional Y-channel inversion. Call after mipmap generation.
inline void coerceDdsFormat(whiteout::textures::Texture& tex, int dds_format, bool invert_y) {
    namespace texn = whiteout::textures;
    const bool is_bc3n = (dds_format == DDS_FORMAT_BC3N);
    const auto target  = is_bc3n ? texn::PixelFormat::BC3 : DDS_PIXEL_FORMATS[dds_format];

    if (is_bc3n) {
        if (tex.format() != texn::PixelFormat::RGBA8)
            tex = tex.copyAsFormat(texn::PixelFormat::RGBA8);
        if (invert_y)
            tex.invertChannel(texn::Channel::G);
        tex.swapChannels(texn::Channel::R, texn::Channel::A);
    } else if (invert_y) {
        if (texn::isBcn(tex.format()))
            tex = tex.copyAsFormat(texn::PixelFormat::RGBA8);
        tex.invertChannel(texn::Channel::G);
    }
    if (tex.format() != target)
        tex = tex.copyAsFormat(target);
}

// ============================================================================
// DDS format validation (raw array overload)
// ============================================================================

/// Clamp @p fmt to the given @p allowed set.  Used by drawDdsFormatCombo.
inline void validateDdsFormatRaw(int& fmt, const int* allowed, int count) noexcept {
    for (int i = 0; i < count; ++i)
        if (allowed[i] == fmt) return;
    fmt = allowed[0];
}

// ============================================================================
// D4 TEX loading with payload / paylow fallback
// ============================================================================

/// Try to auto-resolve and load a Diablo IV TEX file by guessing the payload
/// and paylow paths from the meta path.  Returns std::nullopt if no payload
/// file could be found or the load failed.
inline std::optional<whiteout::textures::Texture> loadD4TexWithFallback(
    whiteout::textures::TextureConverter& converter, const std::string& meta_path) {
    namespace fs = std::filesystem;
    const std::string payload = replaceMetaSegment(meta_path, "payload");
    const std::string paylow  = replaceMetaSegment(meta_path, "paylow");

    const bool payload_exists = !payload.empty() && fs::exists(payload);
    const bool paylow_exists  = !paylow.empty() && fs::exists(paylow);

    if (payload_exists && paylow_exists)
        return converter.loadTexD4(meta_path, payload, paylow);
    if (payload_exists)
        return converter.loadTexD4(meta_path, payload);
    if (paylow_exists)
        return converter.loadTexD4(meta_path, paylow);
    return std::nullopt;
}

} // namespace whiteout::gui
