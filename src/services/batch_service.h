// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file batch_service.h
/// @brief Batch conversion orchestration and worker management.
///        No SDL or ImGui dependencies.

#include "common_types.h"
#include "preferences.h"
#include "texture_converter.h"

#ifdef WHITEOUT_HAS_UPSCALER
#include "upscaler.h"
#endif

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <whiteout/textures/texture.h>

namespace whiteout::textool::services {

/// Immutable snapshot of all parameters needed for a batch conversion run.
struct BatchJob {
    std::string input_dir;
    std::string output_dir;
    std::vector<std::string> files;
    BatchPrefs prefs;
#ifdef WHITEOUT_HAS_UPSCALER
    std::vector<UpscalerModel> upscale_models;
#endif
};

/// Read-only progress snapshot of an in-progress batch conversion.
struct BatchProgress {
    i32 total = 0;
    i32 processed = 0;
    i32 success = 0;
    i32 fail = 0;
    bool done = false;
};

/// Batch conversion orchestrator.  Accepts a BatchJob snapshot, spawns
/// worker threads, and exposes progress via atomic-backed polling.
class BatchService {
public:
    BatchService() = default;
    ~BatchService();

    BatchService(const BatchService&) = delete;
    BatchService& operator=(const BatchService&) = delete;

    /// Start a batch conversion with the given job.
    /// Returns an error message on immediate failure, or empty string on success.
    std::string start(BatchJob job);

    /// Poll current progress (lock-free).
    BatchProgress progress() const;

    /// Join all worker threads.  Safe to call multiple times.
    void joinWorkers();

    /// Returns true while a batch is in progress.
    bool isRunning() const {
        return converting_;
    }

private:
    void workerFunc();
    bool saveOne(whiteout::textures::TextureConverter& converter,
                 whiteout::textures::Texture tex_copy, const std::string& out_path,
                 whiteout::textures::TextureKind kind);

    BatchJob job_;
    bool converting_ = false;
    std::atomic<i32> batch_processed_{0};
    std::atomic<i32> batch_success_{0};
    std::atomic<i32> batch_fail_{0};
    std::atomic<bool> batch_done_{false};
    std::vector<std::thread> workers_;
    std::atomic<i32> work_index_{0};
};

} // namespace whiteout::textool::services
