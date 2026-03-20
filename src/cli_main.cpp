// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WhiteoutTex CLI — command-line texture converter with AI upscaling/downscaling.
///
/// Usage:
///   WhiteoutTexCLI <input> [output] [options...]
///
/// Positional arguments:
///   input       Path to the input texture file.
///   output      Path to the output texture file.  If omitted the input
///               extension is replaced automatically (e.g. .blp → .dds).
///
/// Conversion options:
///   --blp_type=<1|2>
///       BLP container version.  1 = Warcraft III era, 2 = TBC+ era (default).
///
///   --blp_compression=<true_color|paletted|jpeg|bc1|bc2|bc3>
///       Internal encoding when saving as BLP.
///
///   --dds_format=<true_color|bc1|bc2|bc3|bc4|bc5|bc6|bc7|bc3n>
///       Pixel format when saving as DDS.
///
///   --jpeg_quality=<1..100>
///       JPEG encoder quality level (default 75).
///
///   --jpeg_progressive
///       Use progressive (SOF2) JPEG encoding.
///
///   --generate_mipmaps[=<maximum|custom>]
///       Regenerate mip levels.
///         maximum (default) — full mip chain.
///         custom  — use --mipmap_count to limit levels.
///
///   --mipmap_count=<N>
///       Number of mip levels when --generate_mipmaps=custom.
///
///   --texture_kind=<diffuse|normal|specular|orm|albedo|roughness|
///                   metalness|ao|gloss|emissive|alpha_mask|binary_mask|
///                   transparency_mask|blend_mask|lightmap|env_pbr|
///                   env_legacy|multikind|other>
///       Semantic kind of the texture (affects mipmap filters and
///       DDS format selection).
///
///   --dds_invert_y
///       Invert the green (Y) channel when saving DDS (normal maps).
///
///   --blp_dither[=<strength>]
///       Enable ordered dithering for paletted BLP.
///       Optional strength 0.0–1.0 (default 0.8).
///
/// Transform options (applied in order before save):
///   --downscale=<x2|x4>
///       Downscale the texture by halving once (x2) or twice (x4).
///
///   --upscale=<model_name>
///       AI upscale using the named Real-ESRGAN model.
///       Use --list_models to see available models.
///
///   --upscale_alpha
///       Upscale the alpha channel through the AI model instead of
///       bicubic interpolation (slower, higher quality).
///
///   --model_dir=<path>
///       Directory containing .param / .bin model files.
///       Defaults to a 'models' folder next to the executable.
///
///   --gpu=<id>
///       Vulkan GPU device index for AI upscaling (-1 = auto).
///
///   --tile_size=<N>
///       Tile size for AI inference (0 = auto based on VRAM).
///
/// Informational:
///   --list_models
///       List available AI upscaler models and exit.
///
///   --help
///       Show this help message and exit.

#include "common_types.h"
#include "preferences.h"
#include "texture_converter.h"
#include "thread_pool_manager.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace tex = whiteout::textures;
namespace tool = whiteout::textool;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;
using tool::MipmapMode;

// ============================================================================
// Helpers
// ============================================================================

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string replace_extension(const std::string& path, const std::string& new_ext) {
    return std::filesystem::path(path).replace_extension(new_ext).string();
}

static std::string default_output_extension(TFF input_fmt) {
    switch (input_fmt) {
    case TFF::BLP:
        return ".dds";
    case TFF::BMP:
        return ".dds";
    case TFF::DDS:
        return ".blp";
    case TFF::JPEG:
        return ".png";
    case TFF::PNG:
        return ".jpg";
    case TFF::TEX:
        return ".dds";
    case TFF::TGA:
        return ".dds";
    default:
        return ".dds";
    }
}

static std::string get_option_value(const char* arg, const char* prefix) {
    const auto prefix_len = std::strlen(prefix);
    if (std::strncmp(arg, prefix, prefix_len) == 0)
        return std::string(arg + prefix_len);
    return {};
}

