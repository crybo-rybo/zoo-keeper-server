#include "server/session_manager.hpp"

#include "server/internal_utils.hpp"

#include <cstddef>
#include <iostream>
#include <utility>
#include <vector>

namespace zks::server {
namespace {

std::vector<ChatMessage> make_initial_history(const std::string& base_prompt,
                                              const std::optional<std::string>& request_prompt) {
    const std::string effective_prompt =
        combine_system_prompts(base_prompt, request_prompt.value_or(""));
    if (effective_prompt.empty()) {
        return {};
    }
    return {ChatMessage::system(effective_prompt)};
}

void trim_history_to_fit(std::vector<ChatMessage>& messages, size_t max_history_messages) {
    const size_t system_offset =
        (!messages.empty() && messages.front().role == MessageRole::System) ? 1u : 0u;

    if (messages.size() <= system_offset + max_history_messages) {
        return;
    }

    size_t erase_end = messages.size() - max_history_messages;
    if (erase_end < system_offset) {
        erase_end = system_offset;
    }

    while (erase_end < messages.size() && messages[erase_end].role != MessageRole::User) {
        ++erase_end;
    }

    if (erase_end <= system_offset) {
        return;
    }

    messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(system_offset),
                   messages.begin() + static_cast<std::ptrdiff_t>(erase_end));
}

void log_session_event(std::string_view event, std::string_view session_id,
                       std::string_view detail) {
    std::clog << "[session] event=" << event << " session_id=" << session_id
              << " detail=\"" << detail << "\"" << '\n';
}

} // namespace

SessionManager::SessionManager(std::string model_id, SessionConfig config,
                               std::string base_system_prompt, size_t max_history_messages,
                               CompletionStarter completion_starter,
                               RequestCanceler request_canceler)
    : model_id_(std::move(model_id)), config_(config),
      base_system_prompt_(std::move(base_system_prompt)),
      max_history_messages_(max_history_messages),
      completion_starter_(std::move(completion_starter)),
      request_canceler_(std::move(request_canceler)) {}

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
            should_expire =
                !session->closed && !session->active_request.has_value() && session->expires_at <= now;
            if (should_expire) {
                session->closed = true;
            }
        }

        if (!should_expire) {
            ++it;
            continue;
        }

        expired.push_back(session);
        it = sessions_.erase(it);
    }
}

void SessionManager::reap_expired_sessions() {
    std::vector<std::shared_ptr<SessionState>> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        reap_expired_sessions_locked(now_seconds(), expired);
    }

    for (const auto& session : expired) {
        log_session_event("expired", session->id, "idle timeout reached");
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
    std::shared_ptr<SessionState> session;
    std::string session_id;
    bool capacity_reached = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return std::unexpected(
                service_unavailable_error("Server runtime is stopping", "agent_not_ready"));
        }

        const auto now = now_seconds();
        reap_expired_sessions_locked(now, expired);

        if (sessions_.size() >= config_.max_sessions) {
            capacity_reached = true;
        } else {
            session_id =
                "sess-" + std::to_string(next_session_id_.fetch_add(1, std::memory_order_relaxed));
            const auto expires_at = now + static_cast<std::int64_t>(config_.idle_ttl_seconds);
            session = std::make_shared<SessionState>(
                session_id, model_id_, now, expires_at,
                make_initial_history(base_system_prompt_, request.system_prompt));
            sessions_.emplace(session_id, session);
        }
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
    }

    if (capacity_reached) {
        return std::unexpected(service_unavailable_error(
            "Session capacity reached", "session_capacity_reached"));
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
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::unexpected(
                not_found_error("Unknown session: " + std::string(session_id), "session_not_found"));
        }
        session = it->second;
    }

    for (const auto& expired_session : expired) {
        log_session_event("expired", expired_session->id, "idle timeout reached");
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
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::unexpected(
                not_found_error("Unknown session: " + std::string(session_id), "session_not_found"));
        }
        session = it->second;
        sessions_.erase(it);
    }

    std::optional<std::uint64_t> request_id;
    {
        std::lock_guard<std::mutex> session_lock(session->mutex);
        session->closed = true;
        request_id = session->active_request;
    }

    if (request_id.has_value()) {
        request_canceler_(*request_id);
    }
    log_session_event("deleted", session->id, "session removed");
    return {};
}

ApiResult<PendingChatCompletion> SessionManager::start_completion(
    const ChatCompletionRequest& request, std::atomic<std::uint64_t>& next_completion_id,
    std::optional<TokenCallback> callback) {
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
    if (request.messages.size() != 1u || request.messages.front().role != MessageRole::User) {
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
    }

    const std::int64_t started_at = now_seconds();
    const ChatMessage user_message = request.messages.front();
    std::vector<ChatMessage> full_messages;
    CompletionHandle handle;
    {
        std::lock_guard<std::mutex> session_lock(session->mutex);
        if (session->closed) {
            return std::unexpected(
                not_found_error("Unknown session: " + *request.session_id, "session_not_found"));
        }
        if (session->active_request.has_value()) {
            return std::unexpected(
                conflict_error("Session already has an active request", "session_busy"));
        }

        full_messages.reserve(session->history.size() + 1);
        full_messages.insert(full_messages.end(), session->history.begin(), session->history.end());
        full_messages.push_back(user_message);
        handle = completion_starter_(std::move(full_messages), std::move(callback));
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
        [this, session, request_id = session->active_request.value(), user_message](
            const RuntimeResult<CompletionResult>& result) {
            finish_request(session, request_id, user_message, result);
        },
        [this, request_id = session->active_request.value()] { request_canceler_(request_id); },
        std::move(lease),
    };
}

void SessionManager::finish_request(const std::shared_ptr<SessionState>& session,
                                    std::uint64_t request_id) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->active_request.has_value() || *session->active_request != request_id) {
        return;
    }

    session->active_request.reset();
    session->last_used = now_seconds();
    session->expires_at = session->last_used + static_cast<std::int64_t>(config_.idle_ttl_seconds);
}

void SessionManager::finish_request(const std::shared_ptr<SessionState>& session,
                                    std::uint64_t request_id, const ChatMessage& user_message,
                                    const RuntimeResult<CompletionResult>& result) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->active_request.has_value() || *session->active_request != request_id) {
        return;
    }

    if (!session->closed && result) {
        session->history.push_back(user_message);
        session->history.push_back(ChatMessage::assistant(result->text));
        trim_history_to_fit(session->history, max_history_messages_);
    }

    session->active_request.reset();
    session->last_used = now_seconds();
    session->expires_at = session->last_used + static_cast<std::int64_t>(config_.idle_ttl_seconds);
}

void SessionManager::stop() {
    decltype(sessions_) sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        sessions.swap(sessions_);
    }

    for (auto& [id, session] : sessions) {
        std::optional<std::uint64_t> request_id;
        {
            std::lock_guard<std::mutex> session_lock(session->mutex);
            session->closed = true;
            request_id = session->active_request;
        }
        if (request_id.has_value()) {
            request_canceler_(*request_id);
        }
    }
}

} // namespace zks::server
