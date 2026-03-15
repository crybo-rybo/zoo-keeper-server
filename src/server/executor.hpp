#pragma once

#include "server/result.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace zks::server {

/// Fixed-size thread pool with a bounded task queue.
///
/// Submitting a task when the queue is full returns `std::unexpected` immediately
/// rather than blocking or growing the queue. All public methods are thread-safe.
class BoundedExecutor {
  public:
    BoundedExecutor(size_t worker_count, size_t max_pending_tasks);
    ~BoundedExecutor();

    BoundedExecutor(const BoundedExecutor&) = delete;
    BoundedExecutor& operator=(const BoundedExecutor&) = delete;

    /// Enqueues a task for execution. Returns `std::unexpected` if the queue is full.
    [[nodiscard]] Result<void> submit(std::function<void()> task);
    void stop();

  private:
    void worker_loop();

    const size_t max_pending_tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

} // namespace zks::server
