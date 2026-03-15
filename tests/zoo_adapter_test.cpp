#include "doctest.h"

#include "server/zoo_adapter.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

TEST_CASE("role round-trip") {
    CHECK(zks::server::from_zoo_role(zks::server::to_zoo_role(
        zks::server::MessageRole::System)) == zks::server::MessageRole::System);
    CHECK(zks::server::from_zoo_role(zks::server::to_zoo_role(
        zks::server::MessageRole::User)) == zks::server::MessageRole::User);
    CHECK(zks::server::from_zoo_role(zks::server::to_zoo_role(
        zks::server::MessageRole::Assistant)) == zks::server::MessageRole::Assistant);
    CHECK(zks::server::from_zoo_role(zks::server::to_zoo_role(
        zks::server::MessageRole::Tool)) == zks::server::MessageRole::Tool);
}

TEST_CASE("message round-trip") {
    SUBCASE("system") {
        auto msg = zks::server::ChatMessage::system("prompt");
        auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
        CHECK(converted == msg);
    }

    SUBCASE("user") {
        auto msg = zks::server::ChatMessage::user("hello");
        auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
        CHECK(converted == msg);
    }

    SUBCASE("assistant") {
        auto msg = zks::server::ChatMessage::assistant("reply");
        auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
        CHECK(converted == msg);
    }

    SUBCASE("tool with call_id") {
        auto msg = zks::server::ChatMessage::tool("result", "call-42");
        auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
        CHECK(converted == msg);
        CHECK(converted.tool_call_id == "call-42");
    }
}

TEST_CASE("batch message conversion") {
    std::vector<zks::server::ChatMessage> messages = {
        zks::server::ChatMessage::system("sys"),
        zks::server::ChatMessage::user("hi"),
        zks::server::ChatMessage::assistant("hey"),
        zks::server::ChatMessage::tool("result", "call-1"),
    };

    auto zoo_messages = zks::server::to_zoo_messages(messages);
    CHECK(zoo_messages.size() == messages.size());

    for (size_t i = 0; i < messages.size(); ++i) {
        auto converted = zks::server::from_zoo_message(zoo_messages[i]);
        CHECK(converted == messages[i]);
    }
}

TEST_CASE("error code round-trip") {
    // All codes that map 1:1
    const zks::server::RuntimeErrorCode codes[] = {
        zks::server::RuntimeErrorCode::InvalidConfig,
        zks::server::RuntimeErrorCode::InvalidSamplingParams,
        zks::server::RuntimeErrorCode::InvalidModelPath,
        zks::server::RuntimeErrorCode::InvalidContextSize,
        zks::server::RuntimeErrorCode::BackendInitFailed,
        zks::server::RuntimeErrorCode::ModelLoadFailed,
        zks::server::RuntimeErrorCode::ContextCreationFailed,
        zks::server::RuntimeErrorCode::InferenceFailed,
        zks::server::RuntimeErrorCode::TokenizationFailed,
        zks::server::RuntimeErrorCode::ContextWindowExceeded,
        zks::server::RuntimeErrorCode::InvalidMessageSequence,
        zks::server::RuntimeErrorCode::TemplateRenderFailed,
        zks::server::RuntimeErrorCode::AgentNotRunning,
        zks::server::RuntimeErrorCode::RequestCancelled,
        zks::server::RuntimeErrorCode::RequestTimeout,
        zks::server::RuntimeErrorCode::QueueFull,
        zks::server::RuntimeErrorCode::ToolNotFound,
        zks::server::RuntimeErrorCode::ToolExecutionFailed,
        zks::server::RuntimeErrorCode::InvalidToolSignature,
        zks::server::RuntimeErrorCode::InvalidToolSchema,
        zks::server::RuntimeErrorCode::ToolValidationFailed,
        zks::server::RuntimeErrorCode::ToolRetriesExhausted,
        zks::server::RuntimeErrorCode::ToolLoopLimitReached,
    };

    for (auto code : codes) {
        CHECK(zks::server::from_zoo_error_code(zks::server::to_zoo_error_code(code)) == code);
    }

    // RuntimeFailure maps to zoo::Unknown and back to RuntimeFailure
    CHECK(zks::server::from_zoo_error_code(
        zks::server::to_zoo_error_code(zks::server::RuntimeErrorCode::RuntimeFailure)) ==
          zks::server::RuntimeErrorCode::RuntimeFailure);
}

