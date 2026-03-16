// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <memory>

#include <whiteout/interfaces.h>

namespace whiteout::gui {

/// Owns a single thread pool with as many threads as hardware cores.
///
/// The underlying WorkerPool is thread-safe, so any thread may call get()
/// and use the returned pool concurrently.
class ThreadPoolManager {
public:
    ThreadPoolManager();
    ~ThreadPoolManager();

    ThreadPoolManager(const ThreadPoolManager&) = delete;
    ThreadPoolManager& operator=(const ThreadPoolManager&) = delete;

    /// The shared WorkerPool instance.
    interfaces::WorkerPool* get() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Global thread pool manager instance.
ThreadPoolManager& threadPoolManager();

} // namespace whiteout::gui
