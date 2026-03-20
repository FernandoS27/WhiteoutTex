// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file upscaler_service.h
/// @brief Async AI upscaler lifecycle management.
///        No SDL or ImGui dependencies.

#ifdef WHITEOUT_HAS_UPSCALER

#include "common_types.h"
#include "upscaler.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <whiteout/textures/texture.h>

namespace whiteout::textool::services {

/// Result of a completed upscale operation.
struct UpscaleResult {
    bool success = false;
    std::optional<whiteout::textures::Texture> texture;
    std::string status_message;
};

/// Manages the lifecycle of an asynchronous AI upscale operation.
/// Encapsulates the Upscaler instance, background thread, and progress
/// polling.
class UpscalerService {
public:
    UpscalerService() = default;
    ~UpscalerService();

    UpscalerService(const UpscalerService&) = delete;
    UpscalerService& operator=(const UpscalerService&) = delete;

    /// Launch an upscale on a background thread.
    void startAsync(const UpscalerModel& model, const whiteout::textures::Texture& source,
                    bool upscale_alpha);

    /// Poll for a completed result.  Returns the result exactly once
    /// after the background thread finishes, then std::nullopt.
    std::optional<UpscaleResult> pollResult();

    /// Thread-safe status string snapshot.
    std::string status() const;

    /// Returns true while an upscale is in progress.
    bool isRunning() const {
        return in_progress_;
    }

    /// Discover available models in the default model directory.
    static std::vector<UpscalerModel> availableModels();

    /// Check whether a Vulkan-capable GPU is present.
    static bool isGpuAvailable();

    /// Default model directory path.
    static std::filesystem::path defaultModelDir();

private:
    Upscaler upscaler_;
    bool in_progress_ = false;
    mutable std::mutex status_mtx_;
    std::string status_;
    std::atomic<bool> done_{false};
    bool success_ = false;
    std::optional<whiteout::textures::Texture> result_;
    std::thread thread_;
};

} // namespace whiteout::textool::services

#endif // WHITEOUT_HAS_UPSCALER
