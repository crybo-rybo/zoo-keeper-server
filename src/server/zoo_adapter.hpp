#pragma once

#include "server/api_types.hpp"

#include <vector>

#include <zoo/agent.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

/// Converts a zoo::Response to a server CompletionResult.
/// This adapter exists because CompletionUsage uses int64_t while zoo::TokenUsage uses int.
[[nodiscard]] CompletionResult from_zoo_response(const zoo::Response& response);

[[nodiscard]] ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata);
[[nodiscard]] std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata);

} // namespace zks::server
