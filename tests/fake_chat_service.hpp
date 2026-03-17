#pragma once

#include "server/chat_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// Configurable behavior modes for `FakeChatService::start_completion()`.
enum class FakeCompletionMode {
    /// Returns a successful response immediately from `start_completion()`.
    Success,
    /// Returns `server_error("not implemented in fake")` from `start_completion()` itself.
    ServerError,
    /// Returns `service_unavailable_error("queue full", "queue_full")` from `start_completion()`.
    QueueFull,
    /// Returns a PendingChatCompletion whose future resolves to `RequestCancelled`.
    Cancelled,
    /// Returns a PendingChatCompletion whose future resolves to a successful result after
    /// streaming a few tokens via the callback. The future blocks on `finish_latch`
    /// so the test can drop the connection before it completes.
    StreamingSuccess,
};

namespace {

inline zks::server::CompletionHandle
make_handle(std::uint64_t id,
            std::future<zks::server::RuntimeResult<zks::server::CompletionResult>> future) {
    return zks::server::make_completion_handle(id, std::move(future));
}

} // namespace

class FakeChatService final : public zks::server::ChatService {
  public:
    /// Latch used by `StreamingSuccess` mode to block the completion future.
    struct Latch {
        std::mutex mutex;
        std::condition_variable cv;
        bool signaled = false;

        void signal() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                signaled = true;
            }
            cv.notify_all();
        }

        void wait_for(std::chrono::seconds timeout) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, timeout, [this] { return signaled; });
        }
    };

    [[nodiscard]] bool is_ready() const noexcept override {
        return ready_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& model_id() const noexcept override {
        return model_id_;
    }

    [[nodiscard]] const std::vector<zks::server::ToolDefinition>& tools() const noexcept override {
        return tools_;
    }

    [[nodiscard]] zks::server::SessionHealth session_health() const noexcept override {
        return {};
    }

    ~FakeChatService() {
        finish_latch_->signal();
        if (streaming_future_.valid()) {
            streaming_future_.wait();
        }
    }

    zks::server::ApiResult<zks::server::PendingChatCompletion>
    start_completion(const zks::server::ChatCompletionRequest&,
                     std::optional<zks::server::TokenCallback> callback = std::nullopt) override {

        const auto mode = mode_.load(std::memory_order_acquire);

        if (mode == FakeCompletionMode::Success) {
            std::promise<zks::server::RuntimeResult<zks::server::CompletionResult>> promise;
            auto future = promise.get_future();
            {
                std::lock_guard<std::mutex> lock(response_mutex_);
                promise.set_value(response_);
            }

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-success-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = make_handle(0, std::move(future));
            pending.cancel = [] {};
            return pending;
        }

        if (mode == FakeCompletionMode::ServerError) {
            return std::unexpected(zks::server::server_error("not implemented in fake"));
        }

        if (mode == FakeCompletionMode::QueueFull) {
            return std::unexpected(
                zks::server::service_unavailable_error("queue full", "queue_full"));
        }

        if (mode == FakeCompletionMode::Cancelled) {
            std::promise<zks::server::RuntimeResult<zks::server::CompletionResult>> promise;
            auto future = promise.get_future();
            promise.set_value(std::unexpected(zks::server::RuntimeError{
                zks::server::RuntimeErrorCode::RequestCancelled,
                "request was cancelled",
            }));

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-cancelled-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = make_handle(1, std::move(future));
            pending.cancel = [] {};
            return pending;
        }

        if (mode == FakeCompletionMode::StreamingSuccess) {
            auto latch = finish_latch_;
            std::promise<zks::server::RuntimeResult<zks::server::CompletionResult>> promise;
            auto future = promise.get_future();

            streaming_future_ =
                std::async(std::launch::async, [cb = std::move(callback), latch = std::move(latch),
                                                promise = std::move(promise)]() mutable {
                    if (cb.has_value()) {
                        (*cb)("hello ");
                        (*cb)("world");
                    }

                    latch->wait_for(std::chrono::seconds(10));

                    zks::server::CompletionResult response;
                    response.text = "hello world";
                    promise.set_value(std::move(response));
                });

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-stream-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = make_handle(2, std::move(future));
            pending.cancel = [] {};
            return pending;
        }

        return std::unexpected(zks::server::server_error("unknown fake mode"));
    }

    zks::server::ApiResult<zks::server::SessionSummary>
    create_session(const zks::server::SessionCreateRequest&) override {
        return std::unexpected(zks::server::server_error("not implemented in fake"));
    }

    zks::server::ApiResult<zks::server::SessionSummary> get_session(std::string_view) override {
        return std::unexpected(zks::server::server_error("not implemented in fake"));
    }

    zks::server::ApiResult<void> delete_session(std::string_view) override {
        return std::unexpected(zks::server::server_error("not implemented in fake"));
    }

    void stop() override {
        stop_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    int stop_calls() const noexcept {
        return stop_calls_.load(std::memory_order_relaxed);
    }

    void set_ready(bool ready) noexcept {
        ready_.store(ready, std::memory_order_relaxed);
    }

    void set_model_id(std::string id) {
        model_id_ = std::move(id);
    }

    void set_mode(FakeCompletionMode mode) noexcept {
        mode_.store(mode, std::memory_order_release);
    }

    void set_tools(std::vector<zks::server::ToolDefinition> tools) {
        tools_ = std::move(tools);
    }

    void set_response(zks::server::CompletionResult response) {
        std::lock_guard<std::mutex> lock(response_mutex_);
        response_ = std::move(response);
    }

    std::shared_ptr<Latch> finish_latch() const noexcept {
        return finish_latch_;
    }

  private:
    std::string model_id_ = "fake-model";
    std::vector<zks::server::ToolDefinition> tools_;
    zks::server::CompletionResult response_;
    mutable std::mutex response_mutex_;
    std::atomic<int> stop_calls_{0};
    std::atomic<bool> ready_{true};
    std::atomic<FakeCompletionMode> mode_{FakeCompletionMode::ServerError};
    std::shared_ptr<Latch> finish_latch_ = std::make_shared<Latch>();
    std::future<void> streaming_future_;
};
