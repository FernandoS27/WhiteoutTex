// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file fs_utf8.h
/// @brief UTF-8 <-> std::filesystem::path conversions.
///
/// Every narrow string in the app is UTF-8 (SDL dialogs and base path, JSON,
/// INI catalogs).  On Windows, std::filesystem::path(std::string) and
/// path::string() instead convert through the legacy ANSI codepage, which
/// garbles non-ASCII (e.g. CJK) characters.  These helpers route through
/// char8_t so the bytes are always interpreted as UTF-8, on every platform —
/// the same approach WhiteoutLib uses internally for texture I/O.  Construct
/// fstreams from the returned path object (never from a narrow string).

#include <filesystem>
#include <string>
#include <string_view>

namespace whiteout::textool {

/// Interpret a UTF-8 string as a filesystem path.
inline std::filesystem::path utf8ToPath(std::string_view utf8) {
    const auto* b = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(b, b + utf8.size());
}

/// Render a path as a UTF-8 string (native separators).
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

/// Render a path as a UTF-8 string with generic (forward-slash) separators —
/// the form stored in pipeline JSON and used as catalog keys.
inline std::string pathToUtf8Generic(const std::filesystem::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

} // namespace whiteout::textool
