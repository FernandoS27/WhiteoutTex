// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file save_helpers.h
/// @brief Shared BLP/DDS save-preparation and D4 TEX loading helpers
///        used by both SaveDialog and BatchConvert.

#include "common_types.h"
#include "format_registry.h"
#include "localization.h"
#include "save_dialog.h"
#include "texture_converter.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>

namespace whiteout::textool::views {

// ============================================================================
// Registry-driven file-dialog filters
// ============================================================================

/// Build SDL file-dialog filters from the format registry for every format with
/// capability @p cap, in registry order.  When @p allSupported is set, an
/// "All Supported Images" entry (the union of every included format's pattern)
/// is prepended; when @p allFiles is set an "All Files" ("*") entry is appended.
///
/// The result is cached and returned by reference; the backing strings and the
/// filter array therefore have static lifetime, which is required because SDL's
/// file dialogs run asynchronously and read the array after this call returns.
inline const std::vector<SDL_DialogFileFilter>&
dialogFiltersFor(whiteout::textures::FmtCap cap, bool allSupported, bool allFiles) {
    namespace tex = whiteout::textures;

    struct Built {
        std::string aggregate;                   ///< Backs the "All Supported" pattern.
        std::vector<SDL_DialogFileFilter> filters;
    };
    // Keyed by (cap, allSupported, allFiles).  std::map never relocates mapped
    // values, so references handed out here stay valid.
    static std::map<u32, Built> cache;

    const u32 key = (static_cast<u32>(cap) << 2) | (allSupported ? 2u : 0u) | (allFiles ? 1u : 0u);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.filters;

    Built b;
    if (allSupported) {
        for (const auto& row : tex::formatTable()) {
            if (!row.has(cap))
                continue;
            if (!b.aggregate.empty())
                b.aggregate += ';';
            b.aggregate += row.dialogPattern;
        }
    }
    auto [ins, _] = cache.emplace(key, std::move(b));
    Built& stored = ins->second;
    if (allSupported)
        stored.filters.push_back({"All Supported Images", stored.aggregate.c_str()});
    for (const auto& row : tex::formatTable())
        if (row.has(cap))
            stored.filters.push_back({row.dialogName, row.dialogPattern});
    if (allFiles)
        stored.filters.push_back({"All Files", "*"});
    return stored.filters;
}

// ============================================================================
// Shared channel array
// ============================================================================

/// The four RGBA channels in order, used by Multikind iteration and
/// per-channel upscaling.
inline constexpr whiteout::textures::Channel kRGBAChannels[] = {
    whiteout::textures::Channel::R,
    whiteout::textures::Channel::G,
    whiteout::textures::Channel::B,
    whiteout::textures::Channel::A,
};

// ============================================================================
// BLP encoding validation
// ============================================================================

/// Allowed encoding indices for BLP1 (Paletted=2, JPEG=3).
inline constexpr i32 BLP1_ENCODINGS[] = {2, 3};

/// Allowed encoding indices for BLP2 (all seven).
inline constexpr i32 BLP2_ENCODINGS[] = {0, 1, 2, 3, 4, 5, 6};

/// Return the allowed encoding set for the given BLP version combo-box index.
inline void blpAllowedEncodings(i32 blp_version, const i32*& out_allowed, i32& out_count) noexcept {
    if (blp_version == 0) { // BLP1
        out_allowed = BLP1_ENCODINGS;
        out_count = static_cast<i32>(std::size(BLP1_ENCODINGS));
    } else {
        out_allowed = BLP2_ENCODINGS;
        out_count = static_cast<i32>(std::size(BLP2_ENCODINGS));
    }
}

/// Clamp @p encoding to the set valid for `blp_version`. Returns true if it was already valid.
inline bool validateBlpEncoding(i32 blp_version, i32& encoding) noexcept {
    const i32* allowed;
    i32 count;
    blpAllowedEncodings(blp_version, allowed, count);
    for (i32 i = 0; i < count; ++i)
        if (allowed[i] == encoding)
            return true;
    encoding = allowed[0];
    return false;
}

// ============================================================================
// DDS format presets by texture-kind group
// ============================================================================

inline constexpr i32 DDS_PRESET_NORMAL[] = {0, 5, 8};

inline constexpr i32 DDS_PRESET_CHANNEL[] = {0, 4};

inline constexpr i32 DDS_PRESET_OTHER[] = {0, 1, 2, 3, 6, 7};

/// All DDS format indices (used for batch convert "General" mode).
inline constexpr i32 DDS_ALL[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};

/// Returns true for channel-map texture kinds (Roughness, Metalness, AO, Gloss).
inline bool isChannelMapKind(whiteout::textures::TextureKind kind) noexcept {
    return kind == whiteout::textures::TextureKind::Roughness ||
           kind == whiteout::textures::TextureKind::Metalness ||
           kind == whiteout::textures::TextureKind::AmbientOcclusion ||
           kind == whiteout::textures::TextureKind::Gloss;
}

/// Select the DDS format preset array for the given texture kind.
inline void ddsPresetForKind(whiteout::textures::TextureKind kind, const i32*& out_allowed,
                             i32& out_count) noexcept {
    if (kind == whiteout::textures::TextureKind::Normal) {
        out_allowed = DDS_PRESET_NORMAL;
        out_count = static_cast<i32>(std::size(DDS_PRESET_NORMAL));
    } else if (isChannelMapKind(kind)) {
        out_allowed = DDS_PRESET_CHANNEL;
        out_count = static_cast<i32>(std::size(DDS_PRESET_CHANNEL));
    } else {
        out_allowed = DDS_PRESET_OTHER;
        out_count = static_cast<i32>(std::size(DDS_PRESET_OTHER));
    }
}

/// Clamp @p dds_format to the set valid for @p kind.  Returns true if it was already valid.
inline bool validateDdsFormat(whiteout::textures::TextureKind kind, i32& dds_format) noexcept {
    const i32* allowed;
    i32 count;
    ddsPresetForKind(kind, allowed, count);
    for (i32 i = 0; i < count; ++i)
        if (allowed[i] == dds_format)
            return true;
    dds_format = allowed[0];
    return false;
}

// ============================================================================
// DDS pixel format table
// ============================================================================

/// Maps DDS combo-box indices 0–7 to PixelFormat values.
inline constexpr whiteout::textures::PixelFormat DDS_PIXEL_FORMATS[] = {
    whiteout::textures::PixelFormat::RGBA8, whiteout::textures::PixelFormat::BC1,
    whiteout::textures::PixelFormat::BC2,   whiteout::textures::PixelFormat::BC3,
    whiteout::textures::PixelFormat::BC4,   whiteout::textures::PixelFormat::BC5,
    whiteout::textures::PixelFormat::BC6H,  whiteout::textures::PixelFormat::BC7,
};

// ============================================================================
// BLP save preparation
// ============================================================================

/// Populate a BlpSaveOptions struct from GUI combo-box indices
/// and coerce the texture to the required pixel format.
whiteout::textures::blp::SaveOptions buildBlpSaveOptions(i32 blp_version, i32 blp_encoding,
                                                         bool dither, f32 dither_strength,
                                                         i32 jpeg_quality,
                                                         bool jpeg_progressive) noexcept;

/// Coerce @p tex to the pixel format required by the chosen BLP encoding.
void coerceBlpFormat(whiteout::textures::Texture& tex, i32 blp_encoding,
                     whiteout::textures::blp::BlpEncoding enc,
                     whiteout::interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// DDS save preparation
// ============================================================================

/// Apply DDS target-format conversion, including BC3N channel swapping and
/// optional Y-channel inversion. Call after mipmap generation.
void coerceDdsFormat(whiteout::textures::Texture& tex, i32 dds_format, bool invert_y,
                     whiteout::interfaces::WorkerPool* pool = nullptr);

// ============================================================================
// DDS format validation (raw array overload)
// ============================================================================

/// Clamp @p fmt to the given @p allowed set.  Used by drawDdsFormatCombo.
inline void validateDdsFormatRaw(i32& fmt, const i32* allowed, i32 count) noexcept {
    for (i32 i = 0; i < count; ++i)
        if (allowed[i] == fmt)
            return;
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
inline whiteout::u32 effectiveMipCount(MipmapMode mode, i32 customCount,
                                       const whiteout::textures::Texture& tex) {
    using namespace whiteout::textures;
    switch (mode) {
    case MipmapMode::Maximum:
        return computeMaxMipCount(tex.width(), tex.height());
    case MipmapMode::Custom: {
        const u32 maxMips = computeMaxMipCount(tex.width(), tex.height());
        return static_cast<u32>(std::clamp(customCount, 1, static_cast<i32>(maxMips)));
    }
    default:
        return 0; // KeepOriginal
    }
}

// ============================================================================
// Shared downscale options
// ============================================================================

/// Downscale factor labels for combo boxes in both image_details and batch_convert.
inline constexpr const char* kDownscaleOptions[] = {"x2 (halve)", "x4 (quarter)"};
inline constexpr i32 kDownscaleOptionCount = static_cast<i32>(std::size(kDownscaleOptions));

/// Canonical output extension (incl. dot) for a batch output-format combo index.
/// Derived from the registry's FmtCap::BatchOut slice, so it stays in lock-step
/// with the combo's names automatically.  Empty string if the index is invalid.
inline std::string batchOutputExtension(i32 outputFormatIndex) {
    namespace tex = whiteout::textures;
    const auto fmt = tex::capSliceAt(tex::FmtCap::BatchOut, outputFormatIndex);
    const auto* info = tex::formatInfo(fmt);
    return (info && !info->exts.empty()) ? std::string(info->exts.front()) : std::string();
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
        for (i32 i = 0; i < 4; ++i)
            tex.setChannelKind(kRGBAChannels[i], ch[i]);
    }
}

// ============================================================================
// Shared BLP options UI
// ============================================================================

/// Draw the BLP options panel (version, encoding, dither, JPEG quality/progressive).
/// Used by both SaveDialog and BatchConvert.
inline void drawBlpOptionsUI(i32& blp_version, i32& blp_encoding, bool& blp_dither,
                             f32& blp_dither_strength, i32& jpeg_quality, bool& jpeg_progressive) {
    ImGui::SeparatorText(i18n::tr("save.blp_options"));
    {
        const char* version_items[] = {i18n::tr("save.blp_version_blp1"),
                                       i18n::tr("save.blp_version_blp2")};
        ImGui::Combo(i18n::tr("save.blp_version"), &blp_version, version_items,
                     static_cast<i32>(std::size(version_items)));
    }
    {
        const i32* enc_allowed;
        i32 enc_count;
        blpAllowedEncodings(blp_version, enc_allowed, enc_count);
        validateBlpEncoding(blp_version, blp_encoding);

        if (ImGui::BeginCombo(i18n::tr("save.encoding"), BLP_ENCODING_NAMES[blp_encoding])) {
            for (i32 i = 0; i < enc_count; ++i) {
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
        ImGui::Checkbox(i18n::tr("save.dither"), &blp_dither);
        if (blp_dither)
            ImGui::SliderFloat(i18n::tr("save.dither_strength"), &blp_dither_strength, 0.0f, 1.0f);
    }
    if (blp_encoding == 3) {
        ImGui::SliderInt(i18n::tr("save.jpeg_quality"), &jpeg_quality, 1, 100);
        ImGui::Checkbox(i18n::tr("save.jpeg_progressive"), &jpeg_progressive);
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
        for (auto ch : kRGBAChannels)
            dst.setChannelKind(ch, src.channelKind(ch));
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
inline std::optional<std::string> withBcnRoundtrip(const whiteout::textures::Texture& source,
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

} // namespace whiteout::textool::views
