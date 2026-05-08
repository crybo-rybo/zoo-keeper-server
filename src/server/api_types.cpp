#include "server/api_types.hpp"

#include <span>

namespace zks::server {

std::string_view to_string(MessageRole role) noexcept {
    return zoo::role_to_string(role);
}

Result<void> validate_message_sequence(const std::vector<ChatMessage>& history,
                                       MessageRole next_role) {
    auto result = zoo::validate_role_sequence(std::span<const ChatMessage>(history), next_role);
    if (!result) {
        return std::unexpected(result.error().message);
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

std::future_status ExtractionHandle::wait_for(std::chrono::milliseconds timeout) const {
    if (!state) {
        return std::future_status::ready;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->consumed) {
        return std::future_status::ready;
    }
    return state->future.wait_for(timeout);
}

RuntimeResult<ExtractionResult> ExtractionHandle::get() {
    if (!state) {
        return std::unexpected(
            RuntimeError{RuntimeErrorCode::Unknown, "Extraction handle is not ready"});
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->consumed) {
        return std::unexpected(RuntimeError{
            RuntimeErrorCode::Unknown,
            "Extraction result was already consumed",
        });
    }
    state->consumed = true;
    return state->future.get();
}

ExtractionHandle make_extraction_handle(std::uint64_t id,
                                        std::future<RuntimeResult<ExtractionResult>> future) {
    auto s = std::make_shared<ExtractionState>();
    s->future = std::move(future);
    return ExtractionHandle{id, std::move(s)};
}

} // namespace zks::server
