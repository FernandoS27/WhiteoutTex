// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief PSD file writer
 *
 * This file provides the Writer class for encoding Texture objects into Adobe
 * Photoshop PSD format.  Outputs a single-layer RGB(A) PSD file with 8-bit,
 * 16-bit, or 32-bit channel depth.
 *
 * The writer supports two modes:
 * - **Strict** – any issue throws `std::runtime_error`.
 * - **Lenient** (default) – issues are collected and can be queried via
 *   `hasIssues()` / `getIssues()`.  On failure the write methods return
 *   an empty vector (or do nothing for the file overload).
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/writer.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::psd {

// ============================================================================
// Writer
// ============================================================================

/// Encodes a Texture into PSD format.
class Writer : public textures::Writer {
public:
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(WriteMode writeMode = WriteMode::Lenient);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a PSD file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a PSD byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::psd
