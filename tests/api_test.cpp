#include "server/api_json.hpp"
#include "server/streaming.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json parse_body(const drogon::HttpResponsePtr& response) {
    return nlohmann::json::parse(std::string(response->getBody()));
}

} // namespace

TEST(ApiTest, ParseValidChatCompletionRequest) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","session_id":"sess-1","messages":[{"role":"system","content":"Be brief."},{"role":"user","content":"Hello"}],"stream":true})json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->model, "local-model");
    EXPECT_TRUE(parsed->stream);
    EXPECT_EQ(parsed->messages.size(), 2u);
    EXPECT_EQ(parsed->session_id, std::optional<std::string>{"sess-1"});
    EXPECT_EQ(parsed->messages[0].role, zks::server::MessageRole::System);
    EXPECT_EQ(parsed->messages[1].role, zks::server::MessageRole::User);
}

TEST(ApiTest, UnknownRequestFieldRejected) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","messages":[{"role":"user","content":"Hello"}],"temperature":0.2})json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().http_status, 400);
    EXPECT_EQ(parsed.error().code, std::optional<std::string>{"unknown_field"});
}

TEST(ApiTest, ToolMessageWithoutCallIdRejected) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","messages":[{"role":"tool","content":"42"}]})json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().http_status, 400);
    EXPECT_EQ(parsed.error().param, std::optional<std::string>{"messages"});
}

TEST(ApiTest, ParseValidSessionCreateRequest) {
    auto parsed = zks::server::parse_session_create_request(
        R"json({"model":"local-model","system_prompt":"Be concise."})json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->model, "local-model");
    EXPECT_EQ(parsed->system_prompt, std::optional<std::string>{"Be concise."});
}

TEST(ApiTest, ModelsResponse) {
    auto response = zks::server::make_models_response("local-model");
    const auto json = parse_body(response);
    EXPECT_EQ(response->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(json.at("object"), "list");
    EXPECT_EQ(json.at("data").size(), 1u);
    EXPECT_EQ(json.at("data")[0].at("id"), "local-model");
}

TEST(ApiTest, ToolsResponse) {
    zks::server::ToolDefinition tool;
    tool.name = "search_documents";
    tool.description = "Search local documents.";
    tool.parameters_schema = {{"type", "object"},
                              {"properties", {{"query", {{"type", "string"}}}}},
                              {"required", nlohmann::json::array({"query"})}};

    auto response = zks::server::make_tools_response({tool});
    const auto json = parse_body(response);
    EXPECT_EQ(response->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(json.at("object"), "list");
    EXPECT_EQ(json.at("data").size(), 1u);
    EXPECT_EQ(json.at("data")[0].at("function").at("name"), "search_documents");
}

TEST(ApiTest, SessionResponse) {
    const auto response = zks::server::make_session_response(
        zks::server::SessionSummary{"sess-1", "local-model", 10, 11, 20}, drogon::k201Created);
    const auto json = parse_body(response);
    EXPECT_EQ(response->getStatusCode(), drogon::k201Created);
    EXPECT_EQ(json.at("object"), "session");
    EXPECT_EQ(json.at("id"), "sess-1");
    EXPECT_EQ(json.at("model"), "local-model");
    EXPECT_EQ(json.at("expires_at"), 20);
}

TEST(ApiTest, ChatCompletionResponse) {
    zks::server::CompletionResult response;
    response.text = "Hello from the server.";
    response.usage.prompt_tokens = 12;
    response.usage.completion_tokens = 4;
    response.usage.total_tokens = 16;
    response.metrics.latency_ms = std::chrono::milliseconds{1240};
    response.metrics.time_to_first_token_ms = std::chrono::milliseconds{180};
    response.metrics.tokens_per_second = 22.4;

    auto http_response = zks::server::make_chat_completion_response("chatcmpl-1", 1234567890,
                                                                    "local-model", response);
    const auto json = parse_body(http_response);
    EXPECT_EQ(http_response->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(json.at("id"), "chatcmpl-1");
    EXPECT_EQ(json.at("object"), "chat.completion");
    EXPECT_EQ(json.at("model"), "local-model");
    EXPECT_EQ(json.at("choices")[0].at("message").at("content"), "Hello from the server.");
    EXPECT_EQ(json.at("usage").at("total_tokens"), 16);
    ASSERT_TRUE(json.contains("zoo_metrics"));
    EXPECT_EQ(json.at("zoo_metrics").at("latency_ms"), 1240);
    EXPECT_EQ(json.at("zoo_metrics").at("time_to_first_token_ms"), 180);
    EXPECT_EQ(json.at("zoo_metrics").at("tokens_per_second"), 22.4);
}

TEST(ApiTest, RuntimeErrorToApiErrorMapping) {
    auto queue_full = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::QueueFull, "queue full"});
    EXPECT_EQ(queue_full.http_status, 503);
    EXPECT_EQ(queue_full.type, "service_unavailable_error");
    EXPECT_EQ(queue_full.code, std::optional<std::string>{"queue_full"});

    auto invalid_sequence = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::InvalidMessageSequence,
                                  "bad sequence"});
    EXPECT_EQ(invalid_sequence.http_status, 400);
    EXPECT_EQ(invalid_sequence.type, "invalid_request_error");

    auto model_load_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ModelLoadFailed,
                                  "model not found"});
    EXPECT_EQ(model_load_failed.http_status, 500);
    EXPECT_EQ(model_load_failed.type, "server_error");
    EXPECT_EQ(model_load_failed.code, std::optional<std::string>{"model_load_failed"});

    auto inference_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::InferenceFailed,
                                  "decode failed"});
    EXPECT_EQ(inference_failed.http_status, 500);
    EXPECT_EQ(inference_failed.code, std::optional<std::string>{"inference_failed"});

    auto tool_not_found = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolNotFound, "unknown tool"});
    EXPECT_EQ(tool_not_found.http_status, 400);
    EXPECT_EQ(tool_not_found.type, "invalid_request_error");
    EXPECT_EQ(tool_not_found.code, std::optional<std::string>{"tool_not_found"});

    auto tool_retries = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolRetriesExhausted,
                                  "retries exceeded"});
    EXPECT_EQ(tool_retries.http_status, 500);
    EXPECT_EQ(tool_retries.type, "server_error");
    EXPECT_EQ(tool_retries.code, std::optional<std::string>{"tool_retries_exhausted"});

    auto template_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::TemplateRenderFailed,
                                  "render error"});
    EXPECT_EQ(template_failed.http_status, 500);
    EXPECT_EQ(template_failed.code, std::optional<std::string>{"template_render_failed"});

    auto missing_session = zks::server::not_found_error("missing", "session_not_found");
    EXPECT_EQ(missing_session.http_status, 404);
    EXPECT_EQ(missing_session.type, "not_found_error");

    auto busy = zks::server::conflict_error("busy", "session_busy");
    EXPECT_EQ(busy.http_status, 409);
    EXPECT_EQ(busy.type, "conflict_error");
}

