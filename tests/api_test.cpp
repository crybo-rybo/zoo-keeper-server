#include "server/api_json.hpp"
#include "server/streaming.hpp"

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>
#include <zoo/tools/types.hpp>

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
            R"json({"model":"local-model","messages":[{"role":"system","content":"Be brief."},{"role":"user","content":"Hello"}],"stream":true})json");
        if (!parsed) {
            return fail("Valid chat completion request unexpectedly failed.");
        }
        if (parsed->model != "local-model" || !parsed->stream || parsed->messages.size() != 2 ||
            parsed->messages[0].role != zoo::Role::System ||
            parsed->messages[1].role != zoo::Role::User) {
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
        auto response = zks::server::make_models_response("local-model");
        const auto json = parse_body(response);
        if (response->getStatusCode() != drogon::k200OK || json.at("object") != "list" ||
            json.at("data").size() != 1 || json.at("data")[0].at("id") != "local-model") {
            return fail("Models response body mismatch.");
        }
    }

    {
        zoo::tools::ToolMetadata tool;
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
        zoo::Response response;
        response.text = "Hello from the server.";
        response.usage.prompt_tokens = 12;
        response.usage.completion_tokens = 4;
        response.usage.total_tokens = 16;

        auto http_response = zks::server::make_chat_completion_response("chatcmpl-1", 1234567890,
                                                                        "local-model", response);
        const auto json = parse_body(http_response);
        if (http_response->getStatusCode() != drogon::k200OK || json.at("id") != "chatcmpl-1" ||
            json.at("object") != "chat.completion" || json.at("model") != "local-model" ||
            json.at("choices")[0].at("message").at("content") != "Hello from the server." ||
            json.at("usage").at("total_tokens") != 16) {
            return fail("Chat completion response body mismatch.");
        }
    }

    {
        const auto queue_full = zks::server::map_zoo_error_to_api_error(
            zoo::Error{zoo::ErrorCode::QueueFull, "queue full"});
        if (queue_full.http_status != 503 || queue_full.type != "service_unavailable_error") {
            return fail("QueueFull error mapped incorrectly.");
        }

        const auto invalid_sequence = zks::server::map_zoo_error_to_api_error(
            zoo::Error{zoo::ErrorCode::InvalidMessageSequence, "bad sequence"});
        if (invalid_sequence.http_status != 400 ||
            invalid_sequence.type != "invalid_request_error") {
            return fail("InvalidMessageSequence error mapped incorrectly.");
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