static std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[32768]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, 32768);
    if (len > 0 && len < 32768) {
        return std::filesystem::path(buf).parent_path();
    }
#elif defined(__linux__)
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

// ============================================================================
// BLP / DDS format mapping (mirrors save_helpers without ImGui dependency)
// ============================================================================

static constexpr tex::PixelFormat kDdsPixelFormats[] = {
    tex::PixelFormat::RGBA8, // 0: true_color
    tex::PixelFormat::BC1,   // 1: bc1
    tex::PixelFormat::BC2,   // 2: bc2
    tex::PixelFormat::BC3,   // 3: bc3
    tex::PixelFormat::BC4,   // 4: bc4
    tex::PixelFormat::BC5,   // 5: bc5
    tex::PixelFormat::BC6H,  // 6: bc6
    tex::PixelFormat::BC7,   // 7: bc7
};

static tex::blp::SaveOptions build_blp_options(int blp_version, int blp_encoding_idx, bool dither,
                                               float dither_strength, int jpeg_quality,
                                               bool jpeg_progressive) {
    using namespace tex::blp;
    SaveOptions opts;
    opts.version = blp_version == 0 ? BlpVersion::BLP1 : BlpVersion::BLP2;

    // Encoding indices: 0=Infer, 1=BGRA, 2=Palettized, 3=JPEG, 4=BC1, 5=BC2, 6=BC3
    switch (blp_encoding_idx) {
    case 0:
        opts.encoding = BlpEncoding::Infer;
        break;
    case 1:
        opts.encoding = BlpEncoding::BGRA;
        break;
    case 2:
        opts.encoding = BlpEncoding::Palettized;
        break;
    case 3:
        opts.encoding = BlpEncoding::JPEG;
        break;
    default:
        opts.encoding = BlpEncoding::DXT;
        break;
    }

    opts.dither = dither;
    opts.ditherStrength = dither_strength;
    opts.jpegQuality = jpeg_quality;
    opts.jpegProgressive = jpeg_progressive;

    // Force BLP1 for encodings that require it.
    if (opts.encoding == BlpEncoding::JPEG || opts.encoding == BlpEncoding::Palettized)
        opts.version = BlpVersion::BLP1;

    return opts;
}

static void coerce_blp_format(tex::Texture& t, int blp_encoding_idx, tex::blp::BlpEncoding enc,
                              whiteout::interfaces::WorkerPool* pool) {
    if (enc == tex::blp::BlpEncoding::JPEG || enc == tex::blp::BlpEncoding::Palettized ||
        enc == tex::blp::BlpEncoding::BGRA || enc == tex::blp::BlpEncoding::Infer) {
        if (t.format() != tex::PixelFormat::RGBA8)
            t = t.copyAsFormat(tex::PixelFormat::RGBA8, pool);
    } else {
        // DXT subtype: 4→BC1, 5→BC2, 6→BC3
        constexpr tex::PixelFormat kDxt[] = {tex::PixelFormat::BC1, tex::PixelFormat::BC2,
                                             tex::PixelFormat::BC3};
        auto fmt = kDxt[blp_encoding_idx - 4];
        if (t.format() != fmt)
            t = t.copyAsFormat(fmt, pool);
    }
}

static void coerce_dds_format(tex::Texture& t, int dds_idx, bool bc3n, bool invert_y,
                              whiteout::interfaces::WorkerPool* pool) {
    const auto target = bc3n ? tex::PixelFormat::BC3 : kDdsPixelFormats[dds_idx];

    if (bc3n) {
        if (t.format() != tex::PixelFormat::RGBA8)
            t = t.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        if (invert_y)
            t.invertChannel(tex::Channel::G);
        t.swapChannels(tex::Channel::R, tex::Channel::A);
    } else if (invert_y) {
        if (tex::isBcn(t.format()))
            t = t.copyAsFormat(tex::PixelFormat::RGBA8, pool);
        t.invertChannel(tex::Channel::G);
    }
    if (t.format() != target)
        t = t.copyAsFormat(target, pool);
}

