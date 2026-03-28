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

CompletionResult from_zoo_response(const zoo::TextResponse& response) {
    CompletionResult converted;
    converted.text = response.text;
    converted.usage.prompt_tokens = response.usage.prompt_tokens;
    converted.usage.completion_tokens = response.usage.completion_tokens;
    converted.usage.total_tokens = response.usage.total_tokens;
    converted.metrics.latency_ms = response.metrics.latency_ms;
    converted.metrics.time_to_first_token_ms = response.metrics.time_to_first_token_ms;
    converted.metrics.tokens_per_second = response.metrics.tokens_per_second;
    if (response.tool_trace) {
        converted.tool_invocations = response.tool_trace->invocations;
    }
    return converted;
}

ExtractionResult from_zoo_response(const zoo::ExtractionResponse& response) {
    ExtractionResult converted;
    converted.text = response.text;
    converted.data = response.data;
    converted.usage.prompt_tokens = response.usage.prompt_tokens;
    converted.usage.completion_tokens = response.usage.completion_tokens;
    converted.usage.total_tokens = response.usage.total_tokens;
    converted.metrics.latency_ms = response.metrics.latency_ms;
    converted.metrics.time_to_first_token_ms = response.metrics.time_to_first_token_ms;
    converted.metrics.tokens_per_second = response.metrics.tokens_per_second;
    if (response.tool_trace) {
        converted.tool_invocations = response.tool_trace->invocations;
    }
    return converted;
}

template <typename Request>
zoo::GenerationOptions merge_request_overrides_impl(const zoo::GenerationOptions& defaults,
                                                    const Request& request) {
    auto opts = defaults;
    if (request.temperature)
        opts.sampling.temperature = *request.temperature;
    if (request.top_p)
        opts.sampling.top_p = *request.top_p;
    if (request.top_k)
        opts.sampling.top_k = *request.top_k;
    if (request.repeat_penalty)
        opts.sampling.repeat_penalty = *request.repeat_penalty;
    if (request.repeat_last_n)
        opts.sampling.repeat_last_n = *request.repeat_last_n;
    if (request.max_tokens)
        opts.max_tokens = *request.max_tokens;
    if (request.seed)
        opts.sampling.seed = *request.seed;
    if (request.stop)
        opts.stop_sequences = *request.stop;
    if (request.record_tool_trace)
        opts.record_tool_trace = *request.record_tool_trace;
    return opts;
}

zoo::GenerationOptions merge_request_overrides(const zoo::GenerationOptions& defaults,
                                               const ChatCompletionRequest& request) {
    return merge_request_overrides_impl(defaults, request);
}

zoo::GenerationOptions merge_request_overrides(const zoo::GenerationOptions& defaults,
                                               const ExtractionRequest& request) {
    return merge_request_overrides_impl(defaults, request);
}

zoo::GenerationOptions merge_request_overrides(const zoo::GenerationOptions& defaults,
                                               const AgentChatRequest& request) {
    return merge_request_overrides_impl(defaults, request);
}

} // namespace zks::server
