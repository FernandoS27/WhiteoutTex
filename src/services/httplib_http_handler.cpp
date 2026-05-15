// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/httplib_http_handler.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// CPPHTTPLIB_OPENSSL_SUPPORT is defined via target_compile_definitions in
// the root CMakeLists.txt — it switches httplib::Client to TLS mode when
// the URL scheme is https://.
#include <httplib.h>

namespace whiteout::textool::services {

namespace {

struct ParsedUrl {
    std::string origin;  // "https://host[:port]"
    std::string target;  // "/path?query"
    bool valid = false;
};

// Split a URL into (scheme://host[:port], /path...) — cpp-httplib's Client
// is constructed with the origin and called with the path separately.
ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl out;
    std::string_view sv = url;

    const auto scheme_end = sv.find("://");
    if (scheme_end == std::string_view::npos) return out;

    const auto path_pos = sv.find('/', scheme_end + 3);
    if (path_pos == std::string_view::npos) {
        out.origin.assign(sv);
        out.target = "/";
    } else {
        out.origin.assign(sv.substr(0, path_pos));
        out.target.assign(sv.substr(path_pos));
    }
    if (out.origin.empty() || out.target.empty()) return out;
    out.valid = true;
    return out;
}

struct HttpJob {
    std::string url;
    interfaces::HttpCallback callback;
    bool rangeRequest = false;
    u64 rangeStart = 0;
    u64 rangeEnd = 0;
};

} // namespace

struct HttplibHttpHandler::Impl {
    std::vector<std::thread> workers;
    std::deque<HttpJob> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> shutdown{false};

    explicit Impl(std::size_t nThreads) {
        workers.reserve(nThreads);
        for (std::size_t i = 0; i < nThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mutex);
            shutdown.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    void enqueue(HttpJob job) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void workerLoop() {
        while (true) {
            HttpJob job;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] {
                    return shutdown.load(std::memory_order_relaxed) ||
                           !queue.empty();
                });
                if (shutdown.load(std::memory_order_relaxed) && queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            executeJob(std::move(job));
        }
    }

    void executeJob(HttpJob job) {
        interfaces::HttpResponse resp;

        const auto parsed = parseUrl(job.url);
        if (!parsed.valid) {
            resp.error = "HttplibHttpHandler: invalid URL";
            job.callback(std::move(resp));
            return;
        }

        httplib::Client cli(parsed.origin);
        cli.set_follow_location(true);
        cli.set_connection_timeout(15, 0);
        cli.set_read_timeout(60, 0);
        cli.set_write_timeout(30, 0);
        cli.enable_server_certificate_verification(true);

        httplib::Headers headers{
            {"User-Agent", "WhiteoutTex/1.0 (cpp-httplib)"},
            {"Accept", "*/*"},
        };
        if (job.rangeRequest) {
            headers.emplace("Range",
                            "bytes=" + std::to_string(job.rangeStart) + "-" +
                                std::to_string(job.rangeEnd));
        }

        auto res = cli.Get(parsed.target, headers);
        if (!res) {
            resp.error = "cpp-httplib: " +
                         httplib::to_string(res.error());
            job.callback(std::move(resp));
            return;
        }

        resp.statusCode = static_cast<i32>(res->status);
        resp.body.assign(res->body.begin(), res->body.end());
        job.callback(std::move(resp));
    }
};

HttplibHttpHandler::HttplibHttpHandler(std::size_t nThreads)
    : impl_(std::make_unique<Impl>(nThreads)) {}

HttplibHttpHandler::~HttplibHttpHandler() = default;

u32 HttplibHttpHandler::capabilities() const noexcept {
    // cpp-httplib speaks HTTP/1.1 only — no multiplexing.
    return interfaces::HttpCapability::None;
}

void HttplibHttpHandler::getAsync(const std::string& url,
                                  interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    impl_->enqueue(std::move(job));
}

void HttplibHttpHandler::getRangeAsync(const std::string& url, u64 start, u64 end,
                                       interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    job.rangeRequest = true;
    job.rangeStart = start;
    job.rangeEnd = end;
    impl_->enqueue(std::move(job));
}

} // namespace whiteout::textool::services
