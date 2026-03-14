#pragma once

#include "server/chat_service.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FakeChatService final : public zks::server::ChatService {
  public:
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
        std::optional<std::function<void(std::string_view)>>) override {
        return std::unexpected(
            zks::server::server_error("not implemented in fake"));
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

  private:
    std::string model_id_ = "fake-model";
    std::vector<zoo::tools::ToolMetadata> tools_;
    std::atomic<int> stop_calls_{0};
    std::atomic<bool> ready_{true};
};
