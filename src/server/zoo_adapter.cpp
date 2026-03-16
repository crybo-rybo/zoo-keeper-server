#include "server/zoo_adapter.hpp"

namespace zks::server {

ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata) {
    return ToolDefinition{metadata.name, metadata.description, metadata.parameters_schema};
}

std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata) {
    std::vector<ToolDefinition> definitions;
    definitions.reserve(metadata.size());
    for (const auto& entry : metadata) {
        definitions.push_back(from_zoo_tool_metadata(entry));
    }
    return definitions;
}

CompletionResult from_zoo_response(const zoo::Response& response) {
    CompletionResult converted;
    converted.text = response.text;
    converted.usage.prompt_tokens = response.usage.prompt_tokens;
    converted.usage.completion_tokens = response.usage.completion_tokens;
    converted.usage.total_tokens = response.usage.total_tokens;
    converted.metrics.latency_ms = response.metrics.latency_ms;
    converted.metrics.time_to_first_token_ms = response.metrics.time_to_first_token_ms;
    converted.metrics.tokens_per_second = response.metrics.tokens_per_second;
    converted.tool_invocations = response.tool_invocations;
    return converted;
}

} // namespace zks::server
