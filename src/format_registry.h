// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file format_registry.h
 * @brief Single source of truth for texture file-format metadata.
 *
 * Every UI / CLI / converter list that used to hard-code the set of supported
 * formats (extension tables, dialog filters, batch combos, archive scanners)
 * derives from formatTable() instead.  Adding a new format is normally a single
 * row here (plus a construction case in TextureConverter only if it needs
 * special load/save handling).
 *
 * This header is intentionally free of SDL / ImGui dependencies; it carries
 * plain data only.  SDL dialog-filter construction lives in views/save_helpers.h.
 */

#include <span>
#include <string_view>

#include <whiteout/common_types.h>

#include "texture_converter.h" // TextureFileFormat

namespace whiteout::textures {

/// Capability bits describing which surfaces a format participates in.  One bit
/// per surface so the divergent (and historically inconsistent) format subsets
/// are expressed declaratively in the table rather than duplicated per consumer.
enum class FmtCap : u32 {
    None = 0,
    Read = 1u << 0,     ///< load() + Open-file dialog.
    Write = 1u << 1,    ///< Save-file dialog.
    BatchIn = 1u << 2,  ///< Batch read-filter checkbox.
    BatchOut = 1u << 3, ///< Batch output-format combo.
    Archive = 1u << 4,  ///< MPQ / CASC browse list.
};

constexpr FmtCap operator|(FmtCap a, FmtCap b) noexcept {
    return static_cast<FmtCap>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr u32 operator&(FmtCap a, FmtCap b) noexcept {
    return static_cast<u32>(a) & static_cast<u32>(b);
}

/// One row of format metadata.  All string members have static lifetime, so
/// pointers obtained here remain valid for the duration of the program (relied
/// on by SDL's asynchronous file dialogs).
struct FormatInfo {
    TextureFileFormat fmt;
    const char* shortName;     ///< Display name, e.g. "BLP".
    const char* dialogName;    ///< File-dialog label, e.g. "BLP (Blizzard Picture)".
    const char* dialogPattern; ///< SDL filter pattern, e.g. "blp" or "jpg;jpeg".
    std::span<const std::string_view> exts; ///< Canonical extensions incl. dot, lowercase.
    FmtCap caps;               ///< OR of FmtCap surfaces this format participates in.

    bool has(FmtCap c) const noexcept {
        return (caps & c) != 0;
    }
};

/// The single source of truth.  Ordering is load-bearing: the BatchOut- and
/// Write-filtered slices must preserve the historical combo/filter indices that
/// are persisted in the INI (see format_registry.cpp).
std::span<const FormatInfo> formatTable();

/// Classify a lowercase extension (including the leading dot, e.g. ".png").
/// Returns TextureFileFormat::Unknown if no row matches.
TextureFileFormat classifyExtension(std::string_view extLower);

/// Look up the row for @p fmt, or nullptr if not in the table.
const FormatInfo* formatInfo(TextureFileFormat fmt);

/// True if @p fmt participates in surface @p cap.
bool formatHasCap(TextureFileFormat fmt, FmtCap cap);

/// Position of @p fmt within the @p cap-filtered slice of the table (in table
/// order), or -1 if @p fmt lacks that capability.  The slice order matches the
/// indices persisted in the INI for the BatchOut (output_format) and Write
/// (save dialog last_filter) surfaces.
i32 capSliceIndex(FmtCap cap, TextureFileFormat fmt);

/// Inverse of capSliceIndex(): the format at position @p index within the
/// @p cap-filtered slice, or TextureFileFormat::Unknown if out of range.
TextureFileFormat capSliceAt(FmtCap cap, i32 index);

} // namespace whiteout::textures
