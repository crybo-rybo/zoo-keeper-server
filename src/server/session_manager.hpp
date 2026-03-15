#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Transparent hash and equality for std::unordered_map<std::string, ...> that
// allows lookup by std::string_view without allocating a temporary std::string.
struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

namespace zks::server {

/// Manages per-session conversation history with TTL-based expiration.
///
/// When `max_sessions = 0` all session operations return an error immediately.
/// Thread-safe; all public methods acquire the internal mutex.
///
/// `reap_expired_sessions()` is called periodically by the background reaper
/// thread in `ServerRuntime`.
class SessionManager {
  public:
    /// Callback that starts a completion and returns a handle to the async result.
    using CompletionStarter =
        std::function<CompletionHandle(std::vector<ChatMessage>, std::optional<TokenCallback>)>;

    /// Callback to cancel an in-flight request by its completion ID.
    using RequestCanceler = std::function<void(std::uint64_t)>;

    SessionManager(std::string model_id, SessionConfig config, std::string base_system_prompt,
                   size_t max_history_messages, CompletionStarter completion_starter,
                   RequestCanceler request_canceler);

    [[nodiscard]] SessionHealth health() const noexcept;

    /// Removes all sessions whose TTL has elapsed. Called by the background reaper thread.
    void reap_expired_sessions();

    ApiResult<SessionSummary> create_session(const SessionCreateRequest& request);
    ApiResult<SessionSummary> get_session(std::string_view session_id);
    ApiResult<void> delete_session(std::string_view session_id);

    ApiResult<PendingChatCompletion> start_completion(
        const ChatCompletionRequest& request, std::atomic<std::uint64_t>& next_completion_id,
        std::optional<std::function<void(std::string_view)>> callback = std::nullopt);

    void stop();

  private:
    struct SessionState {
        explicit SessionState(std::string session_id, std::string session_model,
                              std::int64_t created_at, std::int64_t expires_at_value,
                              std::vector<ChatMessage> seeded_history)
            : id(std::move(session_id)), model(std::move(session_model)), created(created_at),
              last_used(created_at), expires_at(expires_at_value),
              history(std::move(seeded_history)) {}

        std::string id;
        std::string model;
        std::int64_t created = 0;
        std::int64_t last_used = 0;
        std::int64_t expires_at = 0;
        std::vector<ChatMessage> history;
        std::optional<std::uint64_t> active_request;
        bool closed = false;
        mutable std::mutex mutex;
    };

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] ApiError disabled_error() const;
    [[nodiscard]] SessionSummary snapshot_session(const SessionState& session) const;

    void reap_expired_sessions_locked(std::int64_t now,
                                      std::vector<std::shared_ptr<SessionState>>& expired);
    void finish_request(const std::shared_ptr<SessionState>& session, std::uint64_t request_id);
    void finish_request(const std::shared_ptr<SessionState>& session, std::uint64_t request_id,
                        const ChatMessage& user_message,
                        const RuntimeResult<CompletionResult>& result);

    std::string model_id_;
    SessionConfig config_;
    std::string base_system_prompt_;
    size_t max_history_messages_ = 0;
    CompletionStarter completion_starter_;
    RequestCanceler request_canceler_;
    std::atomic<std::uint64_t> next_session_id_{1};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionState>,
                       TransparentStringHash, TransparentStringEqual>
        sessions_;
    bool stopping_ = false;
};

} // namespace zks::server
