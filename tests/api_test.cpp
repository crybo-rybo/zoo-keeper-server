#include "server/api_json.hpp"
#include "server/streaming.hpp"

#include <chrono>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json parse_body(const drogon::HttpResponsePtr& response) {
    return nlohmann::json::parse(std::string(response->getBody()));
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    {
        auto parsed = zks::server::parse_chat_completion_request(
            R"json({"model":"local-model","session_id":"sess-1","messages":[{"role":"system","content":"Be brief."},{"role":"user","content":"Hello"}],"stream":true})json");
        if (!parsed) {
            return fail("Valid chat completion request unexpectedly failed.");
        }
        if (parsed->model != "local-model" || !parsed->stream || parsed->messages.size() != 2 ||
            parsed->session_id != std::optional<std::string>{"sess-1"} ||
            parsed->messages[0].role != zks::server::MessageRole::System ||
            parsed->messages[1].role != zks::server::MessageRole::User) {
            return fail("Valid chat completion request parsed with unexpected values.");
        }
    }

    {
        auto parsed = zks::server::parse_chat_completion_request(
            R"json({"model":"local-model","messages":[{"role":"user","content":"Hello"}],"temperature":0.2})json");
        if (parsed) {
            return fail("Unknown request field unexpectedly parsed successfully.");
        }
        if (parsed.error().http_status != 400 ||
            parsed.error().code != std::optional<std::string>{"unknown_field"}) {
            return fail("Unknown request field failed with the wrong error.");
        }
    }

    {
        auto parsed = zks::server::parse_chat_completion_request(
            R"json({"model":"local-model","messages":[{"role":"tool","content":"42"}]})json");
        if (parsed) {
            return fail("Tool message without tool_call_id unexpectedly parsed successfully.");
        }
        if (parsed.error().http_status != 400 ||
            parsed.error().param != std::optional<std::string>{"messages"}) {
            return fail("Tool message validation failed with the wrong error.");
        }
    }

    {
        auto parsed =
            zks::server::parse_session_create_request(
                R"json({"model":"local-model","system_prompt":"Be concise."})json");
        if (!parsed || parsed->model != "local-model" ||
            parsed->system_prompt != std::optional<std::string>{"Be concise."}) {
            return fail("Valid session create request parsed with unexpected values.");
        }
    }

    {
        auto response = zks::server::make_models_response("local-model");
        const auto json = parse_body(response);
        if (response->getStatusCode() != drogon::k200OK || json.at("object") != "list" ||
            json.at("data").size() != 1 || json.at("data")[0].at("id") != "local-model") {
            return fail("Models response body mismatch.");
        }
    }

    {
        zks::server::ToolDefinition tool;
        tool.name = "search_documents";
        tool.description = "Search local documents.";
        tool.parameters_schema = {{"type", "object"},
                                  {"properties", {{"query", {{"type", "string"}}}}},
                                  {"required", nlohmann::json::array({"query"})}};

        auto response = zks::server::make_tools_response({tool});
        const auto json = parse_body(response);
        if (response->getStatusCode() != drogon::k200OK || json.at("object") != "list" ||
            json.at("data").size() != 1 ||
            json.at("data")[0].at("function").at("name") != "search_documents") {
            return fail("Tools response body mismatch.");
        }
    }

    {
        const auto response = zks::server::make_session_response(
            zks::server::SessionSummary{"sess-1", "local-model", 10, 11, 20}, drogon::k201Created);
        const auto json = parse_body(response);
        if (response->getStatusCode() != drogon::k201Created || json.at("object") != "session" ||
            json.at("id") != "sess-1" || json.at("model") != "local-model" ||
            json.at("expires_at") != 20) {
            return fail("Session response body mismatch.");
        }
    }

    {
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
        if (http_response->getStatusCode() != drogon::k200OK || json.at("id") != "chatcmpl-1" ||
            json.at("object") != "chat.completion" || json.at("model") != "local-model" ||
            json.at("choices")[0].at("message").at("content") != "Hello from the server." ||
            json.at("usage").at("total_tokens") != 16) {
            return fail("Chat completion response body mismatch.");
        }
        if (!json.contains("zoo_metrics") ||
            json.at("zoo_metrics").at("latency_ms") != 1240 ||
            json.at("zoo_metrics").at("time_to_first_token_ms") != 180 ||
            json.at("zoo_metrics").at("tokens_per_second") != 22.4) {
            return fail("Chat completion zoo_metrics mismatch.");
        }
    }

    {
        const auto queue_full = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::QueueFull, "queue full"});
        if (queue_full.http_status != 503 || queue_full.type != "service_unavailable_error" ||
            queue_full.code != std::optional<std::string>{"queue_full"}) {
            return fail("QueueFull error mapped incorrectly.");
        }

        const auto invalid_sequence = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::InvalidMessageSequence,
                                      "bad sequence"});
        if (invalid_sequence.http_status != 400 ||
            invalid_sequence.type != "invalid_request_error") {
            return fail("InvalidMessageSequence error mapped incorrectly.");
        }

        const auto model_load_failed = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::ModelLoadFailed,
                                      "model not found"});
        if (model_load_failed.http_status != 500 || model_load_failed.type != "server_error" ||
            model_load_failed.code != std::optional<std::string>{"model_load_failed"}) {
            return fail("ModelLoadFailed error mapped incorrectly.");
        }

        const auto inference_failed = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::InferenceFailed,
                                      "decode failed"});
        if (inference_failed.http_status != 500 ||
            inference_failed.code != std::optional<std::string>{"inference_failed"}) {
            return fail("InferenceFailed error mapped incorrectly.");
        }

        const auto tool_not_found = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolNotFound,
                                      "unknown tool"});
        if (tool_not_found.http_status != 400 || tool_not_found.type != "invalid_request_error" ||
            tool_not_found.code != std::optional<std::string>{"tool_not_found"}) {
            return fail("ToolNotFound error mapped incorrectly.");
        }

        const auto tool_retries = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::ToolRetriesExhausted,
                                      "retries exceeded"});
        if (tool_retries.http_status != 500 || tool_retries.type != "server_error" ||
            tool_retries.code != std::optional<std::string>{"tool_retries_exhausted"}) {
            return fail("ToolRetriesExhausted error mapped incorrectly.");
        }

        const auto template_failed = zks::server::map_runtime_error_to_api_error(
            zks::server::RuntimeError{zks::server::RuntimeErrorCode::TemplateRenderFailed,
                                      "render error"});
        if (template_failed.http_status != 500 ||
            template_failed.code != std::optional<std::string>{"template_render_failed"}) {
            return fail("TemplateRenderFailed error mapped incorrectly.");
        }

        const auto missing_session =
            zks::server::not_found_error("missing", "session_not_found");
        if (missing_session.http_status != 404 || missing_session.type != "not_found_error") {
            return fail("not_found_error helper mapped incorrectly.");
        }

        const auto busy = zks::server::conflict_error("busy", "session_busy");
        if (busy.http_status != 409 || busy.type != "conflict_error") {
            return fail("conflict_error helper mapped incorrectly.");
        }
    }

    {
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
        if (!json.contains("tool_invocations") || json.at("tool_invocations").size() != 1) {
            return fail("tool_invocations missing or wrong size.");
        }
        const auto& tool_inv = json.at("tool_invocations")[0];
        if (tool_inv.at("id") != "call-1" || tool_inv.at("name") != "search_documents" ||
            tool_inv.at("status") != "succeeded" ||
            !tool_inv.contains("result") || !tool_inv.contains("arguments")) {
            return fail("tool_invocations content mismatch.");
        }
        if (!json.contains("tool_invocations") || json.at("tool_invocations").is_null()) {
            return fail("tool_invocations must be present even when empty.");
        }

        zks::server::CompletionResult empty_response;
        auto http_empty = zks::server::make_chat_completion_response("chatcmpl-3", 1234567890,
                                                                     "local-model", empty_response);
        const auto json_empty = parse_body(http_empty);
        if (!json_empty.contains("tool_invocations") ||
            !json_empty.at("tool_invocations").is_array() ||
            json_empty.at("tool_invocations").size() != 0) {
            return fail("Empty tool_invocations must be an empty array.");
        }
    }

    {
        const auto chunk = zks::server::make_chat_completion_chunk(
            "chatcmpl-1", 1234567890, "local-model", "Hello", true, std::nullopt);
        const auto finish = zks::server::make_chat_completion_chunk(
            "chatcmpl-1", 1234567890, "local-model", std::nullopt, false, "stop");
        const auto done = zks::server::make_sse_done();

        if (chunk.find("data: ") != 0 ||
            chunk.find("\"role\":\"assistant\"") == std::string::npos ||
            chunk.find("\"content\":\"Hello\"") == std::string::npos) {
            return fail("Streaming content chunk mismatch.");
        }
        if (finish.find("\"finish_reason\":\"stop\"") == std::string::npos) {
            return fail("Streaming finish chunk mismatch.");
        }
        if (done != "data: [DONE]\n\n") {
            return fail("Streaming done marker mismatch.");
        }
    }

    return 0;
}
