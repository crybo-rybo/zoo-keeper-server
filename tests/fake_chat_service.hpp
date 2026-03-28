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
#include <unordered_set>
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

    [[nodiscard]] const zoo::ModelConfig& model_config() const noexcept override {
        return model_config_;
    }

    [[nodiscard]] const zoo::AgentConfig& agent_config() const noexcept override {
        return agent_config_;
    }

    [[nodiscard]] const zoo::GenerationOptions& default_generation() const noexcept override {
        return default_generation_;
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

    zks::server::ApiResult<zks::server::PendingChatCompletion>
    start_agent_chat(const zks::server::AgentChatRequest& request,
                     std::optional<zks::server::TokenCallback> callback = std::nullopt) override {
        zks::server::ChatCompletionRequest completion_request;
        completion_request.model = request.model;
        completion_request.messages = {request.message};
        return start_completion(completion_request, std::move(callback));
    }

    zks::server::ApiResult<zks::server::PendingExtraction>
    start_extraction(const zks::server::ExtractionRequest& request,
                     std::optional<zks::server::TokenCallback> callback = std::nullopt) override {
        if (callback.has_value()) {
            (*callback)(extraction_response_.text);
        }

        std::promise<zks::server::RuntimeResult<zks::server::ExtractionResult>> promise;
        auto future = promise.get_future();
        promise.set_value(extraction_response_);

        zks::server::PendingExtraction pending;
        pending.id = "extract-fake-1";
        pending.created = 0;
        pending.model = request.model;
        pending.handle = zks::server::make_extraction_handle(3, std::move(future));
        pending.cancel = [] {};
        return pending;
    }

    zks::server::ApiResult<void> cancel_request(std::string_view request_id) override {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        if (cancelable_request_ids_.erase(std::string(request_id)) == 0u) {
            return std::unexpected(
                zks::server::not_found_error("Unknown request", "request_not_found"));
        }
        return {};
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

    zks::server::ApiResult<zks::server::AgentHistorySnapshot> get_agent_history() override {
        return agent_history_;
    }

    zks::server::ApiResult<void> clear_agent_history() override {
        agent_history_.messages.clear();
        agent_history_.estimated_tokens = 0;
        agent_history_.context_exceeded = false;
        return {};
    }

    zks::server::ApiResult<zks::server::AgentHistorySnapshot>
    replace_agent_history(const zks::server::AgentHistoryRequest& request) override {
        agent_history_.messages = request.messages;
        return agent_history_;
    }

    zks::server::ApiResult<zks::server::AgentHistorySnapshot>
    swap_agent_history(const zks::server::AgentHistoryRequest& request) override {
        auto previous = agent_history_;
        agent_history_.messages = request.messages;
        return previous;
    }

    zks::server::ApiResult<zks::server::AgentHistorySnapshot>
    append_agent_history_message(const zks::server::AgentHistoryMessageRequest& request) override {
        agent_history_.messages.push_back(request);
        return agent_history_;
    }

    zks::server::ApiResult<std::string> get_system_prompt() override {
        return system_prompt_;
    }

    zks::server::ApiResult<std::string> set_system_prompt(std::string prompt) override {
        system_prompt_ = std::move(prompt);
        return system_prompt_;
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

    void set_extraction_response(zks::server::ExtractionResult response) {
        extraction_response_ = std::move(response);
    }

    void set_cancelable_request_id(std::string request_id) {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        cancelable_request_ids_.insert(std::move(request_id));
    }

    std::shared_ptr<Latch> finish_latch() const noexcept {
        return finish_latch_;
    }

  private:
    std::string model_id_ = "fake-model";
    zoo::ModelConfig model_config_{.model_path = "/tmp/fake.gguf", .context_size = 2048};
    zoo::AgentConfig agent_config_{};
    zoo::GenerationOptions default_generation_{};
    std::vector<zks::server::ToolDefinition> tools_;
    zks::server::CompletionResult response_;
    zks::server::ExtractionResult extraction_response_{
        .text = R"({"name":"Alice"})",
        .data = nlohmann::json{{"name", "Alice"}},
    };
    zks::server::AgentHistorySnapshot agent_history_{};
    std::string system_prompt_ = "fake-system-prompt";
    mutable std::mutex response_mutex_;
    mutable std::mutex cancel_mutex_;
    std::unordered_set<std::string> cancelable_request_ids_;
    std::atomic<int> stop_calls_{0};
    std::atomic<bool> ready_{true};
    std::atomic<FakeCompletionMode> mode_{FakeCompletionMode::ServerError};
    std::shared_ptr<Latch> finish_latch_ = std::make_shared<Latch>();
    std::future<void> streaming_future_;
};
