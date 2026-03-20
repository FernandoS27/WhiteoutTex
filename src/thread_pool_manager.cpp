// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "thread_pool_manager.h"

#include <algorithm>
#include <thread>

#include <whiteout/utils/simple_thread_pool.h>

namespace whiteout::textool {

struct ThreadPoolManager::Impl {
    std::unique_ptr<utils::SimpleThreadPool> pool;

    Impl() {
        const auto cores = std::max(1u, std::thread::hardware_concurrency());
        pool = std::make_unique<utils::SimpleThreadPool>(cores);
    }
};

ThreadPoolManager::ThreadPoolManager() : impl_(std::make_unique<Impl>()) {}

ThreadPoolManager::~ThreadPoolManager() = default;

interfaces::WorkerPool* ThreadPoolManager::get() const noexcept {
    return impl_->pool.get();
}

ThreadPoolManager& threadPoolManager() {
    static ThreadPoolManager instance;
    return instance;
}

} // namespace whiteout::textool
