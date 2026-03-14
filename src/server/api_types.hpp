#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zoo/agent.hpp>
#include <zoo/core/types.hpp>

namespace zks::server {

struct ApiError {
    int http_status = 400;
    std::string message;
    std::string type = "invalid_request_error";
    std::optional<std::string> param;
    std::optional<std::string> code;
};

template <typename T> using ApiResult = std::expected<T, ApiError>;

inline ApiError invalid_request_error(std::string message,
                                      std::optional<std::string> param = std::nullopt,
                                      std::optional<std::string> code = std::nullopt) {
    return ApiError{400, std::move(message), "invalid_request_error", std::move(param),
                    std::move(code)};
}

inline ApiError service_unavailable_error(std::string message,
                                          std::optional<std::string> code = std::nullopt) {
    return ApiError{503, std::move(message), "service_unavailable_error", std::nullopt,
                    std::move(code)};
}

inline ApiError server_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{500, std::move(message), "server_error", std::nullopt, std::move(code)};
}

inline ApiError not_found_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{404, std::move(message), "not_found_error", std::nullopt, std::move(code)};
}

inline ApiError conflict_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{409, std::move(message), "conflict_error", std::nullopt, std::move(code)};
}

inline ApiError auth_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{401, std::move(message), "auth_error", std::nullopt, std::move(code)};
}

struct ChatCompletionRequest {
    std::string model;
    std::vector<zoo::Message> messages;
    bool stream = false;
    std::optional<std::string> session_id;
};

struct SessionCreateRequest {
    std::string model;
    std::optional<std::string> system_prompt;
};

struct SessionSummary {
    std::string id;
    std::string model;
    std::int64_t created = 0;
    std::int64_t last_used = 0;
    std::int64_t expires_at = 0;
};

struct SessionHealth {
    bool enabled = false;
    size_t active = 0;
    size_t max_sessions = 0;
    std::uint32_t idle_ttl_seconds = 0;
};

class CompletionLease {
  public:
    explicit CompletionLease(std::function<void()> on_release = {})
        : on_release_(std::move(on_release)) {}

    ~CompletionLease() {
        release();
    }

    void release() {
        std::call_once(release_once_, [this] {
            if (on_release_) {
                on_release_();
            }
        });
    }

  private:
    std::function<void()> on_release_;
    std::once_flag release_once_;
};

struct PendingChatCompletion {
    std::string id;
    std::int64_t created = 0;
    std::string model;
    zoo::RequestHandle handle;
    std::function<void(const zoo::Expected<zoo::Response>&)> on_result;
    std::function<void()> cancel;
    std::shared_ptr<CompletionLease> lease;
};

} // namespace zks::server
