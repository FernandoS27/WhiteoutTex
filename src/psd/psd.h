// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file psd.h
 * @brief Main header for the PSD (Adobe Photoshop) texture support
 *
 * This is the primary include file for PSD support. Include this single
 * header to access PSD functionality including parsing and writing.
 *
 * Supports 8-bit, 16-bit, and 32-bit RGB/RGBA PSD images via the
 * MolecularMatters psd_sdk.
 *
 * @example Basic Usage
 * @code
 * #include "psd/psd.h"
 *
 * // Parse PSD bytes into a texture
 * psd::Parser parser;
 * auto texture = parser.parse(file_bytes);
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cerr << "Warning: " << issue << std::endl;
 *     }
 * }
 *
 * // Encode texture back into PSD
 * psd::Writer writer;
 * writer.write("output.psd", *texture);
 * @endcode
 */

#include "parser.h"
#include "writer.h"

namespace whiteout {
namespace psd = textures::psd;

namespace textures {
namespace psd {
// ============================================================================
// PSD Library Version
// ============================================================================

/// Library major version number
constexpr int MAJOR_VERSION = 1;
/// Library minor version number
constexpr int MINOR_VERSION = 0;
/// Library patch version number
constexpr int PATCH_VERSION = 0;
} // namespace psd
} // namespace textures
} // namespace whiteout