// ============================================================================
// Texture kind parsing
// ============================================================================

struct KindMapping {
    const char* name;
    tex::TextureKind kind;
};

static constexpr KindMapping kKindMappings[] = {
    {"diffuse", tex::TextureKind::Diffuse},
    {"normal", tex::TextureKind::Normal},
    {"specular", tex::TextureKind::Specular},
    {"orm", tex::TextureKind::ORM},
    {"albedo", tex::TextureKind::Albedo},
    {"roughness", tex::TextureKind::Roughness},
    {"metalness", tex::TextureKind::Metalness},
    {"ao", tex::TextureKind::AmbientOcclusion},
    {"gloss", tex::TextureKind::Gloss},
    {"emissive", tex::TextureKind::Emissive},
    {"alpha_mask", tex::TextureKind::AlphaMask},
    {"binary_mask", tex::TextureKind::BinaryMask},
    {"transparency_mask", tex::TextureKind::TransparencyMask},
    {"blend_mask", tex::TextureKind::BlendMask},
    {"lightmap", tex::TextureKind::Lightmap},
    {"env_pbr", tex::TextureKind::EnvironmentPBR},
    {"env_legacy", tex::TextureKind::EnvironmentLegacy},
    {"multikind", tex::TextureKind::Multikind},
    {"other", tex::TextureKind::Other},
};

// ============================================================================
// Usage
// ============================================================================

