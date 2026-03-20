// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file texture_converter.h
 * @brief Unified texture loading, saving, and format classification
 *
 * TextureConverter provides a single entry point for reading and writing
 * textures across all supported file formats (BLP, BMP, DDS, TEX, TGA).
 * It also offers static utilities for format classification, display-name
 * lookup, and heuristic TextureKind guessing from file names and pixel
 * formats.
 */

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures {

/// Default JPEG quality used by save() when no explicit quality is given.
constexpr i32 kDefaultJpegQuality = 75;

// ============================================================================
// Texture File Format
// ============================================================================

/// Supported on-disk texture file formats.
enum class TextureFileFormat : u32 {
    BLP,     ///< Blizzard Picture (BLP1 / BLP2).
    BMP,     ///< Windows Bitmap (24/32-bit uncompressed).
    DDS,     ///< DirectDraw Surface (legacy + DX10 header).
    JPEG,    ///< JPEG (baseline sequential DCT, Y'CbCr / grayscale).
    PNG,     ///< Portable Network Graphics (8/16-bit, non-interlaced).
    TEX,     ///< Blizzard proprietary texture (Diablo III/IV).
    TGA,     ///< Truevision TGA (uncompressed + RLE).
    Unknown, ///< Unrecognised extension.
};

// ============================================================================
// BCn helpers
// ============================================================================

/// Returns true if @p fmt is a BCn block-compressed format (BC1–BC7).
inline bool isBcn(PixelFormat fmt) noexcept {
    return fmt >= PixelFormat::BC1 && fmt <= PixelFormat::BC7;
}

/// Returns the float working format appropriate for a BCn source texture.
/// BC4 → R32F (single-channel), BC5 → RG32F (two-channel), all others → RGBA32F.
inline PixelFormat workingFormatFor(PixelFormat fmt) noexcept {
    if (fmt == PixelFormat::BC4)
        return PixelFormat::R32F;
    if (fmt == PixelFormat::BC5)
        return PixelFormat::RG32F;
    return PixelFormat::RGBA32F;
}

/**
 * @brief Unified texture I/O facade
 *
 * Provides load / save operations that automatically dispatch to the
 * correct format-specific parser or writer based on the file extension.
 * Issues (warnings / errors) from the underlying parsers and writers are
 * accumulated and queryable via hasIssues() / getIssues().
 *
 * Static utilities are available for extension classification, enum-to-string
 * conversion, and heuristic TextureKind guessing.
 */
class TextureConverter {
public:
    TextureConverter();
    ~TextureConverter();

    TextureConverter(const TextureConverter&) = delete;
    TextureConverter& operator=(const TextureConverter&) = delete;

    // ── Static utilities ───────────────────────────────────────────────

    /// Classify a file path by its extension.
    static TextureFileFormat classifyPath(const std::string& path);

    /// Return true if the TEX file at @p path is a Diablo IV SNO (metadata-only) file.
    /// Returns false for Diablo III TEX files or unrecognised data.
    static bool isD4Tex(const std::string& path);

    /// Guess the semantic TextureKind from the file-name stem and pixel
    /// format.  Checks common suffixes (_n, _norm, _diff, _albedo, …)
    /// and falls back to pixel-format heuristics (BC5 → Normal, etc.).
    static TextureKind guessTextureKind(const std::string& path, PixelFormat fmt);

    /// When guessTextureKind() returns Multikind, call this to obtain the
    /// per-channel kinds (R, G, B, A).  Channels that carry no data are
    /// set to TextureKind::Unused.
    static std::array<TextureKind, 4> guessTextureMultiKind(const std::string& path,
                                                            PixelFormat fmt);

    /// Human-readable name for a TextureFileFormat value.
    static const char* fileFormatName(TextureFileFormat fmt);

    /// Human-readable name for a PixelFormat value.
    static const char* pixelFormatName(PixelFormat fmt);

    /// Human-readable name for a TextureType value.
    static const char* textureTypeName(TextureType type);

    /// Human-readable name for a TextureKind value.
    static const char* textureKindName(TextureKind kind);

    // ── Loading ────────────────────────────────────────────────────────

    /// Load a texture from disk.  The file format is inferred from the
    /// extension.  Returns std::nullopt on failure.
    std::optional<Texture> load(const std::string& path);

    /// Load a texture from disk using an explicit format override.
    std::optional<Texture> load(const std::string& path, TextureFileFormat fmt);

    /// Load a texture from an in-memory buffer with an explicit format.
    std::optional<Texture> load(std::span<const u8> data, TextureFileFormat fmt);

    /// Load a Diablo IV TEX file by providing both the metadata @p metaPath
    /// and the pixel-data @p payloadPath.  Returns std::nullopt on failure.
    std::optional<Texture> loadTexD4(const std::string& metaPath, const std::string& payloadPath);

    /// Load a Diablo IV TEX file with hi-res payload and low-res payload.
    /// @p paylowPath may be empty if no low-res payload is available.
    std::optional<Texture> loadTexD4(const std::string& metaPath, const std::string& payloadPath,
                                     const std::string& paylowPath);

    /// Load a Diablo IV TEX from in-memory buffers (meta + hi-res payload).
    std::optional<Texture> loadTexD4(std::span<const u8> meta, std::span<const u8> payload);

    /// Load a Diablo IV TEX from in-memory buffers (meta + hi + low payload).
    std::optional<Texture> loadTexD4(std::span<const u8> meta, std::span<const u8> payload,
                                     std::span<const u8> paylow);

    // ── Saving ─────────────────────────────────────────────────────────

    /// Save a texture to disk.  The file format is inferred from the
    /// extension.  Uses default BLP save options when writing BLP.
    /// @return true on success.
    bool save(const Texture& tex, const std::string& path);

    /// Save a texture to disk as BLP with explicit save options.
    /// @return true on success.
    bool save(const Texture& tex, const std::string& path, const blp::SaveOptions& blpOpts);

    /// Save a texture to disk as JPEG with explicit quality.
    /// @param quality JPEG quality level (1–100, default 75).
    /// @return true on success.
    bool save(const Texture& tex, const std::string& path, i32 jpegQuality);

    // ── Issue reporting ────────────────────────────────────────────────

    /// @return true if the last load/save produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last load or save call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures
