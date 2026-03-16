// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "texture_converter.h"
#include "common_types.h"
#include "thread_pool_manager.h"

#include <whiteout/textures/blp/blp.h>
#include <whiteout/textures/bmp/bmp.h>
#include <whiteout/textures/dds/dds.h>
#include <whiteout/textures/jpeg/jpeg.h>
#include <whiteout/textures/png/png.h>
#include <whiteout/textures/tex/tex.h>
#include <whiteout/textures/tga/tga.h>

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>

namespace whiteout::textures {

// ============================================================================
// Helpers (file-local)
// ============================================================================

static std::string get_extension_lower(const std::string& path) {
    return gui::to_lower(std::filesystem::path(path).extension().string());
}

// ============================================================================
// Impl
// ============================================================================

class TextureConverter::Impl {
public:
    std::vector<std::string> issues;

    void clearIssues() {
        issues.clear();
    }

    // -- Generic load helpers -----------------------------------------------

    /// Load from a file path.  On failure, collects parser issues or pushes
    /// a generic error message.
    template <typename ParserT>
    std::optional<Texture> loadFile(const std::string& path, const char* fmtName) {
        ParserT parser;
        auto result = parser.parse(path);
        if (!result) {
            if (parser.hasIssues())
                issues.insert(issues.end(), parser.getIssues().begin(), parser.getIssues().end());
            else
                issues.push_back(std::string("Unknown ") + fmtName + " parse error");
        }
        return result;
    }

    /// Load from a file path, treating any parser issue as a fatal error
    /// (used by DDS and TEX where warnings invalidate the result).
    template <typename ParserT>
    std::optional<Texture> loadFileStrict(const std::string& path) {
        ParserT parser;
        auto result = parser.parse(path);
        if (parser.hasIssues()) {
            issues.insert(issues.end(), parser.getIssues().begin(), parser.getIssues().end());
            return std::nullopt;
        }
        return result;
    }

    /// Load a Diablo IV TEX.  Forwards all arguments to tex::Parser::parse().
    /// Issues are always fatal; a generic message is added on silent failure.
    template <typename... Args>
    std::optional<Texture> loadTexD4(Args&&... args) {
        tex::Parser parser;
        auto result = parser.parse(std::forward<Args>(args)...);
        if (parser.hasIssues()) {
            issues.insert(issues.end(), parser.getIssues().begin(), parser.getIssues().end());
            return std::nullopt;
        }
        if (!result)
            issues.push_back("Unknown D4 TEX parse error");
        return result;
    }

    /// Load from an in-memory buffer.
    template <typename ParserT>
    std::optional<Texture> loadFromBuffer(std::span<const u8> data, const char* fmtName) {
        ParserT parser;
        auto result = parser.parse(data);
        if (!result) {
            if (parser.hasIssues())
                issues.insert(issues.end(), parser.getIssues().begin(), parser.getIssues().end());
            else
                issues.push_back(std::string("Unknown ") + fmtName + " parse error");
        }
        return result;
    }

    // -- Generic save helper ------------------------------------------------

    /// Save via a default-constructed writer.  Forwards all arguments to
    /// writer.write().
    template <typename WriterT, typename... Args>
    bool saveFile(const std::string& path, Args&&... args) {
        WriterT writer;
        writer.write(path, std::forward<Args>(args)...);
        if (writer.hasIssues()) {
            issues.insert(issues.end(), writer.getIssues().begin(), writer.getIssues().end());
            return false;
        }
        return true;
    }

    /// BLP writer accepts an optional WorkerPool for parallel quantization.
    bool saveBlp(const Texture& tex, const std::string& path, const blp::SaveOptions& opts) {
        blp::Writer writer(blp::Writer::WriteMode::Lenient,
                           gui::threadPoolManager().get());
        writer.write(path, tex, opts);
        if (writer.hasIssues()) {
            issues.insert(issues.end(), writer.getIssues().begin(), writer.getIssues().end());
            return false;
        }
        return true;
    }

    /// JPEG writer requires quality in its constructor.
    bool saveJpeg(const Texture& tex, const std::string& path, i32 quality) {
        jpeg::Writer writer(quality);
        writer.write(path, tex);
        if (writer.hasIssues()) {
            issues.insert(issues.end(), writer.getIssues().begin(), writer.getIssues().end());
            return false;
        }
        return true;
    }
};

// ============================================================================
// Construction / destruction
// ============================================================================

TextureConverter::TextureConverter() : pImpl(std::make_unique<Impl>()) {}
TextureConverter::~TextureConverter() = default;

// ============================================================================
// Static utilities
// ============================================================================

TextureFileFormat TextureConverter::classifyPath(const std::string& path) {
    auto ext = get_extension_lower(path);
    if (ext == ".blp")
        return TextureFileFormat::BLP;
    if (ext == ".bmp")
        return TextureFileFormat::BMP;
    if (ext == ".dds")
        return TextureFileFormat::DDS;
    if (ext == ".jpg" || ext == ".jpeg")
        return TextureFileFormat::JPEG;
    if (ext == ".png")
        return TextureFileFormat::PNG;
    if (ext == ".tex")
        return TextureFileFormat::TEX;
    if (ext == ".tga")
        return TextureFileFormat::TGA;
    return TextureFileFormat::Unknown;
}

bool TextureConverter::isD4Tex(const std::string& path) {
    tex::Parser parser;
    return parser.detectKind(path) == tex::Parser::FileKind::Diablo4MetaTex;
}

TextureKind TextureConverter::guessTextureKind(const std::string& path, PixelFormat fmt) {
    auto stem = gui::to_lower(std::filesystem::path(path).stem().string());

    if (stem.find("_diff") != std::string::npos || stem.find("diffuse") != std::string::npos)
        return TextureKind::Diffuse;
    if (stem.find("_orm") != std::string::npos)
        return TextureKind::ORM;
    if (stem.find("_ao") != std::string::npos || stem.find("occlusion") != std::string::npos)
        return TextureKind::AmbientOcclusion;
    if (stem.find("_roughness") != std::string::npos || stem.find("_rough") != std::string::npos)
        return TextureKind::Roughness;
    if (stem.find("_metal") != std::string::npos)
        return TextureKind::Metalness;
    if (stem.find("_gloss") != std::string::npos || stem.find("_smooth") != std::string::npos)
        return TextureKind::Gloss;
    if (stem.find("_spec") != std::string::npos || stem.find("specular") != std::string::npos)
        return TextureKind::Specular;
    if (stem.find("_emis") != std::string::npos || stem.find("emissive") != std::string::npos)
        return TextureKind::Emissive;
    if (stem.find("_alpha") != std::string::npos || stem.find("_mask") != std::string::npos ||
        stem.find("_opacity") != std::string::npos)
        return TextureKind::AlphaMask;
    if (stem.find("lightmap") != std::string::npos)
        return TextureKind::Lightmap;
    if (stem.find("_envpbr") != std::string::npos || stem.find("_ibl") != std::string::npos)
        return TextureKind::EnvironmentPBR;
    if (stem.find("_envlegacy") != std::string::npos || stem.find("_envmap") != std::string::npos ||
        stem.find("_env") != std::string::npos || stem.find("_reflection") != std::string::npos)
        return TextureKind::EnvironmentLegacy;
    if (stem.find("_albedo") != std::string::npos || stem.find("_tint") != std::string::npos ||
        stem.find("_basecolor") != std::string::npos)
        return TextureKind::Albedo;
    if (stem.find("normal") != std::string::npos || stem.find("_nrm") != std::string::npos ||
        stem.find("_norm") != std::string::npos)
        return TextureKind::Normal;

    // Heuristic: BC5 / dual-channel → normal map.
    if (fmt == PixelFormat::BC5 || fmt == PixelFormat::RG8 || fmt == PixelFormat::RG16 ||
        fmt == PixelFormat::RG32F)
        return TextureKind::Normal;

    // Single-channel → roughness.
    if (fmt == PixelFormat::BC4 || fmt == PixelFormat::R8 || fmt == PixelFormat::R16 ||
        fmt == PixelFormat::R32F)
        return TextureKind::Roughness;

    return TextureKind::Diffuse;
}

const char* TextureConverter::fileFormatName(TextureFileFormat fmt) {
    switch (fmt) {
    case TextureFileFormat::BLP:
        return "BLP";
    case TextureFileFormat::BMP:
        return "BMP";
    case TextureFileFormat::DDS:
        return "DDS";
    case TextureFileFormat::JPEG:
        return "JPEG";
    case TextureFileFormat::PNG:
        return "PNG";
    case TextureFileFormat::TEX:
        return "TEX";
    case TextureFileFormat::TGA:
        return "TGA";
    default:
        return "Unknown";
    }
}

const char* TextureConverter::pixelFormatName(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::R8:
        return "R8";
    case PixelFormat::R16:
        return "R16";
    case PixelFormat::R32F:
        return "R32F";
    case PixelFormat::RG8:
        return "RG8";
    case PixelFormat::RG16:
        return "RG16";
    case PixelFormat::RG32F:
        return "RG32F";
    case PixelFormat::RGBA8:
        return "RGBA8";
    case PixelFormat::RGBA16:
        return "RGBA16";
    case PixelFormat::RGBA32F:
        return "RGBA32F";
    case PixelFormat::BC1:
        return "BC1 (DXT1)";
    case PixelFormat::BC2:
        return "BC2 (DXT3)";
    case PixelFormat::BC3:
        return "BC3 (DXT5)";
    case PixelFormat::BC4:
        return "BC4 (ATI1)";
    case PixelFormat::BC5:
        return "BC5 (ATI2)";
    case PixelFormat::BC6H:
        return "BC6H";
    case PixelFormat::BC7:
        return "BC7";
    }
    return "Unknown";
}

const char* TextureConverter::textureTypeName(TextureType type) {
    switch (type) {
    case TextureType::Texture2D:
        return "2D";
    case TextureType::Texture3D:
        return "3D";
    case TextureType::TextureCube:
        return "Cube";
    }
    return "Unknown";
}

const char* TextureConverter::textureKindName(TextureKind kind) {
    switch (kind) {
    case TextureKind::Other:
        return "Other";
    case TextureKind::Diffuse:
        return "Diffuse";
    case TextureKind::Normal:
        return "Normal";
    case TextureKind::Specular:
        return "Specular";
    case TextureKind::ORM:
        return "ORM";
    case TextureKind::Albedo:
        return "Albedo";
    case TextureKind::Roughness:
        return "Roughness";
    case TextureKind::Metalness:
        return "Metalness";
    case TextureKind::AmbientOcclusion:
        return "AmbientOcclusion";
    case TextureKind::Gloss:
        return "Gloss";
    case TextureKind::Emissive:
        return "Emissive";
    case TextureKind::AlphaMask:
        return "AlphaMask";
    case TextureKind::Lightmap:
        return "Lightmap";
    case TextureKind::EnvironmentPBR:
        return "EnvironmentPBR";
    case TextureKind::EnvironmentLegacy:
        return "EnvironmentLegacy";
    }
    return "Unknown";
}

// ============================================================================
// Loading
// ============================================================================

std::optional<Texture> TextureConverter::load(const std::string& path) {
    return load(path, classifyPath(path));
}

std::optional<Texture> TextureConverter::load(const std::string& path, TextureFileFormat fmt) {
    pImpl->clearIssues();
    switch (fmt) {
    case TextureFileFormat::BLP:  return pImpl->loadFile<blp::Parser>(path, "BLP");
    case TextureFileFormat::BMP:  return pImpl->loadFile<bmp::Parser>(path, "BMP");
    case TextureFileFormat::DDS:  return pImpl->loadFileStrict<dds::Parser>(path);
    case TextureFileFormat::JPEG: return pImpl->loadFile<jpeg::Parser>(path, "JPEG");
    case TextureFileFormat::PNG:  return pImpl->loadFile<png::Parser>(path, "PNG");
    case TextureFileFormat::TEX:  return pImpl->loadFileStrict<tex::Parser>(path);
    case TextureFileFormat::TGA:  return pImpl->loadFile<tga::Parser>(path, "TGA");
    default:
        pImpl->issues.push_back("Unsupported input format");
        return std::nullopt;
    }
}