static void print_usage(const char* program) {
    std::cout << "WhiteoutTex CLI — texture converter with AI upscaling & downscaling\n\n"
              << "Usage: " << program << " <input> [output] [options...]\n\n"
              << "If no output path is given, the extension is replaced automatically:\n"
              << "  .blp -> .dds    .bmp -> .dds    .tex -> .dds\n"
              << "  .dds -> .blp    .tga -> .dds\n"
              << "  .jpg -> .png    .png -> .jpg\n\n"
              << "Conversion options:\n"
              << "  --blp_type=<1|2>           BLP container version (default: 2)\n"
              << "  --blp_compression=<type>   BLP encoding: true_color|paletted|jpeg|bc1|bc2|bc3\n"
              << "  --dds_format=<type>        DDS format: true_color|bc1-bc7|bc3n\n"
              << "  --jpeg_quality=<1..100>    JPEG quality level (default: 75)\n"
              << "  --jpeg_progressive         Use progressive JPEG encoding\n"
              << "  --generate_mipmaps[=mode]  Regenerate mipmaps: maximum (default) or custom\n"
              << "  --mipmap_count=<N>         Mip count for --generate_mipmaps=custom\n"
              << "  --texture_kind=<kind>      Set texture kind (see below)\n"
              << "  --dds_invert_y             Invert DDS green channel (normal maps)\n"
              << "  --blp_dither[=<strength>]  Dither for paletted BLP (strength 0.0-1.0)\n\n"
              << "Transform options (applied in order before save):\n"
              << "  --downscale=<x2|x4>        Downscale by 2x or 4x\n"
#ifdef WHITEOUT_HAS_UPSCALER
              << "  --upscale=<model_name>     AI upscale with named Real-ESRGAN model\n"
              << "  --upscale_alpha            Upscale alpha via AI model\n"
              << "  --model_dir=<path>         Model directory (default: models/ next to exe)\n"
              << "  --gpu=<id>                 Vulkan GPU index (-1 = auto)\n"
              << "  --tile_size=<N>            Inference tile size (0 = auto)\n"
              << "  --list_models              List available AI models and exit\n"
#endif
              << "\n"
              << "Texture kinds:\n"
              << "  diffuse, normal, specular, orm, albedo, roughness, metalness, ao,\n"
              << "  gloss, emissive, alpha_mask, binary_mask, transparency_mask,\n"
              << "  blend_mask, lightmap, env_pbr, env_legacy, multikind, other\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // -- Parse arguments ---------------------------------------------------
    std::vector<std::string> positional;
    std::string opt_blp_type;
    std::string opt_blp_compression;
    std::string opt_dds_format;
    std::string opt_jpeg_quality;
    bool opt_jpeg_progressive = false;
    std::string opt_texture_kind;
    std::string opt_generate_mipmaps; // "" | "maximum" | "custom"
    std::string opt_mipmap_count;
    std::string opt_downscale;
    bool opt_dds_invert_y = false;
    bool opt_blp_dither = false;
    float opt_blp_dither_strength = 0.8f;

    // Upscaler options
    std::string opt_upscale_model;
    bool opt_upscale_alpha = false;
    std::string opt_model_dir;
    std::string opt_gpu;
    std::string opt_tile_size;
    bool opt_list_models = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        std::string v;
        if ((v = get_option_value(arg, "--blp_type=")).size()) {
            opt_blp_type = to_lower(v);
        } else if ((v = get_option_value(arg, "--blp_compression=")).size()) {
            opt_blp_compression = to_lower(v);
        } else if ((v = get_option_value(arg, "--dds_format=")).size()) {
            opt_dds_format = to_lower(v);
        } else if ((v = get_option_value(arg, "--jpeg_quality=")).size()) {
            opt_jpeg_quality = v;
        } else if (std::strcmp(arg, "--jpeg_progressive") == 0) {
            opt_jpeg_progressive = true;
        } else if ((v = get_option_value(arg, "--texture_kind=")).size()) {
            opt_texture_kind = to_lower(v);
        } else if ((v = get_option_value(arg, "--generate_mipmaps=")).size()) {
            opt_generate_mipmaps = to_lower(v);
        } else if (std::strcmp(arg, "--generate_mipmaps") == 0) {
            opt_generate_mipmaps = "maximum";
        } else if ((v = get_option_value(arg, "--mipmap_count=")).size()) {
            opt_mipmap_count = v;
        } else if ((v = get_option_value(arg, "--downscale=")).size()) {
            opt_downscale = to_lower(v);
        } else if (std::strcmp(arg, "--dds_invert_y") == 0) {
            opt_dds_invert_y = true;
        } else if ((v = get_option_value(arg, "--blp_dither=")).size()) {
            opt_blp_dither = true;
            opt_blp_dither_strength = std::clamp(std::stof(v), 0.0f, 1.0f);
        } else if (std::strcmp(arg, "--blp_dither") == 0) {
            opt_blp_dither = true;
        } else if ((v = get_option_value(arg, "--upscale=")).size()) {
            opt_upscale_model = v;
        } else if (std::strcmp(arg, "--upscale_alpha") == 0) {
            opt_upscale_alpha = true;
        } else if ((v = get_option_value(arg, "--model_dir=")).size()) {
            opt_model_dir = v;
        } else if ((v = get_option_value(arg, "--gpu=")).size()) {
            opt_gpu = v;
        } else if ((v = get_option_value(arg, "--tile_size=")).size()) {
            opt_tile_size = v;
        } else if (std::strcmp(arg, "--list_models") == 0) {
            opt_list_models = true;
        } else if (std::strncmp(arg, "--", 2) == 0) {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        } else {
            positional.push_back(arg);
        }
    }

    // -- Determine model directory -----------------------------------------
    std::filesystem::path model_dir;
    if (!opt_model_dir.empty()) {
        model_dir = opt_model_dir;
    } else {
        model_dir = executable_dir() / "models";
    }

    // -- List models -------------------------------------------------------
