#include "doctest.h"

#include "server/api_json.hpp"
#include "server/streaming.hpp"

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json parse_body(const drogon::HttpResponsePtr& response) {
    return nlohmann::json::parse(std::string(response->getBody()));
}

} // namespace

TEST_CASE("parse valid chat completion request") {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","session_id":"sess-1","messages":[{"role":"system","content":"Be brief."},{"role":"user","content":"Hello"}],"stream":true})json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->model == "local-model");
    CHECK(parsed->stream);
    CHECK(parsed->messages.size() == 2);
    CHECK(parsed->session_id == std::optional<std::string>{"sess-1"});
    CHECK(parsed->messages[0].role == zks::server::MessageRole::System);
    CHECK(parsed->messages[1].role == zks::server::MessageRole::User);
}

TEST_CASE("unknown request field rejected") {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","messages":[{"role":"user","content":"Hello"}],"temperature":0.2})json");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().http_status == 400);
    CHECK(parsed.error().code == std::optional<std::string>{"unknown_field"});
}

TEST_CASE("tool message without tool_call_id rejected") {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","messages":[{"role":"tool","content":"42"}]})json");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().http_status == 400);
    CHECK(parsed.error().param == std::optional<std::string>{"messages"});
}

TEST_CASE("parse valid session create request") {
    auto parsed = zks::server::parse_session_create_request(
        R"json({"model":"local-model","system_prompt":"Be concise."})json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->model == "local-model");
    CHECK(parsed->system_prompt == std::optional<std::string>{"Be concise."});
}

TEST_CASE("models response") {
    auto response = zks::server::make_models_response("local-model");
    const auto json = parse_body(response);
    CHECK(response->getStatusCode() == drogon::k200OK);
    CHECK(json.at("object") == "list");
    CHECK(json.at("data").size() == 1);
    CHECK(json.at("data")[0].at("id") == "local-model");
}

TEST_CASE("tools response") {
    zks::server::ToolDefinition tool;
    tool.name = "search_documents";
    tool.description = "Search local documents.";
    tool.parameters_schema = {{"type", "object"},
                              {"properties", {{"query", {{"type", "string"}}}}},
                              {"required", nlohmann::json::array({"query"})}};

    auto response = zks::server::make_tools_response({tool});
    const auto json = parse_body(response);
    CHECK(response->getStatusCode() == drogon::k200OK);
    CHECK(json.at("object") == "list");
    CHECK(json.at("data").size() == 1);
    CHECK(json.at("data")[0].at("function").at("name") == "search_documents");
}

TEST_CASE("session response") {
    const auto response = zks::server::make_session_response(
        zks::server::SessionSummary{"sess-1", "local-model", 10, 11, 20}, drogon::k201Created);
    const auto json = parse_body(response);
    CHECK(response->getStatusCode() == drogon::k201Created);
    CHECK(json.at("object") == "session");
    CHECK(json.at("id") == "sess-1");
    CHECK(json.at("model") == "local-model");
    CHECK(json.at("expires_at") == 20);
}

TEST_CASE("chat completion response") {
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
    CHECK(http_response->getStatusCode() == drogon::k200OK);
    CHECK(json.at("id") == "chatcmpl-1");
    CHECK(json.at("object") == "chat.completion");
    CHECK(json.at("model") == "local-model");
    CHECK(json.at("choices")[0].at("message").at("content") == "Hello from the server.");
    CHECK(json.at("usage").at("total_tokens") == 16);
    REQUIRE(json.contains("zoo_metrics"));
    CHECK(json.at("zoo_metrics").at("latency_ms") == 1240);
    CHECK(json.at("zoo_metrics").at("time_to_first_token_ms") == 180);
    CHECK(json.at("zoo_metrics").at("tokens_per_second") == 22.4);
}

TEST_CASE("runtime error to API error mapping") {
    auto queue_full = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::QueueFull, "queue full"});
    CHECK(queue_full.http_status == 503);
    CHECK(queue_full.type == "service_unavailable_error");
    CHECK(queue_full.code == std::optional<std::string>{"queue_full"});

    auto invalid_sequence = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::InvalidMessageSequence,
                                  "bad sequence"});
    CHECK(invalid_sequence.http_status == 400);
    CHECK(invalid_sequence.type == "invalid_request_error");

    auto model_load_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ModelLoadFailed,
                                  "model not found"});
    CHECK(model_load_failed.http_status == 500);
    CHECK(model_load_failed.type == "server_error");
    CHECK(model_load_failed.code == std::optional<std::string>{"model_load_failed"});

    auto inference_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::InferenceFailed,
                                  "decode failed"});
    CHECK(inference_failed.http_status == 500);
    CHECK(inference_failed.code == std::optional<std::string>{"inference_failed"});

    auto tool_not_found = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolNotFound, "unknown tool"});
    CHECK(tool_not_found.http_status == 400);
    CHECK(tool_not_found.type == "invalid_request_error");
    CHECK(tool_not_found.code == std::optional<std::string>{"tool_not_found"});

    auto tool_retries = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolRetriesExhausted,
                                  "retries exceeded"});
    CHECK(tool_retries.http_status == 500);
    CHECK(tool_retries.type == "server_error");
    CHECK(tool_retries.code == std::optional<std::string>{"tool_retries_exhausted"});

    auto template_failed = zks::server::map_runtime_error_to_api_error(
        zks::server::RuntimeError{zks::server::RuntimeErrorCode::TemplateRenderFailed,
                                  "render error"});
    CHECK(template_failed.http_status == 500);
    CHECK(template_failed.code == std::optional<std::string>{"template_render_failed"});

    auto missing_session = zks::server::not_found_error("missing", "session_not_found");
    CHECK(missing_session.http_status == 404);
    CHECK(missing_session.type == "not_found_error");

    auto busy = zks::server::conflict_error("busy", "session_busy");
    CHECK(busy.http_status == 409);
    CHECK(busy.type == "conflict_error");
}

