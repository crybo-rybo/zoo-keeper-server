#pragma once

#include "server/chat_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <zoo/agent.hpp>
#include <zoo/core/types.hpp>

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
    /// Returns a PendingChatCompletion whose future resolves to a successful Response
    /// after streaming a few tokens via the callback. The future blocks on `finish_latch`
    /// so the test can drop the connection before it completes.
    StreamingSuccess,
};

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

    [[nodiscard]] const std::vector<zoo::tools::ToolMetadata>& tools() const noexcept override {
        return tools_;
    }

    [[nodiscard]] zks::server::SessionHealth session_health() const noexcept override {
        return {};
    }

    zks::server::ApiResult<zks::server::PendingChatCompletion> start_completion(
        const zks::server::ChatCompletionRequest&,
        std::optional<std::function<void(std::string_view)>> callback = std::nullopt) override {

        auto mode = mode_.load(std::memory_order_acquire);

        if (mode == FakeCompletionMode::Success) {
            std::promise<zoo::Expected<zoo::Response>> promise;
            auto future = promise.get_future();
            {
                std::lock_guard<std::mutex> lock(response_mutex_);
                promise.set_value(response_);
            }

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-success-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = zoo::RequestHandle(0, std::move(future));
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
            std::promise<zoo::Expected<zoo::Response>> promise;
            auto future = promise.get_future();
            promise.set_value(std::unexpected(
                zoo::Error{zoo::ErrorCode::RequestCancelled, "request was cancelled"}));

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-cancelled-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = zoo::RequestHandle(1, std::move(future));
            pending.cancel = [] {};
            return pending;
        }

        if (mode == FakeCompletionMode::StreamingSuccess) {
            // Stream some tokens via callback, then wait on the latch before resolving.
            auto latch = finish_latch_;
            auto mid = model_id_;
            std::promise<zoo::Expected<zoo::Response>> promise;
            auto future = promise.get_future();

            std::thread([cb = std::move(callback), latch = std::move(latch),
                         mid = std::move(mid), promise = std::move(promise)]() mutable {
                // Stream tokens if a callback was provided.
                if (cb.has_value()) {
                    (*cb)("hello ");
                    (*cb)("world");
                }

                // Wait for the test to signal us to finish (or timeout).
                latch->wait_for(std::chrono::seconds(10));

                zoo::Response response;
                response.text = "hello world";
                response.usage = {};
                response.metrics = {};
                promise.set_value(std::move(response));
            }).detach();

            zks::server::PendingChatCompletion pending;
            pending.id = "fake-stream-1";
            pending.created = 0;
            pending.model = model_id_;
            pending.handle = zoo::RequestHandle(2, std::move(future));
            pending.cancel = [] {};
            return pending;
        }

        return std::unexpected(zks::server::server_error("unknown fake mode"));
    }

    zks::server::ApiResult<zks::server::SessionSummary>
    create_session(const zks::server::SessionCreateRequest&) override {
        return std::unexpected(
            zks::server::server_error("not implemented in fake"));
    }

    zks::server::ApiResult<zks::server::SessionSummary>
    get_session(std::string_view) override {
        return std::unexpected(
            zks::server::server_error("not implemented in fake"));
    }

    zks::server::ApiResult<void> delete_session(std::string_view) override {
        return std::unexpected(
            zks::server::server_error("not implemented in fake"));
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

    void set_tools(std::vector<zoo::tools::ToolMetadata> tools) {
        tools_ = std::move(tools);
    }

    void set_response(zoo::Response response) {
        std::lock_guard<std::mutex> lock(response_mutex_);
        response_ = std::move(response);
    }

    /// Returns the shared latch used by `StreamingSuccess` mode. Call `signal()` on
    /// the returned latch to unblock the fake completion future.
    std::shared_ptr<Latch> finish_latch() const noexcept {
        return finish_latch_;
    }

  private:
    std::string model_id_ = "fake-model";
    std::vector<zoo::tools::ToolMetadata> tools_;
    zoo::Response response_;
    mutable std::mutex response_mutex_;
    std::atomic<int> stop_calls_{0};
    std::atomic<bool> ready_{true};
    std::atomic<FakeCompletionMode> mode_{FakeCompletionMode::ServerError};
    std::shared_ptr<Latch> finish_latch_ = std::make_shared<Latch>();
};
