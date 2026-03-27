#pragma once

#include "server/result.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zoo/core/types.hpp>

namespace zks::server {

// --- Type aliases: use zoo-keeper types directly (controlled submodule) ---

using MessageRole = zoo::Role;
using ChatMessage = zoo::Message;
using RuntimeErrorCode = zoo::ErrorCode;
using RuntimeError = zoo::Error;
template <typename T> using RuntimeResult = zoo::Expected<T>;
using ToolInvocationStatus = zoo::ToolInvocationStatus;
using ToolInvocationRecord = zoo::ToolInvocation;

using TokenCallback = std::function<void(std::string_view)>;

// --- String conversions ---

/// Thin wrapper around zoo::role_to_string().
[[nodiscard]] std::string_view to_string(MessageRole role) noexcept;

// ToolInvocationStatus: use zoo::to_string() via ADL.

// --- API error handling ---

struct ApiError {
    int http_status = 400;
    std::string message;
    std::string type = "invalid_request_error";
    std::optional<std::string> param;
    std::optional<std::string> code;
};

template <typename T> using ApiResult = std::expected<T, ApiError>;

// --- Message validation ---

[[nodiscard]] Result<void> validate_message_sequence(const std::vector<ChatMessage>& history,
                                                     MessageRole next_role);

// --- Server-specific types (different field types from zoo) ---

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameters_schema;

    bool operator==(const ToolDefinition& other) const = default;
};

struct CompletionUsage {
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;

    bool operator==(const CompletionUsage& other) const = default;
};

struct CompletionMetrics {
    std::chrono::milliseconds latency_ms{0};
    std::chrono::milliseconds time_to_first_token_ms{0};
    double tokens_per_second = 0.0;

    bool operator==(const CompletionMetrics& other) const = default;
};

struct CompletionResult {
    std::string text;
    CompletionUsage usage;
    CompletionMetrics metrics;
    std::vector<ToolInvocationRecord> tool_invocations;

    bool operator==(const CompletionResult& other) const = default;
};

using CompletionObserver = std::function<void(const RuntimeResult<CompletionResult>&)>;

/// Non-virtual inner state for CompletionHandle. Heap-allocated via shared_ptr
/// to keep CompletionHandle movable/copyable.
struct CompletionState {
    mutable std::mutex mutex;
    mutable std::future<RuntimeResult<CompletionResult>> future;
    bool consumed = false;
};

/// Lightweight handle to an in-flight or completed async completion.
/// No virtual dispatch — holds a concrete future via shared state.
struct CompletionHandle {
    std::uint64_t id = 0;
    std::shared_ptr<CompletionState> state;

    [[nodiscard]] std::future_status wait_for(std::chrono::milliseconds timeout) const;
    /// Consumes the result from the underlying future. May only be called once.
    RuntimeResult<CompletionResult> get();
};

/// Constructs a CompletionHandle from a result future.
[[nodiscard]] CompletionHandle
make_completion_handle(std::uint64_t id, std::future<RuntimeResult<CompletionResult>> future);

// --- API error helpers ---

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

inline ApiError not_found_error(std::string message,
                                std::optional<std::string> code = std::nullopt) {
    return ApiError{404, std::move(message), "not_found_error", std::nullopt, std::move(code)};
}

inline ApiError conflict_error(std::string message,
                               std::optional<std::string> code = std::nullopt) {
    return ApiError{409, std::move(message), "conflict_error", std::nullopt, std::move(code)};
}

inline ApiError auth_error(std::string message, std::optional<std::string> code = std::nullopt) {
    return ApiError{401, std::move(message), "auth_error", std::nullopt, std::move(code)};
}

// --- Request/response types ---

struct ChatCompletionRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    bool stream = false;
    std::optional<std::string> session_id;

    // Per-request sampling overrides (optional; server defaults used when absent).
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int> top_k;
    std::optional<float> repeat_penalty;
    std::optional<int> max_tokens;
    std::optional<int> seed;
    std::optional<std::vector<std::string>> stop;
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

// --- Completion lifecycle ---

/// RAII guard that invokes a cleanup callback exactly once, either on destruction or
/// when `release()` is called explicitly, whichever comes first.
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
    CompletionHandle handle;
    CompletionObserver on_result;
    std::function<void()> cancel;
    std::shared_ptr<CompletionLease> lease;
};

} // namespace zks::server
