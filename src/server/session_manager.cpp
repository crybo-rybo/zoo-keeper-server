#include "server/session_manager.hpp"

#include <chrono>
#include <iostream>
#include <utility>
#include <vector>

namespace zks::server {
namespace {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void log_session_event(std::string_view event, std::string_view session_id, std::string_view detail) {
    std::clog << "[session] event=" << event << " session_id=" << session_id
              << " detail=\"" << detail << "\"" << '\n';
}

} // namespace

SessionManager::SessionManager(std::string model_id, SessionConfig config, AgentFactory agent_factory)
    : model_id_(std::move(model_id)), config_(config), agent_factory_(std::move(agent_factory)) {}

bool SessionManager::enabled() const noexcept {
    return config_.enabled();
}

ApiError SessionManager::disabled_error() const {
    return invalid_request_error("Sessions are disabled for this server", "session_id",
                                 "sessions_disabled");
}

SessionHealth SessionManager::health() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return SessionHealth{enabled(), sessions_.size(), config_.max_sessions,
                         config_.idle_ttl_seconds};
}

SessionSummary SessionManager::snapshot_session(const SessionState& session) const {
    std::lock_guard<std::mutex> lock(session.mutex);
    return SessionSummary{session.id, session.model, session.created, session.last_used,
                          session.expires_at};
}

void SessionManager::reap_expired_sessions_locked(
    std::int64_t now, std::vector<std::shared_ptr<SessionState>>& expired) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto& session = it->second;
        bool should_expire = false;
        {
            std::lock_guard<std::mutex> session_lock(session->mutex);
            should_expire = !session->active_request.has_value() && session->expires_at <= now;
        }

        if (!should_expire) {
            ++it;
            continue;
        }

        expired.push_back(session);
        it = sessions_.erase(it);
    }
}

ApiResult<SessionSummary> SessionManager::create_session(const SessionCreateRequest& request) {
    if (!enabled()) {
        return std::unexpected(disabled_error());
    }
    if (request.model != model_id_) {
        return std::unexpected(
            invalid_request_error("Unknown model: " + request.model, "model", "invalid_model"));
    }

    std::vector<std::shared_ptr<SessionState>> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return std::unexpected(
                service_unavailable_error("Server runtime is stopping", "agent_not_ready"));
        }

        reap_expired_sessions_locked(now_seconds(), expired);
        if (sessions_.size() >= config_.max_sessions) {
            return std::unexpected(service_unavailable_error(
                "Session capacity reached", "session_capacity_reached"));
        }
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
        expired_session->agent->stop();
    }

    auto agent_result = agent_factory_(request.system_prompt);
    if (!agent_result) {
        return std::unexpected(server_error(agent_result.error(), "session_create_failed"));
    }

    auto created = now_seconds();
    auto expires_at = created + static_cast<std::int64_t>(config_.idle_ttl_seconds);
    auto session_id =
        "sess-" + std::to_string(next_session_id_.fetch_add(1, std::memory_order_relaxed));
    auto session = std::make_shared<SessionState>(session_id, model_id_, created, expires_at,
                                                  std::move(*agent_result));

    expired.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return std::unexpected(
                service_unavailable_error("Server runtime is stopping", "agent_not_ready"));
        }

        reap_expired_sessions_locked(created, expired);
        if (sessions_.size() >= config_.max_sessions) {
            return std::unexpected(service_unavailable_error(
                "Session capacity reached", "session_capacity_reached"));
        }
        sessions_.emplace(session_id, session);
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
        expired_session->agent->stop();
    }

    log_session_event("created", session_id, "session ready");
    return snapshot_session(*session);
}

ApiResult<SessionSummary> SessionManager::get_session(std::string_view session_id) {
    if (!enabled()) {
        return std::unexpected(disabled_error());
    }

    std::shared_ptr<SessionState> session;
    std::vector<std::shared_ptr<SessionState>> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reap_expired_sessions_locked(now_seconds(), expired);
        auto it = sessions_.find(std::string(session_id));
        if (it == sessions_.end()) {
            return std::unexpected(
                not_found_error("Unknown session: " + std::string(session_id), "session_not_found"));
        }
        session = it->second;
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
        expired_session->agent->stop();
    }

    return snapshot_session(*session);
}

