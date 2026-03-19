// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file save_helpers.h
/// @brief Shared BLP/DDS save-preparation and D4 TEX loading helpers
///        used by both SaveDialog and BatchConvert.

#include "common_types.h"
#include "save_dialog.h"
#include "texture_converter.h"

#include <optional>

#include <whiteout/interfaces.h>
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

/// All DDS format indices (used for batch convert "General" mode).
inline constexpr int DDS_ALL[]       = {0, 1, 2, 3, 4, 5, 6, 7, 8};
inline constexpr int DDS_ALL_COUNT   = 9;

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
whiteout::textures::blp::SaveOptions buildBlpSaveOptions(
    int blp_version, int blp_encoding,
    bool dither, float dither_strength, int jpeg_quality) noexcept;

/// Coerce @p tex to the pixel format required by the chosen BLP encoding.
void coerceBlpFormat(whiteout::textures::Texture& tex, int blp_encoding,
                     whiteout::textures::blp::BlpEncoding enc,
                     whiteout::interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// DDS save preparation
// ============================================================================

/// Apply DDS target-format conversion, including BC3N channel swapping and
/// optional Y-channel inversion. Call after mipmap generation.
void coerceDdsFormat(whiteout::textures::Texture& tex, int dds_format, bool invert_y,
                     whiteout::interfaces::WorkerPool* pool = nullptr);

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
std::optional<whiteout::textures::Texture> loadD4TexWithFallback(
    whiteout::textures::TextureConverter& converter, const std::string& meta_path);

// ============================================================================
// Mipmap mode helpers
// ============================================================================

/// Compute the effective mip count to pass to generateMipmaps().
/// Returns 0 if no generation should happen (KeepOriginal mode).
inline whiteout::u32 effectiveMipCount(MipmapMode mode, int customCount,
                                       const whiteout::textures::Texture& tex) {
    using namespace whiteout::textures;
    switch (mode) {
    case MipmapMode::Maximum:
        return computeMaxMipCount(tex.width(), tex.height());
    case MipmapMode::Custom: {
        const u32 maxMips = computeMaxMipCount(tex.width(), tex.height());
        return static_cast<u32>(std::clamp(customCount, 1, static_cast<int>(maxMips)));
    }
    default:
        return 0; // KeepOriginal
    }
}

// ============================================================================
// Texture kind guessing helper
// ============================================================================

/// Guess the texture kind from a filename and assign it (including per-channel
/// kinds for Multikind textures).  Consolidates the duplicated pattern in
/// App::applyLoadedTexture() and BatchConvert::workerFunc().
inline void applyGuessedKind(whiteout::textures::Texture& tex, const std::string& path) {
    namespace t = whiteout::textures;
    auto kind = t::TextureConverter::guessTextureKind(path, tex.format());
    tex.setKind(kind);
    if (kind == t::TextureKind::Multikind) {
        auto ch = t::TextureConverter::guessTextureMultiKind(path, tex.format());
        tex.setChannelKind(t::Channel::R, ch[0]);
        tex.setChannelKind(t::Channel::G, ch[1]);
        tex.setChannelKind(t::Channel::B, ch[2]);
        tex.setChannelKind(t::Channel::A, ch[3]);
    }
}

// ============================================================================
// Shared BLP options UI
// ============================================================================

/// Draw the BLP options panel (version, encoding, dither, JPEG quality).
/// Used by both SaveDialog and BatchConvert.
inline void drawBlpOptionsUI(int& blp_version, int& blp_encoding,
                             bool& blp_dither, float& blp_dither_strength,
                             int& jpeg_quality) {
    ImGui::SeparatorText("BLP Options");
    ImGui::Combo("BLP Version", &blp_version,
                 "BLP1 (Warcraft 3 Classic)\0BLP2 (WoW)\0");
    {
        const int* enc_allowed;
        int enc_count;
        blpAllowedEncodings(blp_version, enc_allowed, enc_count);
        validateBlpEncoding(blp_version, blp_encoding);

        if (ImGui::BeginCombo("Encoding", BLP_ENCODING_NAMES[blp_encoding])) {
            for (int i = 0; i < enc_count; ++i) {
                bool selected = (blp_encoding == enc_allowed[i]);
                if (ImGui::Selectable(BLP_ENCODING_NAMES[enc_allowed[i]], selected))
                    blp_encoding = enc_allowed[i];
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    if (blp_encoding == 2) {
        ImGui::Checkbox("Dither", &blp_dither);
        if (blp_dither)
            ImGui::SliderFloat("Dither Strength", &blp_dither_strength, 0.0f, 1.0f);
    }
    if (blp_encoding == 3) {
        ImGui::SliderInt("JPEG Quality", &jpeg_quality, 1, 100);
    }
}

// ============================================================================
// Kind metadata transfer
// ============================================================================

/// Copy texture kind metadata (kind, sRGB, per-channel kinds) from @p src to @p dst.
/// Useful after format conversions that don't preserve metadata.
inline void copyKindMetadata(whiteout::textures::Texture& dst,
                             const whiteout::textures::Texture& src) {
    dst.setKind(src.kind());
    dst.setSrgb(src.isSrgb());
    if (src.kind() == whiteout::textures::TextureKind::Multikind) {
        dst.setChannelKind(whiteout::textures::Channel::R,
                           src.channelKind(whiteout::textures::Channel::R));
        dst.setChannelKind(whiteout::textures::Channel::G,
                           src.channelKind(whiteout::textures::Channel::G));
        dst.setChannelKind(whiteout::textures::Channel::B,
                           src.channelKind(whiteout::textures::Channel::B));
        dst.setChannelKind(whiteout::textures::Channel::A,
                           src.channelKind(whiteout::textures::Channel::A));
    }
}

// ============================================================================
// BCn-aware texture operation wrapper
// ============================================================================

/// Execute an operation on a texture, handling BCn decompression/recompression
/// and kind preservation automatically.
/// @p op is called as op(Texture& work, WorkerPool* pool) and should return
/// std::optional<std::string> — nullopt on success, error message on failure.
template <typename Op>
inline std::optional<std::string> withBcnRoundtrip(
    const whiteout::textures::Texture& source,
    whiteout::interfaces::WorkerPool* pool,
    whiteout::textures::Texture& out_result,
    Op&& op) {
    using namespace whiteout::textures;
    auto work = source;
    const PixelFormat original_fmt = work.format();
    if (isBcn(original_fmt)) {
        work = work.copyAsFormat(workingFormatFor(original_fmt), pool);
        copyKindMetadata(work, source);
    }
    if (auto err = op(work, pool)) {
        return err;
    }
    if (isBcn(original_fmt)) {
        work = work.copyAsFormat(original_fmt, pool);
    }
    copyKindMetadata(work, source);
    out_result = std::move(work);
    return std::nullopt;
}

} // namespace whiteout::gui
