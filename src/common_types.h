// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textool {

// ============================================================================
// Rust-style type aliases (mirrors whiteout::)
// ============================================================================

using u8 = whiteout::u8;
using u16 = whiteout::u16;
using u32 = whiteout::u32;
using u64 = whiteout::u64;

using i8 = whiteout::i8;
using i16 = whiteout::i16;
using i32 = whiteout::i32;
using i64 = whiteout::i64;

using f32 = whiteout::f32;
using f64 = whiteout::f64;
using f16 = whiteout::f16;

// ============================================================================
// Constants
// ============================================================================

/// Standard buffer size for OS path character arrays.
inline constexpr std::size_t PATH_BUFFER_SIZE = 4096;

// ============================================================================
// String utilities
// ============================================================================

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Safely copy a string into a fixed-size char buffer with null termination.
template <std::size_t N>
inline void copyToBuffer(char (&buf)[N], const std::string& src) noexcept {
    std::strncpy(buf, src.c_str(), N - 1);
    buf[N - 1] = '\0';
}

/// Append converter issues as indented lines to @p out.
inline void appendIssues(std::string& out, const std::vector<std::string>& issues) {
    for (const auto& issue : issues)
        out += "\n  " + issue;
}

// ============================================================================
// Shared OS-dialog state
// ============================================================================

/// Thread-safe state for folder-picker OS dialog callbacks.
struct FolderState {
    std::mutex mtx;
    std::string pending_path;
    std::atomic<bool> has_pending{false};
};

/// Consume a pending folder-dialog result into a fixed-size char buffer.
/// Returns true if a result was consumed.
template <std::size_t N>
inline bool consumeFolderResult(FolderState& state, char (&buf)[N]) {
    if (!state.has_pending.load())
        return false;
    std::lock_guard lock(state.mtx);
    copyToBuffer(buf, state.pending_path);
    state.has_pending.store(false);
    return true;
}

// ============================================================================
// D4 path utilities
// ============================================================================

/// Replace the first "meta" segment in @p path with @p seg.
/// Returns the modified path, or "" if "meta" was not found.
inline std::string replaceMetaSegment(const std::string& path, const char* seg) {
    constexpr std::string_view kMeta = "meta";
    auto pos = path.find(kMeta);
    if (pos == std::string::npos)
        return {};
    std::string result = path;
    result.replace(pos, kMeta.size(), seg);
    return result;
}

} // namespace whiteout::textool
