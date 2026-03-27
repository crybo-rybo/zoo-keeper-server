#pragma once

#include "server/api_types.hpp"

#include <vector>

#include <zoo/agent.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

/// Converts a zoo::TextResponse to a server CompletionResult.
/// This adapter exists because CompletionUsage uses int64_t while zoo::TokenUsage uses int.
[[nodiscard]] CompletionResult from_zoo_response(const zoo::TextResponse& response);

/// Merges per-request overrides from a ChatCompletionRequest onto default GenerationOptions.
[[nodiscard]] zoo::GenerationOptions merge_request_overrides(const zoo::GenerationOptions& defaults,
                                                             const ChatCompletionRequest& request);

[[nodiscard]] ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata);
[[nodiscard]] std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata);

} // namespace zks::server