ApiResult<void> SessionManager::delete_session(std::string_view session_id) {
    if (!enabled()) {
        return std::unexpected(disabled_error());
    }

    std::shared_ptr<SessionState> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(std::string(session_id));
        if (it == sessions_.end()) {
            return std::unexpected(
                not_found_error("Unknown session: " + std::string(session_id), "session_not_found"));
        }
        session = it->second;
        sessions_.erase(it);
    }

    std::optional<zoo::RequestId> request_id;
    {
        std::lock_guard<std::mutex> session_lock(session->mutex);
        request_id = session->active_request;
    }

    if (request_id.has_value()) {
        session->agent->cancel(*request_id);
    }
    session->agent->stop();
    log_session_event("deleted", session->id, "session removed");
    return {};
}

ApiResult<PendingChatCompletion> SessionManager::start_completion(
    const ChatCompletionRequest& request, std::atomic<std::uint64_t>& next_completion_id,
    std::optional<std::function<void(std::string_view)>> callback) {
    if (!enabled()) {
        return std::unexpected(disabled_error());
    }
    if (!request.session_id.has_value()) {
        return std::unexpected(invalid_request_error("session_id is required", "session_id"));
    }
    if (request.model != model_id_) {
        return std::unexpected(
            invalid_request_error("Unknown model: " + request.model, "model", "invalid_model"));
    }
    if (request.messages.size() != 1u || request.messages.front().role != zoo::Role::User) {
        return std::unexpected(invalid_request_error(
            "Session chat requests must contain exactly one user message", "messages",
            "invalid_message_sequence"));
    }

    std::shared_ptr<SessionState> session;
    std::vector<std::shared_ptr<SessionState>> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reap_expired_sessions_locked(now_seconds(), expired);
        auto it = sessions_.find(*request.session_id);
        if (it == sessions_.end()) {
            return std::unexpected(
                not_found_error("Unknown session: " + *request.session_id, "session_not_found"));
        }
        session = it->second;
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
        expired_session->agent->stop();
    }

    std::int64_t started_at = now_seconds();
    zoo::RequestHandle handle;
    {
        std::lock_guard<std::mutex> session_lock(session->mutex);
        if (session->active_request.has_value()) {
            return std::unexpected(
                conflict_error("Session already has an active request", "session_busy"));
        }

        handle = session->agent->chat(request.messages.front(), std::move(callback));
        session->active_request = handle.id;
        session->last_used = started_at;
        session->expires_at = started_at + static_cast<std::int64_t>(config_.idle_ttl_seconds);
    }

    const auto completion_id =
        "chatcmpl-" + std::to_string(next_completion_id.fetch_add(1, std::memory_order_relaxed));
    auto lease = std::make_shared<CompletionLease>(
        [this, session, request_id = handle.id] { finish_request(session, request_id); });

    return PendingChatCompletion{
        completion_id,
        started_at,
        model_id_,
        std::move(handle),
        [session, request_id = session->active_request.value()] { session->agent->cancel(request_id); },
        std::move(lease),
    };
}

void SessionManager::finish_request(const std::shared_ptr<SessionState>& session,
                                    zoo::RequestId request_id) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->active_request.has_value() || *session->active_request != request_id) {
        return;
    }

    session->active_request.reset();
    session->last_used = now_seconds();
    session->expires_at = session->last_used + static_cast<std::int64_t>(config_.idle_ttl_seconds);
}

void SessionManager::stop() {
    std::unordered_map<std::string, std::shared_ptr<SessionState>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        sessions.swap(sessions_);
    }

    for (auto& [id, session] : sessions) {
        std::optional<zoo::RequestId> request_id;
        {
            std::lock_guard<std::mutex> session_lock(session->mutex);
            request_id = session->active_request;
        }
        if (request_id.has_value()) {
            session->agent->cancel(*request_id);
        }
        session->agent->stop();
    }
}

} // namespace zks::server