TEST_CASE("chat completion response with tool invocations") {
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
    REQUIRE(json.contains("tool_invocations"));
    REQUIRE(json.at("tool_invocations").size() == 1);
    const auto& tool_inv = json.at("tool_invocations")[0];
    CHECK(tool_inv.at("id") == "call-1");
    CHECK(tool_inv.at("name") == "search_documents");
    CHECK(tool_inv.at("status") == "succeeded");
    CHECK(tool_inv.contains("result"));
    CHECK(tool_inv.contains("arguments"));
}

TEST_CASE("empty tool_invocations is an empty array") {
    zks::server::CompletionResult empty_response;
    auto http_empty = zks::server::make_chat_completion_response("chatcmpl-3", 1234567890,
                                                                 "local-model", empty_response);
    const auto json = parse_body(http_empty);
    REQUIRE(json.contains("tool_invocations"));
    CHECK(json.at("tool_invocations").is_array());
    CHECK(json.at("tool_invocations").size() == 0);
}

TEST_CASE("streaming chunk format") {
    const auto chunk = zks::server::make_chat_completion_chunk(
        "chatcmpl-1", 1234567890, "local-model", "Hello", true, std::nullopt);
    const auto finish = zks::server::make_chat_completion_chunk(
        "chatcmpl-1", 1234567890, "local-model", std::nullopt, false, "stop");
    const auto done = zks::server::make_sse_done();

    CHECK(chunk.find("data: ") == 0);
    CHECK(chunk.find("\"role\":\"assistant\"") != std::string::npos);
    CHECK(chunk.find("\"content\":\"Hello\"") != std::string::npos);
    CHECK(finish.find("\"finish_reason\":\"stop\"") != std::string::npos);
    CHECK(done == "data: [DONE]\n\n");
}
