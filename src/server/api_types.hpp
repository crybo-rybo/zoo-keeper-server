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

namespace zks::server {

struct ApiError {
    int http_status = 400;
    std::string message;
    std::string type = "invalid_request_error";
    std::optional<std::string> param;
    std::optional<std::string> code;
};

template <typename T> using ApiResult = std::expected<T, ApiError>;

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool,
};

[[nodiscard]] std::string_view to_string(MessageRole role) noexcept;

struct ChatMessage {
    MessageRole role = MessageRole::User;
    std::string content;
    std::optional<std::string> tool_call_id;

    [[nodiscard]] static ChatMessage system(std::string content);
    [[nodiscard]] static ChatMessage user(std::string content);
    [[nodiscard]] static ChatMessage assistant(std::string content);
    [[nodiscard]] static ChatMessage tool(std::string content, std::string tool_call_id);

    bool operator==(const ChatMessage& other) const = default;
};

[[nodiscard]] Result<void> validate_message_sequence(const std::vector<ChatMessage>& history,
                                                     MessageRole next_role);

enum class ToolInvocationStatus {
    Succeeded,
    ValidationFailed,
    ExecutionFailed,
};

[[nodiscard]] std::string_view to_string(ToolInvocationStatus status) noexcept;

enum class RuntimeErrorCode {
    InvalidConfig,
    InvalidSamplingParams,
    InvalidModelPath,
    InvalidContextSize,
    BackendInitFailed,
    ModelLoadFailed,
    ContextCreationFailed,
    InferenceFailed,
    TokenizationFailed,
    ContextWindowExceeded,
    InvalidMessageSequence,
    TemplateRenderFailed,
    AgentNotRunning,
    RequestCancelled,
    RequestTimeout,
    QueueFull,
    ToolNotFound,
    ToolExecutionFailed,
    InvalidToolSignature,
    InvalidToolSchema,
    ToolValidationFailed,
    ToolRetriesExhausted,
    ToolLoopLimitReached,
    RuntimeFailure,
};

struct RuntimeError {
    RuntimeErrorCode code = RuntimeErrorCode::RuntimeFailure;
    std::string message;
    std::optional<std::string> context;

    bool operator==(const RuntimeError& other) const = default;
};

template <typename T> using RuntimeResult = std::expected<T, RuntimeError>;

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameters_schema;

    bool operator==(const ToolDefinition& other) const = default;
};

struct ToolInvocationRecord {
    std::string id;
    std::string name;
    std::string arguments_json;
    ToolInvocationStatus status = ToolInvocationStatus::Succeeded;
    std::optional<std::string> result_json;
    std::optional<RuntimeError> error;

    bool operator==(const ToolInvocationRecord& other) const = default;
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

using TokenCallback = std::function<void(std::string_view)>;
using CompletionObserver = std::function<void(const RuntimeResult<CompletionResult>&)>;

class CompletionSource {
  public:
    virtual ~CompletionSource() = default;

    [[nodiscard]] virtual std::future_status
    wait_for(std::chrono::milliseconds timeout) const = 0;
    virtual RuntimeResult<CompletionResult> get() = 0;
};

struct CompletionHandle {
    std::uint64_t id = 0;
    std::shared_ptr<CompletionSource> source;

    [[nodiscard]] std::future_status wait_for(std::chrono::milliseconds timeout) const;
    RuntimeResult<CompletionResult> get();
};

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
    std::vector<ChatMessage> messages;
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
    CompletionHandle handle;
    CompletionObserver on_result;
    std::function<void()> cancel;
    std::shared_ptr<CompletionLease> lease;
};

} // namespace zks::server
