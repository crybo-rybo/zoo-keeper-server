#include "server/zoo_adapter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

TEST(ZooAdapterTest, RoleRoundTrip) {
    EXPECT_EQ(
        zks::server::from_zoo_role(zks::server::to_zoo_role(zks::server::MessageRole::System)),
        zks::server::MessageRole::System);
    EXPECT_EQ(zks::server::from_zoo_role(zks::server::to_zoo_role(zks::server::MessageRole::User)),
              zks::server::MessageRole::User);
    EXPECT_EQ(
        zks::server::from_zoo_role(zks::server::to_zoo_role(zks::server::MessageRole::Assistant)),
        zks::server::MessageRole::Assistant);
    EXPECT_EQ(zks::server::from_zoo_role(zks::server::to_zoo_role(zks::server::MessageRole::Tool)),
              zks::server::MessageRole::Tool);
}

TEST(ZooAdapterTest, SystemMessageRoundTrip) {
    auto msg = zks::server::ChatMessage::system("prompt");
    auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
    EXPECT_EQ(converted, msg);
}

TEST(ZooAdapterTest, UserMessageRoundTrip) {
    auto msg = zks::server::ChatMessage::user("hello");
    auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
    EXPECT_EQ(converted, msg);
}

TEST(ZooAdapterTest, AssistantMessageRoundTrip) {
    auto msg = zks::server::ChatMessage::assistant("reply");
    auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
    EXPECT_EQ(converted, msg);
}

TEST(ZooAdapterTest, ToolMessageRoundTrip) {
    auto msg = zks::server::ChatMessage::tool("result", "call-42");
    auto converted = zks::server::from_zoo_message(zks::server::to_zoo_message(msg));
    EXPECT_EQ(converted, msg);
    EXPECT_EQ(converted.tool_call_id, "call-42");
}

TEST(ZooAdapterTest, BatchMessageConversion) {
    std::vector<zks::server::ChatMessage> messages = {
        zks::server::ChatMessage::system("sys"),
        zks::server::ChatMessage::user("hi"),
        zks::server::ChatMessage::assistant("hey"),
        zks::server::ChatMessage::tool("result", "call-1"),
    };

    auto zoo_messages = zks::server::to_zoo_messages(messages);
    ASSERT_EQ(zoo_messages.size(), messages.size());

    for (size_t i = 0; i < messages.size(); ++i) {
        auto converted = zks::server::from_zoo_message(zoo_messages[i]);
        EXPECT_EQ(converted, messages[i]);
    }
}

TEST(ZooAdapterTest, ErrorCodeRoundTrip) {
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
        EXPECT_EQ(zks::server::from_zoo_error_code(zks::server::to_zoo_error_code(code)), code);
    }

    // RuntimeFailure -> zoo::Unknown -> RuntimeFailure
    EXPECT_EQ(zks::server::from_zoo_error_code(
                  zks::server::to_zoo_error_code(zks::server::RuntimeErrorCode::RuntimeFailure)),
              zks::server::RuntimeErrorCode::RuntimeFailure);
}

TEST(ZooAdapterTest, ErrorStructRoundTrip) {
    zks::server::RuntimeError original{
        zks::server::RuntimeErrorCode::InferenceFailed,
        "boom",
        "ctx",
    };
    auto converted = zks::server::from_zoo_error(zks::server::to_zoo_error(original));
    EXPECT_EQ(converted.code, original.code);
    EXPECT_EQ(converted.message, original.message);
    EXPECT_EQ(converted.context, original.context);
}

TEST(ZooAdapterTest, ErrorStructWithoutContext) {
    zks::server::RuntimeError original{
        zks::server::RuntimeErrorCode::QueueFull,
        "full",
        std::nullopt,
    };
    auto converted = zks::server::from_zoo_error(zks::server::to_zoo_error(original));
    EXPECT_EQ(converted.code, original.code);
    EXPECT_EQ(converted.message, original.message);
    EXPECT_FALSE(converted.context.has_value());
}

TEST(ZooAdapterTest, ToolInvocationStatusRoundTrip) {
    EXPECT_EQ(zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::Succeeded),
              zks::server::ToolInvocationStatus::Succeeded);
    EXPECT_EQ(
        zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::ValidationFailed),
        zks::server::ToolInvocationStatus::ValidationFailed);
    EXPECT_EQ(
        zks::server::from_zoo_tool_invocation_status(zoo::ToolInvocationStatus::ExecutionFailed),
        zks::server::ToolInvocationStatus::ExecutionFailed);
}

TEST(ZooAdapterTest, ToolMetadataConversion) {
    zoo::tools::ToolMetadata metadata;
    metadata.name = "search";
    metadata.description = "Search docs";
    metadata.parameters_schema = {{"type", "object"},
                                  {"properties", {{"query", {{"type", "string"}}}}}};

    auto converted = zks::server::from_zoo_tool_metadata(metadata);
    EXPECT_EQ(converted.name, "search");
    EXPECT_EQ(converted.description, "Search docs");
    EXPECT_EQ(converted.parameters_schema, metadata.parameters_schema);

    auto vec = zks::server::from_zoo_tool_metadata(std::vector{metadata});
    ASSERT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0].name, "search");
}

TEST(ZooAdapterTest, FromZooResponsePreservesAllFields) {
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
    EXPECT_EQ(converted.text, "hello world");
    EXPECT_EQ(converted.usage.prompt_tokens, 10);
    EXPECT_EQ(converted.usage.completion_tokens, 5);
    EXPECT_EQ(converted.usage.total_tokens, 15);
    EXPECT_EQ(converted.metrics.latency_ms, std::chrono::milliseconds{500});
    EXPECT_EQ(converted.metrics.time_to_first_token_ms, std::chrono::milliseconds{100});
    EXPECT_EQ(converted.metrics.tokens_per_second, 25.0);
    ASSERT_EQ(converted.tool_invocations.size(), 1u);
    EXPECT_EQ(converted.tool_invocations[0].id, "call-1");
    EXPECT_EQ(converted.tool_invocations[0].name, "search");
    EXPECT_EQ(converted.tool_invocations[0].arguments_json, R"({"query":"test"})");
    EXPECT_EQ(converted.tool_invocations[0].status, zks::server::ToolInvocationStatus::Succeeded);
    EXPECT_EQ(converted.tool_invocations[0].result_json, R"({"answer":"42"})");
    EXPECT_FALSE(converted.tool_invocations[0].error.has_value());
}
