// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file upscaler.h
 * @brief GPU-accelerated image upscaling via Real-ESRGAN / ncnn / Vulkan
 *
 * Wraps the Real-ESRGAN ncnn implementation to upscale textures.
 * Only available when built with WHITEOUT_ENABLE_UPSCALER=ON and the
 * Vulkan SDK is present at configure time.
 */

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>

namespace whiteout::gui {

/// Describes a Real-ESRGAN model preset.
struct UpscalerModel {
    std::string display_name; ///< Human-readable name for the UI.
    std::string file_stem;    ///< File stem for .param / .bin files.
    i32 scale;                ///< Upscale factor (2, 3, or 4).
    i32 prepadding;           ///< Required tile prepadding.

    /// Build the label used in combo boxes: "Display Name (Nx)".
    std::string label() const {
        return display_name + " (" + std::to_string(scale) + "x)";
    }
};

/// GPU-accelerated image upscaler using Real-ESRGAN via ncnn / Vulkan.
///
/// Usage:
///   1. Check isGpuAvailable() to verify Vulkan GPU support.
///   2. Call availableModels() to discover downloaded model files.
///   3. init() with a chosen model.
///   4. process() to upscale a texture.
class Upscaler {
public:
    Upscaler();
    ~Upscaler();

    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    /// Returns true if a Vulkan-capable GPU is available for inference.
    static bool isGpuAvailable();

    /// Returns the default model directory (models/ next to the executable).
    static std::filesystem::path defaultModelDir();

    /// Lists model presets whose .param and .bin files exist in @p model_dir.
    static std::vector<UpscalerModel> availableModels(
        const std::filesystem::path& model_dir);

    /// Returns the index of the best available GPU (prefers discrete, most VRAM).
    static i32 bestGpuIndex();

    /// Initialize the upscaler with the given model.
    /// @param model_dir Directory containing .param / .bin model files.
    /// @param model     The model preset to load.
    /// @param gpu_id    Vulkan GPU device index (-1 = auto-select best GPU).
    /// @param tile_size Tile size for inference (0 = auto based on VRAM).
    /// @return true on success.
    bool init(const std::filesystem::path& model_dir,
              const UpscalerModel& model,
              i32 gpu_id = -1,
              i32 tile_size = 0);

    /// Upscale a texture.
    /// @param input Source texture (any uncompressed format; converted to RGBA8 internally).
    /// @param upscale_alpha If true, the alpha channel is upscaled through the
    ///        model (as a grayscale RGB image) instead of bicubic interpolation.
    /// @return Upscaled RGBA8 texture, or std::nullopt on failure.
    std::optional<textures::Texture> process(const textures::Texture& input,
                                             bool upscale_alpha = false);

    /// Returns true if the upscaler has been initialized and is ready.
    bool isReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::gui
