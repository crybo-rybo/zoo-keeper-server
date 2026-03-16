#include "server/api_types.hpp"

namespace zks::server {

std::string_view to_string(MessageRole role) noexcept {
    return zoo::role_to_string(role);
}

Result<void> validate_message_sequence(const std::vector<ChatMessage>& history,
                                       MessageRole next_role) {
    if (history.empty()) {
        if (next_role == MessageRole::Tool) {
            return std::unexpected("First message cannot be a tool response");
        }
        return {};
    }

    if (next_role == MessageRole::System) {
        return std::unexpected("System message only allowed at the beginning");
    }

    const MessageRole last_role = history.back().role;
    if (next_role == last_role && next_role != MessageRole::Tool) {
        return std::unexpected("Cannot have consecutive messages with the same role (except Tool)");
    }

    return {};
}

std::future_status CompletionHandle::wait_for(std::chrono::milliseconds timeout) const {
    if (!state) {
        return std::future_status::ready;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->consumed) {
        return std::future_status::ready;
    }
    return state->future.wait_for(timeout);
}

RuntimeResult<CompletionResult> CompletionHandle::get() {
    if (!state) {
        return std::unexpected(
            RuntimeError{RuntimeErrorCode::Unknown, "Completion handle is not ready"});
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->consumed) {
        return std::unexpected(RuntimeError{
            RuntimeErrorCode::Unknown,
            "Completion result was already consumed",
        });
    }
    state->consumed = true;
    return state->future.get();
}

CompletionHandle make_completion_handle(std::uint64_t id,
                                        std::future<RuntimeResult<CompletionResult>> future) {
    auto s = std::make_shared<CompletionState>();
    s->future = std::move(future);
    return CompletionHandle{id, std::move(s)};
}

} // namespace zks::server
