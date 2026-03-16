// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <memory>

#include <whiteout/interfaces.h>

namespace whiteout::gui {

/// Maximum number of thread pools that can exist simultaneously.
inline constexpr std::size_t kMaxPoolCount = 4;

/// Manages a fixed-size pool of SimpleThreadPool instances.
///
/// Each pool has as many threads as hardware cores.  Callers borrow a pool
/// via `borrow()` and return it automatically when BorrowedPool is destroyed.
/// If all pools are currently in use, `borrow()` blocks until one is freed.
class ThreadPoolManager {
public:
    /// RAII handle returned by borrow(). Returns the pool on destruction.
    class BorrowedPool {
    public:
        BorrowedPool();
        ~BorrowedPool();

        BorrowedPool(const BorrowedPool&) = delete;
        BorrowedPool& operator=(const BorrowedPool&) = delete;

        BorrowedPool(BorrowedPool&& o) noexcept;
        BorrowedPool& operator=(BorrowedPool&& o) noexcept;

        /// The borrowed WorkerPool, or nullptr if this handle is empty.
        interfaces::WorkerPool* get() const noexcept;

    private:
        friend class ThreadPoolManager;
        BorrowedPool(ThreadPoolManager* mgr, std::size_t idx);

        void release();

        ThreadPoolManager* mgr_ = nullptr;
        std::size_t index_ = 0;
    };

    ThreadPoolManager();
    ~ThreadPoolManager();

    ThreadPoolManager(const ThreadPoolManager&) = delete;
    ThreadPoolManager& operator=(const ThreadPoolManager&) = delete;

    /// Borrow a thread pool.  Blocks if all pools are currently in use.
    BorrowedPool borrow();

private:
    friend class BorrowedPool;

    interfaces::WorkerPool* poolAt(std::size_t idx) const;
    void returnPool(std::size_t idx);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Global thread pool manager instance.
ThreadPoolManager& threadPoolManager();

} // namespace whiteout::gui
