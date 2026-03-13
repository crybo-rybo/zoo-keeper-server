#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <zoo/agent.hpp>

namespace zks::server {

class SessionManager {
  public:
    using AgentFactory =
        std::function<Result<std::unique_ptr<zoo::Agent>>(const std::optional<std::string>&)>;

    SessionManager(std::string model_id, SessionConfig config, AgentFactory agent_factory);

    [[nodiscard]] SessionHealth health() const noexcept;

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
                              std::unique_ptr<zoo::Agent> owned_agent)
            : id(std::move(session_id)), model(std::move(session_model)), created(created_at),
              last_used(created_at), expires_at(expires_at_value), agent(std::move(owned_agent)) {}

        std::string id;
        std::string model;
        std::int64_t created = 0;
        std::int64_t last_used = 0;
        std::int64_t expires_at = 0;
        std::unique_ptr<zoo::Agent> agent;
        std::optional<zoo::RequestId> active_request;
        mutable std::mutex mutex;
    };

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] ApiError disabled_error() const;
    [[nodiscard]] SessionSummary snapshot_session(const SessionState& session) const;

    void reap_expired_sessions_locked(std::int64_t now,
                                      std::vector<std::shared_ptr<SessionState>>& expired);
    void finish_request(const std::shared_ptr<SessionState>& session, zoo::RequestId request_id);

    std::string model_id_;
    SessionConfig config_;
    AgentFactory agent_factory_;
    std::atomic<std::uint64_t> next_session_id_{1};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionState>> sessions_;
    bool stopping_ = false;
};

} // namespace zks::server
