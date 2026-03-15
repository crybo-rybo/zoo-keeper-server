#include "server/executor.hpp"

#include <iostream>
#include <utility>

namespace zks::server {

BoundedExecutor::BoundedExecutor(size_t worker_count, size_t max_pending_tasks)
    : max_pending_tasks_(max_pending_tasks == 0 ? 1 : max_pending_tasks) {
    const size_t effective_workers = worker_count == 0 ? 1 : worker_count;
    workers_.reserve(effective_workers);
    for (size_t index = 0; index < effective_workers; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

BoundedExecutor::~BoundedExecutor() {
    stop();
}

Result<void> BoundedExecutor::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return std::unexpected("Executor is stopping");
        }
        if (tasks_.size() >= max_pending_tasks_) {
            return std::unexpected("Executor queue is full");
        }
        tasks_.push(std::move(task));
    }

    cv_.notify_one();
    return {};
}

void BoundedExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void BoundedExecutor::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& error) {
            std::cerr << "Background task failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Background task failed with an unknown exception" << '\n';
        }
    }
}

} // namespace zks::server