TEST_CASE("error struct round-trip") {
    zks::server::RuntimeError original{
        zks::server::RuntimeErrorCode::InferenceFailed,
        "boom",
        "ctx",
    };
    auto converted = zks::server::from_zoo_error(zks::server::to_zoo_error(original));
    CHECK(converted.code == original.code);
    CHECK(converted.message == original.message);
    CHECK(converted.context == original.context);
}

TEST_CASE("error struct without context") {
    zks::server::RuntimeError original{
        zks::server::RuntimeErrorCode::QueueFull,
        "full",
        std::nullopt,
    };
    auto converted = zks::server::from_zoo_error(zks::server::to_zoo_error(original));
    CHECK(converted.code == original.code);
    CHECK(converted.message == original.message);
    CHECK_FALSE(converted.context.has_value());
}

TEST_CASE("tool invocation status round-trip") {
    CHECK(zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::Succeeded) ==
          zks::server::ToolInvocationStatus::Succeeded);
    CHECK(zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::ValidationFailed) ==
          zks::server::ToolInvocationStatus::ValidationFailed);
    CHECK(zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::ExecutionFailed) ==
          zks::server::ToolInvocationStatus::ExecutionFailed);
}

TEST_CASE("tool metadata conversion") {
    zoo::tools::ToolMetadata metadata;
    metadata.name = "search";
    metadata.description = "Search docs";
    metadata.parameters_schema = {{"type", "object"},
                                  {"properties", {{"query", {{"type", "string"}}}}}};

    auto converted = zks::server::from_zoo_tool_metadata(metadata);
    CHECK(converted.name == "search");
    CHECK(converted.description == "Search docs");
    CHECK(converted.parameters_schema == metadata.parameters_schema);

    // Vector overload
    auto vec = zks::server::from_zoo_tool_metadata(std::vector{metadata});
    CHECK(vec.size() == 1);
    CHECK(vec[0].name == "search");
}

TEST_CASE("from_zoo_response preserves all fields") {
    zoo::Response response;
    response.text = "hello world";
    response.usage.prompt_tokens = 10;
    response.usage.completion_tokens = 5;
    response.usage.total_tokens = 15;
    response.metrics.latency_ms = std::chrono::milliseconds{500};
    response.metrics.time_to_first_token_ms = std::chrono::milliseconds{100};
    response.metrics.tokens_per_second = 25.0;

    zoo::ToolInvocation invocation;
    invocation.id = "call-1";
    invocation.name = "search";
    invocation.arguments_json = R"({"query":"test"})";
    invocation.status = zoo::ToolInvocationStatus::Succeeded;
    invocation.result_json = R"({"answer":"42"})";
    response.tool_invocations.push_back(std::move(invocation));

    auto converted = zks::server::from_zoo_response(response);
    CHECK(converted.text == "hello world");
    CHECK(converted.usage.prompt_tokens == 10);
    CHECK(converted.usage.completion_tokens == 5);
    CHECK(converted.usage.total_tokens == 15);
    CHECK(converted.metrics.latency_ms == std::chrono::milliseconds{500});
    CHECK(converted.metrics.time_to_first_token_ms == std::chrono::milliseconds{100});
    CHECK(converted.metrics.tokens_per_second == 25.0);
    REQUIRE(converted.tool_invocations.size() == 1);
    CHECK(converted.tool_invocations[0].id == "call-1");
    CHECK(converted.tool_invocations[0].name == "search");
    CHECK(converted.tool_invocations[0].arguments_json == R"({"query":"test"})");
    CHECK(converted.tool_invocations[0].status == zks::server::ToolInvocationStatus::Succeeded);
    CHECK(converted.tool_invocations[0].result_json == R"({"answer":"42"})");
    CHECK_FALSE(converted.tool_invocations[0].error.has_value());
}
