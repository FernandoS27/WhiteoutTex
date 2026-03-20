// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file texture_service.h
/// @brief Business logic for texture loading, preparation, and pixel operations.
///        No SDL or ImGui dependencies.

#include "common_types.h"
#include "texture_converter.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/textures/texture.h>

namespace whiteout::gui {

/// Result of a texture load operation.
struct TextureLoadResult {
    std::optional<whiteout::textures::Texture> texture;
    whiteout::textures::TextureFileFormat file_format =
        whiteout::textures::TextureFileFormat::Unknown;
    whiteout::textures::PixelFormat source_fmt = whiteout::textures::PixelFormat::RGBA8;
    std::string error_message;

    /// True if the texture is BC3 Normal and may need BC3N channel swap.
    bool needs_bc3n_dialog = false;

    /// True if a D4 TEX file needs manual payload path specification.
    bool needs_d4_payload_dialog = false;
    std::string d4_meta_path;
    std::string d4_payload_prefill;
};

/// Result of a mipmap/downscale/transform operation.
struct TextureOpResult {
    bool success = false;
    std::string message;
};

/// Business logic for texture loading, preparation, and pixel operations.
class TextureService {
public:
    explicit TextureService(whiteout::textures::TextureConverter& converter);

    /// Load a texture from a file path.  Handles D4 TEX detection,
    /// kind guessing, and BCn decompression.
    TextureLoadResult loadFromFile(const std::string& path);

    /// Load a D4 TEX with explicit payload path(s).
    TextureLoadResult loadD4WithPayload(const std::string& meta_path,
                                        const std::string& payload_path,
                                        const std::string& paylow_path = {});

    /// Load a texture from an in-memory buffer with an explicit format.
    TextureLoadResult loadFromMemory(const std::string& name, std::span<const u8> data,
                                     whiteout::textures::TextureFileFormat fmt);

    /// Load a D4 TEX from in-memory buffers (meta + payload + optional paylow).
    TextureLoadResult loadD4FromMemory(const std::string& name, std::span<const u8> meta,
                                       std::span<const u8> payload,
                                       std::span<const u8> paylow = {});

    /// Regenerate mipmaps on the texture (with BCn round-trip if needed).
    TextureOpResult regenerateMipmaps(whiteout::textures::Texture& texture, u32 mip_count);

    /// Downscale the texture by the given number of levels (with BCn round-trip if needed).
    TextureOpResult downscale(whiteout::textures::Texture& texture, u32 levels);

    /// Apply BC3N normal-map channel swap (R↔A, invert G).
    static void applyBC3NSwap(whiteout::textures::Texture& texture);

    /// Build an RGBA8 display copy (handles normal-map expansion, single-channel broadcast).
    static whiteout::textures::Texture makeDisplayTexture(
        const whiteout::textures::Texture& texture,
        whiteout::interfaces::WorkerPool* pool = nullptr);

    /// Apply per-channel visibility filter to RGBA8 pixel data.
    static std::vector<u8> applyChannelFilter(const u8* data, i32 width, i32 height, bool show_r,
                                              bool show_g, bool show_b, bool show_a);

private:
    /// Apply kind guessing, BCn decompression, and source format tracking.
    TextureLoadResult prepare(const std::string& path, whiteout::textures::Texture texture);

    whiteout::textures::TextureConverter& converter_;
};

} // namespace whiteout::gui
