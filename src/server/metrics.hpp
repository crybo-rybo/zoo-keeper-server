#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace zks::server {

struct MetricsSnapshot {
    uint64_t requests_total = 0;
    uint64_t requests_errors = 0;
    uint64_t requests_cancelled_total = 0;
    uint64_t requests_queue_rejected_total = 0;
    uint64_t stream_disconnects_total = 0;
    size_t active_sessions = 0;
    std::string model_id;
    int64_t uptime_seconds = 0;
};

class ServerMetrics {
  public:
    ServerMetrics() : start_time_(std::chrono::steady_clock::now()) {}

    void increment_requests() noexcept {
        requests_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void increment_errors() noexcept {
        requests_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    void increment_cancelled() noexcept {
        requests_cancelled_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void increment_queue_rejected() noexcept {
        requests_queue_rejected_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void increment_stream_disconnects() noexcept {
        stream_disconnects_total_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t requests_total() const noexcept {
        return requests_total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t requests_errors() const noexcept {
        return requests_errors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t requests_cancelled_total() const noexcept {
        return requests_cancelled_total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t requests_queue_rejected_total() const noexcept {
        return requests_queue_rejected_total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t stream_disconnects_total() const noexcept {
        return stream_disconnects_total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t uptime_seconds() const noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start_time_)
            .count();
    }

  private:
    std::atomic<uint64_t> requests_total_{0};
    std::atomic<uint64_t> requests_errors_{0};
    std::atomic<uint64_t> requests_cancelled_total_{0};
    std::atomic<uint64_t> requests_queue_rejected_total_{0};
    std::atomic<uint64_t> stream_disconnects_total_{0};
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace zks::server
