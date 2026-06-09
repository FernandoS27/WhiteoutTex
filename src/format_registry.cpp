// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "format_registry.h"

namespace whiteout::textures {

namespace {

using namespace std::string_view_literals;

// Per-format extension arrays (static lifetime; referenced by the table spans).
constexpr std::string_view kExtBlp[] = {".blp"sv};
constexpr std::string_view kExtBmp[] = {".bmp"sv};
constexpr std::string_view kExtDds[] = {".dds"sv};
constexpr std::string_view kExtJpeg[] = {".jpg"sv, ".jpeg"sv};
constexpr std::string_view kExtPng[] = {".png"sv};
constexpr std::string_view kExtPsd[] = {".psd"sv};
constexpr std::string_view kExtTex[] = {".tex"sv};
constexpr std::string_view kExtTga[] = {".tga"sv};
constexpr std::string_view kExtTiff[] = {".tif"sv, ".tiff"sv};
constexpr std::string_view kExtD2r[] = {".texture"sv};

constexpr FmtCap kAll =
    FmtCap::Read | FmtCap::Write | FmtCap::BatchIn | FmtCap::BatchOut | FmtCap::Archive;

// ── The single source of truth ──────────────────────────────────────────────
//
// Row order is load-bearing.  Two slices have indices persisted in the INI and
// must keep their historical order:
//   * BatchOut → BLP,BMP,DDS,JPEG,PNG,TGA,TIFF,D2R  (BatchPrefs::output_format)
//   * Write    → BLP,BMP,DDS,JPEG,PNG,PSD,TGA,TIFF,D2R  (SavePrefs::last_filter)
// Per-format caps reproduce the exact (historically inconsistent) subsets:
//   PSD has no batch/archive presence; TEX is read-only & not a batch output;
//   TIFF/PSD are not archive-browsable.
constexpr FormatInfo kTable[] = {
    {TextureFileFormat::BLP, "BLP", "BLP (Blizzard Picture)", "blp", kExtBlp, kAll},
    {TextureFileFormat::BMP, "BMP", "BMP (Bitmap)", "bmp", kExtBmp, kAll},
    {TextureFileFormat::DDS, "DDS", "DDS (DirectDraw Surface)", "dds", kExtDds, kAll},
    {TextureFileFormat::JPEG, "JPEG", "JPEG", "jpg;jpeg", kExtJpeg, kAll},
    {TextureFileFormat::PNG, "PNG", "PNG", "png", kExtPng, kAll},
    {TextureFileFormat::PSD, "PSD", "PSD (Adobe Photoshop)", "psd", kExtPsd,
     FmtCap::Read | FmtCap::Write},
    {TextureFileFormat::TEX, "TEX", "TEX (Blizzard Proprietary)", "tex", kExtTex,
     FmtCap::Read | FmtCap::BatchIn | FmtCap::Archive},
    {TextureFileFormat::TGA, "TGA", "TGA (Targa)", "tga", kExtTga, kAll},
    {TextureFileFormat::TIFF, "TIFF", "TIFF (Tagged Image File Format)", "tif;tiff", kExtTiff,
     FmtCap::Read | FmtCap::Write | FmtCap::BatchIn | FmtCap::BatchOut},
    {TextureFileFormat::D2R, "D2R", "D2R (Diablo II Resurrected)", "texture", kExtD2r, kAll},
};

} // namespace

std::span<const FormatInfo> formatTable() {
    return kTable;
}

TextureFileFormat classifyExtension(std::string_view extLower) {
    for (const auto& row : kTable)
        for (const auto& e : row.exts)
            if (e == extLower)
                return row.fmt;
    return TextureFileFormat::Unknown;
}

const FormatInfo* formatInfo(TextureFileFormat fmt) {
    for (const auto& row : kTable)
        if (row.fmt == fmt)
            return &row;
    return nullptr;
}

bool formatHasCap(TextureFileFormat fmt, FmtCap cap) {
    const auto* row = formatInfo(fmt);
    return row && row->has(cap);
}

i32 capSliceIndex(FmtCap cap, TextureFileFormat fmt) {
    i32 idx = 0;
    for (const auto& row : kTable) {
        if (!row.has(cap))
            continue;
        if (row.fmt == fmt)
            return idx;
        ++idx;
    }
    return -1;
}

TextureFileFormat capSliceAt(FmtCap cap, i32 index) {
    if (index < 0)
        return TextureFileFormat::Unknown;
    i32 idx = 0;
    for (const auto& row : kTable) {
        if (!row.has(cap))
            continue;
        if (idx == index)
            return row.fmt;
        ++idx;
    }
    return TextureFileFormat::Unknown;
}

} // namespace whiteout::textures
