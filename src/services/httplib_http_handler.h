// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <whiteout/interfaces.h>

namespace whiteout::textool::services {

/// Cross-platform HTTP(S) handler backed by cpp-httplib (single-header).
///
/// Built only on macOS, where WhiteoutLib's WinHTTP and libcurl backends are
/// not used.  Windows and Linux continue to use whiteout::utils::SimpleHttpHandler.
///
/// Requests are dispatched onto an internal worker pool.  HTTPS is supported
/// when CPPHTTPLIB_OPENSSL_SUPPORT is defined at build time (OpenSSL link).
/// Thread-safe: getAsync / getRangeAsync may be called concurrently.
class HttplibHttpHandler : public interfaces::HttpHandler {
public:
    explicit HttplibHttpHandler(std::size_t nThreads = 4);
    ~HttplibHttpHandler() override;

    HttplibHttpHandler(const HttplibHttpHandler&) = delete;
    HttplibHttpHandler& operator=(const HttplibHttpHandler&) = delete;

    u32 capabilities() const noexcept override;

    void getAsync(const std::string& url,
                  interfaces::HttpCallback callback) override;

    void getRangeAsync(const std::string& url, u64 start, u64 end,
                       interfaces::HttpCallback callback) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::textool::services
