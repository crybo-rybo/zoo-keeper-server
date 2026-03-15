#include "server/api_types.hpp"

namespace zks::server {

std::string_view to_string(MessageRole role) noexcept {
    switch (role) {
    case MessageRole::System:
        return "system";
    case MessageRole::User:
        return "user";
    case MessageRole::Assistant:
        return "assistant";
    case MessageRole::Tool:
        return "tool";
    }
    return "unknown";
}

ChatMessage ChatMessage::system(std::string content) {
    return ChatMessage{MessageRole::System, std::move(content), std::nullopt};
}

ChatMessage ChatMessage::user(std::string content) {
    return ChatMessage{MessageRole::User, std::move(content), std::nullopt};
}

ChatMessage ChatMessage::assistant(std::string content) {
    return ChatMessage{MessageRole::Assistant, std::move(content), std::nullopt};
}

ChatMessage ChatMessage::tool(std::string content, std::string tool_call_id) {
    return ChatMessage{MessageRole::Tool, std::move(content), std::move(tool_call_id)};
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

std::string_view to_string(ToolInvocationStatus status) noexcept {
    switch (status) {
    case ToolInvocationStatus::Succeeded:
        return "succeeded";
    case ToolInvocationStatus::ValidationFailed:
        return "validation_failed";
    case ToolInvocationStatus::ExecutionFailed:
        return "execution_failed";
    }
    return "unknown";
}

std::future_status CompletionHandle::wait_for(std::chrono::milliseconds timeout) const {
    if (!source) {
        return std::future_status::ready;
    }
    return source->wait_for(timeout);
}

RuntimeResult<CompletionResult> CompletionHandle::get() {
    if (!source) {
        return std::unexpected(
            RuntimeError{RuntimeErrorCode::RuntimeFailure, "Completion handle is not ready"});
    }
    return source->get();
}

} // namespace zks::server