#ifdef WHITEOUT_HAS_UPSCALER
    if (opt_list_models) {
        if (!tool::Upscaler::isGpuAvailable()) {
            std::cerr << "No Vulkan-capable GPU detected.\n";
            return 1;
        }
        auto models = tool::Upscaler::availableModels(model_dir);
        if (models.empty()) {
            std::cerr << "No models found in " << model_dir.string() << "\n";
            return 1;
        }
        std::cout << "Available AI upscaler models (in " << model_dir.string() << "):\n";
        for (const auto& m : models) {
            std::cout << "  " << m.file_stem << "  — " << m.display_name << " (" << m.scale
                      << "x)\n";
        }
        return 0;
    }
#else
    if (opt_list_models) {
        std::cerr << "AI upscaling is not enabled in this build.\n";
        return 1;
    }
#endif

    // -- Validate positional args ------------------------------------------
    if (positional.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string input_path = positional[0];
    TFF input_fmt = TC::classifyPath(input_path);
    if (input_fmt == TFF::Unknown) {
        std::cerr << "Unrecognised input extension: "
                  << std::filesystem::path(input_path).extension().string() << "\n";
        return 1;
    }

    std::string output_path;
    if (positional.size() >= 2) {
        output_path = positional[1];
    } else {
        output_path = replace_extension(input_path, default_output_extension(input_fmt));
    }

    TFF output_fmt = TC::classifyPath(output_path);
    if (output_fmt == TFF::Unknown) {
        std::cerr << "Unrecognised output extension: "
                  << std::filesystem::path(output_path).extension().string() << "\n";
        return 1;
    }

    // -- Create output directory if needed ---------------------------------
    {
        auto parent = std::filesystem::path(output_path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
    }

    // -- Resolve BLP options -----------------------------------------------
    int blp_version_idx = 1;  // default BLP2
    int blp_encoding_idx = 0; // default Infer

    if (!opt_blp_type.empty()) {
        if (opt_blp_type == "1")
            blp_version_idx = 0;
        else if (opt_blp_type == "2")
            blp_version_idx = 1;
        else {
            std::cerr << "Invalid --blp_type '" << opt_blp_type << "'. Expected 1 or 2.\n";
            return 1;
        }
    }

    if (!opt_blp_compression.empty()) {
        if (opt_blp_compression == "true_color")
            blp_encoding_idx = 1;
        else if (opt_blp_compression == "paletted")
            blp_encoding_idx = 2;
        else if (opt_blp_compression == "jpeg")
            blp_encoding_idx = 3;
        else if (opt_blp_compression == "bc1")
            blp_encoding_idx = 4;
        else if (opt_blp_compression == "bc2")
            blp_encoding_idx = 5;
        else if (opt_blp_compression == "bc3")
            blp_encoding_idx = 6;
        else {
            std::cerr << "Invalid --blp_compression '" << opt_blp_compression
                      << "'. Expected true_color, paletted, jpeg, bc1, bc2, or bc3.\n";
            return 1;
        }
    }

    // -- Resolve DDS options -----------------------------------------------
    int dds_format_idx = -1;
    bool dds_bc3n = false;

    if (!opt_dds_format.empty()) {
        if (opt_dds_format == "true_color")
            dds_format_idx = 0;
        else if (opt_dds_format == "bc1")
            dds_format_idx = 1;
        else if (opt_dds_format == "bc2")
            dds_format_idx = 2;
        else if (opt_dds_format == "bc3")
            dds_format_idx = 3;
        else if (opt_dds_format == "bc4")
            dds_format_idx = 4;
        else if (opt_dds_format == "bc5")
            dds_format_idx = 5;
        else if (opt_dds_format == "bc6")
            dds_format_idx = 6;
        else if (opt_dds_format == "bc7")
            dds_format_idx = 7;
        else if (opt_dds_format == "bc3n") {
            dds_format_idx = 3;
            dds_bc3n = true;
        } else {
            std::cerr << "Invalid --dds_format '" << opt_dds_format
                      << "'. Expected true_color, bc1-bc7, or bc3n.\n";
            return 1;
        }
    }

    // -- Resolve JPEG quality ----------------------------------------------
    int jpeg_quality = tex::kDefaultJpegQuality;
    if (!opt_jpeg_quality.empty()) {
        int q = std::atoi(opt_jpeg_quality.c_str());
        if (q < 1 || q > 100) {
            std::cerr << "Invalid --jpeg_quality '" << opt_jpeg_quality << "'. Expected 1..100.\n";
            return 1;
        }
        jpeg_quality = q;
    }

    // If --jpeg_quality was given for BLP output without explicit compression,
    // auto-select JPEG encoding.
    if (output_fmt == TFF::BLP && !opt_jpeg_quality.empty() && opt_blp_compression.empty()) {
        blp_encoding_idx = 3; // JPEG
    }

    // Auto dither for paletted.
    if (output_fmt == TFF::BLP && blp_encoding_idx == 2 && !opt_blp_dither) {
        opt_blp_dither = true;
    }

    // -- Resolve texture kind ----------------------------------------------
    std::optional<tex::TextureKind> explicit_kind;
    if (!opt_texture_kind.empty()) {
        bool found = false;
        for (const auto& m : kKindMappings) {
            if (opt_texture_kind == m.name) {
                explicit_kind = m.kind;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Invalid --texture_kind '" << opt_texture_kind << "'.\n"
                      << "Valid kinds: ";
            for (const auto& m : kKindMappings)
                std::cerr << m.name << " ";
            std::cerr << "\n";
            return 1;
        }
    }

    // -- Resolve downscale -------------------------------------------------
    int downscale_levels = 0;
    if (!opt_downscale.empty()) {
        if (opt_downscale == "x2")
            downscale_levels = 1;
        else if (opt_downscale == "x4")
            downscale_levels = 2;
        else {
            std::cerr << "Invalid --downscale '" << opt_downscale << "'. Expected x2 or x4.\n";
            return 1;
        }
    }

    // -- Resolve mipmap mode -----------------------------------------------
    MipmapMode mipmap_mode = MipmapMode::KeepOriginal;
    int mipmap_custom_count = 1;
    if (!opt_generate_mipmaps.empty()) {
        if (opt_generate_mipmaps == "maximum") {
            mipmap_mode = MipmapMode::Maximum;
        } else if (opt_generate_mipmaps == "custom") {
            mipmap_mode = MipmapMode::Custom;
            if (!opt_mipmap_count.empty()) {
                mipmap_custom_count = std::atoi(opt_mipmap_count.c_str());
                if (mipmap_custom_count < 1)
                    mipmap_custom_count = 1;
            }
        } else {
            std::cerr << "Invalid --generate_mipmaps mode '" << opt_generate_mipmaps
                      << "'. Expected maximum or custom.\n";
            return 1;
        }
    }

    auto* pool = tool::threadPoolManager().get();

    // -- Load ---------------------------------------------------------------
    std::cout << "Converting " << TC::fileFormatName(input_fmt) << " -> "
              << TC::fileFormatName(output_fmt) << "\n";

    TC converter;
    std::optional<tex::Texture> texture;

    // Handle D4 TEX files with payload/paylow fallback.
    if (input_fmt == TFF::TEX && TC::isD4Tex(input_path)) {
        auto payload = tool::replaceMetaSegment(input_path, "payload");
        auto paylow = tool::replaceMetaSegment(input_path, "paylow");
        bool has_payload = !payload.empty() && std::filesystem::exists(payload);
        bool has_paylow = !paylow.empty() && std::filesystem::exists(paylow);

        if (has_payload && has_paylow)
            texture = converter.loadTexD4(input_path, payload, paylow);
        else if (has_payload)
            texture = converter.loadTexD4(input_path, payload);
        else if (has_paylow)
            texture = converter.loadTexD4(input_path, paylow);
    }

    if (!texture) {
        texture = converter.load(input_path, input_fmt);
    }

    if (!texture) {
        std::cerr << "Failed to load " << input_path;
        if (converter.hasIssues())
            std::cerr << ": " << converter.getIssues().front();
        std::cerr << "\n";
        return 1;
    }

    std::cout << "  Input:      " << input_path << "\n";
    std::cout << "  Type:       " << TC::textureTypeName(texture->type()) << "\n";
    std::cout << "  Format:     " << TC::pixelFormatName(texture->format()) << "\n";
    std::cout << "  Dimensions: " << texture->width() << "x" << texture->height() << "\n";
    std::cout << "  Mip levels: " << texture->mipCount() << "\n";

    // -- Set texture kind ---------------------------------------------------
    if (explicit_kind) {
        texture->setKind(*explicit_kind);
        if (*explicit_kind == tex::TextureKind::Multikind) {
            auto ch = TC::guessTextureMultiKind(input_path, texture->format());
            for (int i = 0; i < 4; ++i) {
                constexpr tex::Channel kChannels[] = {tex::Channel::R, tex::Channel::G,
                                                      tex::Channel::B, tex::Channel::A};
                texture->setChannelKind(kChannels[i], ch[i]);
            }
        }
        std::cout << "  Kind:       " << TC::textureKindName(*explicit_kind) << " (explicit)\n";
    } else {
        auto kind = TC::guessTextureKind(input_path, texture->format());
        texture->setKind(kind);
        if (kind == tex::TextureKind::Multikind) {
            auto ch = TC::guessTextureMultiKind(input_path, texture->format());
            constexpr tex::Channel kChannels[] = {tex::Channel::R, tex::Channel::G, tex::Channel::B,
                                                  tex::Channel::A};
            for (int i = 0; i < 4; ++i)
                texture->setChannelKind(kChannels[i], ch[i]);
        }
        std::cout << "  Kind:       " << TC::textureKindName(kind) << " (guessed)\n";
    }

    // -- Apply transforms (downscale / upscale) in order --------------------

    // Downscale: drop mip levels by halving.
    if (downscale_levels > 0) {
        // Decompress BCn for manipulation.
        if (tex::isBcn(texture->format())) {
            std::cout << "  Decompressing for downscale...\n";
            *texture = texture->copyAsFormat(tex::PixelFormat::RGBA8, pool);
        }
        auto err = texture->downscale(static_cast<whiteout::u32>(downscale_levels), pool);
        if (err) {
            std::cerr << "Downscale failed: " << *err << "\n";
            return 1;
        }
        std::cout << "  Downscaled:  " << texture->width() << "x" << texture->height() << "\n";
    }

    // AI Upscale.
    if (!opt_upscale_model.empty()) {
#ifdef WHITEOUT_HAS_UPSCALER
        if (!tool::Upscaler::isGpuAvailable()) {
            std::cerr << "No Vulkan-capable GPU detected for AI upscaling.\n";
            return 1;
        }

        auto models = tool::Upscaler::availableModels(model_dir);
        if (models.empty()) {
            std::cerr << "No models found in " << model_dir.string() << "\n";
            return 1;
        }

        // Find model by file_stem (case-insensitive).
        const tool::UpscalerModel* chosen = nullptr;
        std::string model_lower = to_lower(opt_upscale_model);
        for (const auto& m : models) {
            if (to_lower(m.file_stem) == model_lower) {
                chosen = &m;
                break;
            }
        }
        if (!chosen) {
            std::cerr << "Model '" << opt_upscale_model << "' not found.\nAvailable:\n";
            for (const auto& m : models)
                std::cerr << "  " << m.file_stem << "\n";
            return 1;
        }

        int gpu_id = -1;
        if (!opt_gpu.empty())
            gpu_id = std::atoi(opt_gpu.c_str());

        int tile_size = 0;
        if (!opt_tile_size.empty())
            tile_size = std::atoi(opt_tile_size.c_str());

        tool::Upscaler upscaler;
        std::cout << "  Initializing AI upscaler (" << chosen->display_name << " " << chosen->scale
                  << "x)...\n";

        if (!upscaler.init(model_dir, *chosen, gpu_id, tile_size)) {
            std::cerr << "Failed to initialize upscaler.\n";
            return 1;
        }

        std::cout << "  Upscaling...\n";
        auto result = upscaler.process(*texture, opt_upscale_alpha);
        if (!result) {
            std::cerr << "AI upscaling failed.\n";
            return 1;
        }

        *texture = std::move(*result);
        std::cout << "  Upscaled:   " << texture->width() << "x" << texture->height() << "\n";
#else
        std::cerr << "AI upscaling is not enabled in this build.\n"
                  << "Rebuild with -DWHITEOUT_ENABLE_UPSCALER=ON\n";
        return 1;
#endif
    }

    // -- Generate mipmaps ---------------------------------------------------
    if (mipmap_mode != MipmapMode::KeepOriginal) {
        if (tex::isBcn(texture->format())) {
            std::cout << "  Decompressing for mipmap generation...\n";
            *texture = texture->copyAsFormat(tex::PixelFormat::RGBA8, pool);
        }

        whiteout::u32 mip_count = 0;
        if (mipmap_mode == MipmapMode::Maximum) {
            mip_count = tex::computeMaxMipCount(texture->width(), texture->height());
        } else {
            auto max_mips = tex::computeMaxMipCount(texture->width(), texture->height());
            mip_count = static_cast<whiteout::u32>(
                std::clamp(mipmap_custom_count, 1, static_cast<int>(max_mips)));
        }

        std::cout << "  Generating " << mip_count << " mip levels...\n";
        texture->generateMipmaps(mip_count, pool);
        std::cout << "  Mip levels: " << texture->mipCount() << " (regenerated)\n";
    }

    // -- Pre-save format conversion -----------------------------------------
    switch (output_fmt) {
    case TFF::BLP: {
        auto blp_opts =
            build_blp_options(blp_version_idx, blp_encoding_idx, opt_blp_dither,
                              opt_blp_dither_strength, jpeg_quality, opt_jpeg_progressive);
        coerce_blp_format(*texture, blp_encoding_idx, blp_opts.encoding, pool);
        break;
    }
    case TFF::DDS: {
        if (dds_format_idx >= 0) {
            coerce_dds_format(*texture, dds_format_idx, dds_bc3n, opt_dds_invert_y, pool);
        } else if (opt_dds_invert_y) {
            if (tex::isBcn(texture->format()))
                *texture = texture->copyAsFormat(tex::PixelFormat::RGBA8, pool);
            texture->invertChannel(tex::Channel::G);
        }
        break;
    }
    default:
        break;
    }

    // -- Save ---------------------------------------------------------------
    bool ok = false;
    if (output_fmt == TFF::BLP) {
        auto blp_opts =
            build_blp_options(blp_version_idx, blp_encoding_idx, opt_blp_dither,
                              opt_blp_dither_strength, jpeg_quality, opt_jpeg_progressive);
        ok = converter.save(*texture, output_path, blp_opts);
    } else if (output_fmt == TFF::JPEG) {
        ok = converter.save(*texture, output_path, jpeg_quality, opt_jpeg_progressive);
    } else {
        ok = converter.save(*texture, output_path);
    }

    if (!ok) {
        std::cerr << "Failed to save " << output_path;
        if (converter.hasIssues())
            std::cerr << ": " << converter.getIssues().front();
        std::cerr << "\n";
        return 1;
    }

    std::cout << "  Output:     " << output_path << "\n";
    std::cout << "  Format:     " << TC::pixelFormatName(texture->format()) << "\n";
    std::cout << "  Dimensions: " << texture->width() << "x" << texture->height() << "\n";
    return 0;
}