TEST(ApiTest, ChatCompletionResponseWithToolInvocations) {
    zks::server::CompletionResult response;
    response.text = "I used a tool for that.";

    zks::server::ToolInvocationRecord inv;
    inv.id = "call-1";
    inv.name = "search_documents";
    inv.arguments_json = R"({"query":"test"})";
    inv.status = zks::server::ToolInvocationStatus::Succeeded;
    inv.result_json = R"({"results":[]})";
    response.tool_invocations.push_back(std::move(inv));

    auto http_response = zks::server::make_chat_completion_response("chatcmpl-2", 1234567890,
                                                                    "local-model", response);
    const auto json = parse_body(http_response);
    ASSERT_TRUE(json.contains("tool_invocations"));
    ASSERT_EQ(json.at("tool_invocations").size(), 1u);
    const auto& tool_inv = json.at("tool_invocations")[0];
    EXPECT_EQ(tool_inv.at("id"), "call-1");
    EXPECT_EQ(tool_inv.at("name"), "search_documents");
    EXPECT_EQ(tool_inv.at("status"), "succeeded");
    EXPECT_TRUE(tool_inv.contains("result"));
    EXPECT_TRUE(tool_inv.contains("arguments"));
}

TEST(ApiTest, EmptyToolInvocationsIsEmptyArray) {
    zks::server::CompletionResult empty_response;
    auto http_empty = zks::server::make_chat_completion_response("chatcmpl-3", 1234567890,
                                                                 "local-model", empty_response);
    const auto json = parse_body(http_empty);
    ASSERT_TRUE(json.contains("tool_invocations"));
    EXPECT_TRUE(json.at("tool_invocations").is_array());
    EXPECT_EQ(json.at("tool_invocations").size(), 0u);
}

TEST(ApiTest, StreamingChunkFormat) {
    const auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-1", 1234567890, "local-model", "Hello", true, std::nullopt);
    const auto finish = zks::server::make_chat_completion_chunk(
        "chatcmpl-1", 1234567890, "local-model", std::nullopt, false, "stop");
    const auto done = zks::server::make_sse_done();

    EXPECT_EQ(chunk.find("data: "), 0u);
    EXPECT_NE(chunk.find("\"role\":\"assistant\""), std::string::npos);
    EXPECT_NE(chunk.find("\"content\":\"Hello\""), std::string::npos);
    EXPECT_NE(finish.find("\"finish_reason\":\"stop\""), std::string::npos);
    EXPECT_EQ(done, "data: [DONE]\n\n");
}
