// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#ifdef WHITEOUT_HAS_UPSCALER

#include "services/upscaler_service.h"

#include <memory>

namespace tex = whiteout::textures;

namespace whiteout::textool::services {

UpscalerService::~UpscalerService() {
    if (thread_.joinable())
        thread_.join();
}

// ============================================================================
// Async entry point
// ============================================================================

void UpscalerService::startAsync(const UpscalerModel& model, const tex::Texture& source,
                                 bool upscale_alpha) {
    if (thread_.joinable())
        thread_.join();

    in_progress_ = true;
    done_.store(false);
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_ = "Initializing model...";
    }

    auto model_dir = Upscaler::defaultModelDir();
    auto model_copy = model;
    auto texture_copy = std::make_shared<tex::Texture>(source);
    auto* self = this;

    thread_ = std::thread([self, model_dir, model_copy, texture_copy, upscale_alpha]() {
        if (!self->upscaler_.init(model_dir, model_copy)) {
            {
                std::lock_guard<std::mutex> lk(self->status_mtx_);
                self->status_ = "Failed to load model.";
            }
            self->success_ = false;
            self->done_.store(true);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(self->status_mtx_);
            self->status_ = "Upscaling...";
        }
        auto result = self->upscaler_.process(*texture_copy, upscale_alpha);
        if (result) {
            self->result_ = std::move(*result);
            {
                std::lock_guard<std::mutex> lk(self->status_mtx_);
                self->status_ = "Upscale complete.";
            }
            self->success_ = true;
        } else {
            {
                std::lock_guard<std::mutex> lk(self->status_mtx_);
                self->status_ = "Upscale failed.";
            }
            self->success_ = false;
        }
        self->done_.store(true);
    });
}

// ============================================================================
// Polling
// ============================================================================

std::optional<UpscaleResult> UpscalerService::pollResult() {
    if (!done_.load())
        return std::nullopt;

    if (thread_.joinable())
        thread_.join();

    done_.store(false);
    in_progress_ = false;

    UpscaleResult r;
    r.success = success_;
    r.texture = std::move(result_);
    result_.reset();
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        r.status_message = status_;
    }
    return r;
}

std::string UpscalerService::status() const {
    std::lock_guard<std::mutex> lk(status_mtx_);
    return status_;
}

// ============================================================================
// Static delegates
// ============================================================================

std::vector<UpscalerModel> UpscalerService::availableModels() {
    return Upscaler::availableModels(Upscaler::defaultModelDir());
}

bool UpscalerService::isGpuAvailable() {
    return Upscaler::isGpuAvailable();
}

std::filesystem::path UpscalerService::defaultModelDir() {
    return Upscaler::defaultModelDir();
}

} // namespace whiteout::textool::services

#endif // WHITEOUT_HAS_UPSCALER
