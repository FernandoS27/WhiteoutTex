// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "thread_pool_manager.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <whiteout/utils/simple_thread_pool.h>

namespace whiteout::gui {

// ============================================================================
// ThreadPoolManager::Impl
// ============================================================================

struct ThreadPoolManager::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::unique_ptr<utils::SimpleThreadPool>> pools;
    std::vector<bool> in_use;

    Impl() {
        const auto cores = std::max(1u, std::thread::hardware_concurrency());
        pools.reserve(kMaxPoolCount);
        in_use.resize(kMaxPoolCount, false);
        for (std::size_t i = 0; i < kMaxPoolCount; ++i)
            pools.push_back(std::make_unique<utils::SimpleThreadPool>(cores));
    }

    bool hasFree() const {
        for (auto b : in_use)
            if (!b) return true;
        return false;
    }
};

// ============================================================================
// BorrowedPool
// ============================================================================

ThreadPoolManager::BorrowedPool::BorrowedPool() = default;

ThreadPoolManager::BorrowedPool::~BorrowedPool() { release(); }

ThreadPoolManager::BorrowedPool::BorrowedPool(ThreadPoolManager* mgr, std::size_t idx)
    : mgr_(mgr), index_(idx) {}

ThreadPoolManager::BorrowedPool::BorrowedPool(BorrowedPool&& o) noexcept
    : mgr_(o.mgr_), index_(o.index_) { o.mgr_ = nullptr; }

ThreadPoolManager::BorrowedPool&
ThreadPoolManager::BorrowedPool::operator=(BorrowedPool&& o) noexcept {
    if (this != &o) {
        release();
        mgr_ = o.mgr_;
        index_ = o.index_;
        o.mgr_ = nullptr;
    }
    return *this;
}

interfaces::WorkerPool* ThreadPoolManager::BorrowedPool::get() const noexcept {
    return mgr_ ? mgr_->poolAt(index_) : nullptr;
}

void ThreadPoolManager::BorrowedPool::release() {
    if (mgr_) {
        mgr_->returnPool(index_);
        mgr_ = nullptr;
    }
}

// ============================================================================
// ThreadPoolManager
// ============================================================================

ThreadPoolManager::ThreadPoolManager() : impl_(std::make_unique<Impl>()) {}

ThreadPoolManager::~ThreadPoolManager() = default;

ThreadPoolManager::BorrowedPool ThreadPoolManager::borrow() {
    std::unique_lock lock(impl_->mtx);
    impl_->cv.wait(lock, [this] { return impl_->hasFree(); });
    for (std::size_t i = 0; i < kMaxPoolCount; ++i) {
        if (!impl_->in_use[i]) {
            impl_->in_use[i] = true;
            return BorrowedPool(this, i);
        }
    }
    return {};
}

interfaces::WorkerPool* ThreadPoolManager::poolAt(std::size_t idx) const {
    return impl_->pools[idx].get();
}

void ThreadPoolManager::returnPool(std::size_t idx) {
    {
        std::lock_guard lock(impl_->mtx);
        impl_->in_use[idx] = false;
    }
    impl_->cv.notify_one();
}

// ============================================================================
// Global accessor
// ============================================================================

ThreadPoolManager& threadPoolManager() {
    static ThreadPoolManager instance;
    return instance;
}

} // namespace whiteout::gui