std::optional<Texture> TextureConverter::loadTexD4(const std::string& metaPath,
                                                    const std::string& payloadPath) {
    pImpl->clearIssues();
    return pImpl->loadTexD4(metaPath, payloadPath);
}

std::optional<Texture> TextureConverter::loadTexD4(const std::string& metaPath,
                                                    const std::string& payloadPath,
                                                    const std::string& paylowPath) {
    pImpl->clearIssues();
    return pImpl->loadTexD4(metaPath, payloadPath, paylowPath);
}

std::optional<Texture> TextureConverter::load(std::span<const u8> data, TextureFileFormat fmt) {
    pImpl->clearIssues();
    switch (fmt) {
    case TextureFileFormat::BLP:
        return pImpl->loadFromBuffer<blp::Parser>(data, "BLP");
    case TextureFileFormat::BMP:
        return pImpl->loadFromBuffer<bmp::Parser>(data, "BMP");
    case TextureFileFormat::DDS:
        return pImpl->loadFromBuffer<dds::Parser>(data, "DDS");
    case TextureFileFormat::JPEG:
        return pImpl->loadFromBuffer<jpeg::Parser>(data, "JPEG");
    case TextureFileFormat::PNG:
        return pImpl->loadFromBuffer<png::Parser>(data, "PNG");
    case TextureFileFormat::TEX:
        return pImpl->loadFromBuffer<tex::Parser>(data, "TEX");
    case TextureFileFormat::TGA:
        return pImpl->loadFromBuffer<tga::Parser>(data, "TGA");
    default:
        pImpl->issues.push_back("Unsupported input format");
        return std::nullopt;
    }
}

std::optional<Texture> TextureConverter::loadTexD4(std::span<const u8> meta,
                                                    std::span<const u8> payload) {
    pImpl->clearIssues();
    return pImpl->loadTexD4(meta, payload);
}

std::optional<Texture> TextureConverter::loadTexD4(std::span<const u8> meta,
                                                    std::span<const u8> payload,
                                                    std::span<const u8> paylow) {
    pImpl->clearIssues();
    if (paylow.empty())
        return pImpl->loadTexD4(meta, payload);
    return pImpl->loadTexD4(meta, payload, paylow, nullptr);
}

// ============================================================================
// Saving
// ============================================================================

bool TextureConverter::save(const Texture& tex, const std::string& path) {
    return save(tex, path, blp::SaveOptions{});
}

bool TextureConverter::save(const Texture& tex, const std::string& path,
                            const blp::SaveOptions& blpOpts) {
    pImpl->clearIssues();
    auto fmt = classifyPath(path);
    switch (fmt) {
    case TextureFileFormat::BLP:  return pImpl->saveBlp(tex, path, blpOpts);
    case TextureFileFormat::BMP:  return pImpl->saveFile<bmp::Writer>(path, tex);
    case TextureFileFormat::DDS:  return pImpl->saveFile<dds::Writer>(path, tex);
    case TextureFileFormat::JPEG: return pImpl->saveJpeg(tex, path, kDefaultJpegQuality);
    case TextureFileFormat::PNG:  return pImpl->saveFile<png::Writer>(path, tex);
    case TextureFileFormat::TEX:  return pImpl->saveFile<tex::Writer>(path, tex);
    case TextureFileFormat::TGA:  return pImpl->saveFile<tga::Writer>(path, tex);
    default:
        pImpl->issues.push_back("Unsupported output format");
        return false;
    }
}

bool TextureConverter::save(const Texture& tex, const std::string& path, i32 jpegQuality) {
    pImpl->clearIssues();
    auto fmt = classifyPath(path);
    if (fmt != TextureFileFormat::JPEG) {
        pImpl->issues.push_back("JPEG quality option only applies to JPEG output");
        return false;
    }
    return pImpl->saveJpeg(tex, path, jpegQuality);
}

// ============================================================================
// Issue reporting
// ============================================================================

bool TextureConverter::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& TextureConverter::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures
