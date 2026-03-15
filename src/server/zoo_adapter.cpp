#include "server/zoo_adapter.hpp"

#include <utility>

namespace zks::server {

MessageRole from_zoo_role(zoo::Role role) noexcept {
    switch (role) {
    case zoo::Role::System:
        return MessageRole::System;
    case zoo::Role::User:
        return MessageRole::User;
    case zoo::Role::Assistant:
        return MessageRole::Assistant;
    case zoo::Role::Tool:
        return MessageRole::Tool;
    }
    return MessageRole::User;
}

zoo::Role to_zoo_role(MessageRole role) noexcept {
    switch (role) {
    case MessageRole::System:
        return zoo::Role::System;
    case MessageRole::User:
        return zoo::Role::User;
    case MessageRole::Assistant:
        return zoo::Role::Assistant;
    case MessageRole::Tool:
        return zoo::Role::Tool;
    }
    return zoo::Role::User;
}

ChatMessage from_zoo_message(const zoo::Message& message) {
    switch (message.role) {
    case zoo::Role::System:
        return ChatMessage::system(message.content);
    case zoo::Role::User:
        return ChatMessage::user(message.content);
    case zoo::Role::Assistant:
        return ChatMessage::assistant(message.content);
    case zoo::Role::Tool:
        return ChatMessage::tool(message.content, message.tool_call_id.value_or(""));
    }

    return ChatMessage::user(message.content);
}

zoo::Message to_zoo_message(const ChatMessage& message) {
    switch (message.role) {
    case MessageRole::System:
        return zoo::Message::system(message.content);
    case MessageRole::User:
        return zoo::Message::user(message.content);
    case MessageRole::Assistant:
        return zoo::Message::assistant(message.content);
    case MessageRole::Tool:
        return zoo::Message::tool(message.content, message.tool_call_id.value_or(""));
    }

    return zoo::Message::user(message.content);
}

std::vector<zoo::Message> to_zoo_messages(const std::vector<ChatMessage>& messages) {
    std::vector<zoo::Message> converted;
    converted.reserve(messages.size());
    for (const auto& message : messages) {
        converted.push_back(to_zoo_message(message));
    }
    return converted;
}

RuntimeErrorCode from_zoo_error_code(zoo::ErrorCode code) noexcept {
    switch (code) {
    case zoo::ErrorCode::InvalidConfig:
        return RuntimeErrorCode::InvalidConfig;
    case zoo::ErrorCode::InvalidSamplingParams:
        return RuntimeErrorCode::InvalidSamplingParams;
    case zoo::ErrorCode::InvalidModelPath:
        return RuntimeErrorCode::InvalidModelPath;
    case zoo::ErrorCode::InvalidContextSize:
        return RuntimeErrorCode::InvalidContextSize;
    case zoo::ErrorCode::BackendInitFailed:
        return RuntimeErrorCode::BackendInitFailed;
    case zoo::ErrorCode::ModelLoadFailed:
        return RuntimeErrorCode::ModelLoadFailed;
    case zoo::ErrorCode::ContextCreationFailed:
        return RuntimeErrorCode::ContextCreationFailed;
    case zoo::ErrorCode::InferenceFailed:
        return RuntimeErrorCode::InferenceFailed;
    case zoo::ErrorCode::TokenizationFailed:
        return RuntimeErrorCode::TokenizationFailed;
    case zoo::ErrorCode::ContextWindowExceeded:
        return RuntimeErrorCode::ContextWindowExceeded;
    case zoo::ErrorCode::InvalidMessageSequence:
        return RuntimeErrorCode::InvalidMessageSequence;
    case zoo::ErrorCode::TemplateRenderFailed:
        return RuntimeErrorCode::TemplateRenderFailed;
    case zoo::ErrorCode::AgentNotRunning:
        return RuntimeErrorCode::AgentNotRunning;
    case zoo::ErrorCode::RequestCancelled:
        return RuntimeErrorCode::RequestCancelled;
    case zoo::ErrorCode::RequestTimeout:
        return RuntimeErrorCode::RequestTimeout;
    case zoo::ErrorCode::QueueFull:
        return RuntimeErrorCode::QueueFull;
    case zoo::ErrorCode::ToolNotFound:
        return RuntimeErrorCode::ToolNotFound;
    case zoo::ErrorCode::ToolExecutionFailed:
        return RuntimeErrorCode::ToolExecutionFailed;
    case zoo::ErrorCode::InvalidToolSignature:
        return RuntimeErrorCode::InvalidToolSignature;
    case zoo::ErrorCode::InvalidToolSchema:
        return RuntimeErrorCode::InvalidToolSchema;
    case zoo::ErrorCode::ToolValidationFailed:
        return RuntimeErrorCode::ToolValidationFailed;
    case zoo::ErrorCode::ToolRetriesExhausted:
        return RuntimeErrorCode::ToolRetriesExhausted;
    case zoo::ErrorCode::ToolLoopLimitReached:
        return RuntimeErrorCode::ToolLoopLimitReached;
    case zoo::ErrorCode::Unknown:
        return RuntimeErrorCode::RuntimeFailure;
    }
    return RuntimeErrorCode::RuntimeFailure;
}

zoo::ErrorCode to_zoo_error_code(RuntimeErrorCode code) noexcept {
    switch (code) {
    case RuntimeErrorCode::InvalidConfig:
        return zoo::ErrorCode::InvalidConfig;
    case RuntimeErrorCode::InvalidSamplingParams:
        return zoo::ErrorCode::InvalidSamplingParams;
    case RuntimeErrorCode::InvalidModelPath:
        return zoo::ErrorCode::InvalidModelPath;
    case RuntimeErrorCode::InvalidContextSize:
        return zoo::ErrorCode::InvalidContextSize;
    case RuntimeErrorCode::BackendInitFailed:
        return zoo::ErrorCode::BackendInitFailed;
    case RuntimeErrorCode::ModelLoadFailed:
        return zoo::ErrorCode::ModelLoadFailed;
    case RuntimeErrorCode::ContextCreationFailed:
        return zoo::ErrorCode::ContextCreationFailed;
    case RuntimeErrorCode::InferenceFailed:
        return zoo::ErrorCode::InferenceFailed;
    case RuntimeErrorCode::TokenizationFailed:
        return zoo::ErrorCode::TokenizationFailed;
    case RuntimeErrorCode::ContextWindowExceeded:
        return zoo::ErrorCode::ContextWindowExceeded;
    case RuntimeErrorCode::InvalidMessageSequence:
        return zoo::ErrorCode::InvalidMessageSequence;
    case RuntimeErrorCode::TemplateRenderFailed:
        return zoo::ErrorCode::TemplateRenderFailed;
    case RuntimeErrorCode::AgentNotRunning:
        return zoo::ErrorCode::AgentNotRunning;
    case RuntimeErrorCode::RequestCancelled:
        return zoo::ErrorCode::RequestCancelled;
    case RuntimeErrorCode::RequestTimeout:
        return zoo::ErrorCode::RequestTimeout;
    case RuntimeErrorCode::QueueFull:
        return zoo::ErrorCode::QueueFull;
    case RuntimeErrorCode::ToolNotFound:
        return zoo::ErrorCode::ToolNotFound;
    case RuntimeErrorCode::ToolExecutionFailed:
        return zoo::ErrorCode::ToolExecutionFailed;
    case RuntimeErrorCode::InvalidToolSignature:
        return zoo::ErrorCode::InvalidToolSignature;
    case RuntimeErrorCode::InvalidToolSchema:
        return zoo::ErrorCode::InvalidToolSchema;
    case RuntimeErrorCode::ToolValidationFailed:
        return zoo::ErrorCode::ToolValidationFailed;
    case RuntimeErrorCode::ToolRetriesExhausted:
        return zoo::ErrorCode::ToolRetriesExhausted;
    case RuntimeErrorCode::ToolLoopLimitReached:
        return zoo::ErrorCode::ToolLoopLimitReached;
    case RuntimeErrorCode::RuntimeFailure:
        break;
    }
    return zoo::ErrorCode::Unknown;
}

RuntimeError from_zoo_error(const zoo::Error& error) {
    return RuntimeError{from_zoo_error_code(error.code), error.message, error.context};
}

zoo::Error to_zoo_error(const RuntimeError& error) {
    return zoo::Error{to_zoo_error_code(error.code), error.message, error.context};
}

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

ToolInvocationStatus from_zoo_tool_invocation_status(zoo::ToolInvocationStatus status) noexcept {
    switch (status) {
    case zoo::ToolInvocationStatus::Succeeded:
        return ToolInvocationStatus::Succeeded;
    case zoo::ToolInvocationStatus::ValidationFailed:
        return ToolInvocationStatus::ValidationFailed;
    case zoo::ToolInvocationStatus::ExecutionFailed:
        return ToolInvocationStatus::ExecutionFailed;
    }
    return ToolInvocationStatus::ExecutionFailed;
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
    converted.tool_invocations.reserve(response.tool_invocations.size());

    for (const auto& invocation : response.tool_invocations) {
        ToolInvocationRecord converted_invocation;
        converted_invocation.id = invocation.id;
        converted_invocation.name = invocation.name;
        converted_invocation.arguments_json = invocation.arguments_json;
        converted_invocation.status = from_zoo_tool_invocation_status(invocation.status);
        converted_invocation.result_json = invocation.result_json;
        if (invocation.error.has_value()) {
            converted_invocation.error = from_zoo_error(*invocation.error);
        }
        converted.tool_invocations.push_back(std::move(converted_invocation));
    }

    return converted;
}

} // namespace zks::server
