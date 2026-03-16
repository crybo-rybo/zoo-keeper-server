#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/internal_utils.hpp"
#include "server/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zks::server {

/// Result of SessionStore::begin_request() — the validated, history-enriched message
/// list ready to be sent to the agent.
struct BeginRequestResult {
    std::vector<ChatMessage> messages;
    std::int64_t started_at = 0;
    ChatMessage user_message; // the single user message from the request
};

/// Pure state store for per-session conversation history with TTL-based expiration.
///
/// No knowledge of `CompletionHandle`, `zoo::Agent`, or `TokenCallback` —
/// completion orchestration lives in `ZooChatService`.
///
/// When `max_sessions = 0` all session operations return an error immediately.
/// Thread-safe; all public methods acquire the internal mutex.
class SessionStore {
  public:
    SessionStore(std::string model_id, SessionConfig config, std::string base_system_prompt,
                 size_t max_history_messages, Clock clock = now_seconds);

    [[nodiscard]] SessionHealth health() const noexcept;

    /// Removes all sessions whose TTL has elapsed.
    void reap_expired_sessions();

    ApiResult<SessionSummary> create_session(const SessionCreateRequest& request);
    ApiResult<SessionSummary> get_session(std::string_view session_id);
    ApiResult<void> delete_session(std::string_view session_id);

    /// Validates the request, builds the full message list (history + new user message),
    /// and marks the session as having an active request.
    ApiResult<BeginRequestResult> begin_request(const ChatCompletionRequest& request,
                                                std::uint64_t request_id);

    /// Appends the user message and assistant response to history (on success),
    /// then clears the active request. Ignored if session was closed or request_id doesn't match.
    void commit_result(std::string_view session_id, std::uint64_t request_id,
                       const ChatMessage& user_message,
                       const RuntimeResult<CompletionResult>& result);

    /// Clears the active request without modifying history. Used by CompletionLease cleanup.
    void release_request(std::string_view session_id, std::uint64_t request_id);

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
    void touch_session(SessionState& session);

    std::string model_id_;
    SessionConfig config_;
    std::string base_system_prompt_;
    size_t max_history_messages_ = 0;
    Clock clock_;
    std::atomic<std::uint64_t> next_session_id_{1};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionState>, TransparentStringHash,
                       TransparentStringEqual>
        sessions_;
    bool stopping_ = false;
};

// Backward compatibility alias during transition
using SessionManager = SessionStore;

} // namespace zks::server
