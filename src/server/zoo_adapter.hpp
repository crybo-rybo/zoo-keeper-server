#pragma once

#include "server/api_types.hpp"

#include <vector>

#include <zoo/agent.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

[[nodiscard]] MessageRole from_zoo_role(zoo::Role role) noexcept;
[[nodiscard]] zoo::Role to_zoo_role(MessageRole role) noexcept;

[[nodiscard]] ChatMessage from_zoo_message(const zoo::Message& message);
[[nodiscard]] zoo::Message to_zoo_message(const ChatMessage& message);

[[nodiscard]] std::vector<zoo::Message> to_zoo_messages(const std::vector<ChatMessage>& messages);
[[nodiscard]] RuntimeErrorCode from_zoo_error_code(zoo::ErrorCode code) noexcept;
[[nodiscard]] zoo::ErrorCode to_zoo_error_code(RuntimeErrorCode code) noexcept;
[[nodiscard]] RuntimeError from_zoo_error(const zoo::Error& error);
[[nodiscard]] zoo::Error to_zoo_error(const RuntimeError& error);

[[nodiscard]] ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata);
[[nodiscard]] std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata);
[[nodiscard]] ToolInvocationStatus
from_zoo_tool_invocation_status(zoo::ToolInvocationStatus status) noexcept;
[[nodiscard]] CompletionResult from_zoo_response(const zoo::Response& response);

} // namespace zks::server
