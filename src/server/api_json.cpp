#include "server/api_json.hpp"

#include "server/internal_utils.hpp"

#include <algorithm>
#include <array>
#include <string>

#include <zoo/core/types.hpp>

namespace zks::server {
namespace {

template <size_t N>
ApiResult<void> reject_api_unknown_keys(const nlohmann::json& json, const char* context,
                                        const std::array<const char*, N>& allowed_keys) {
    std::array<std::string_view, N> sv_keys;
    for (size_t i = 0; i < N; ++i) {
        sv_keys[i] = allowed_keys[i];
    }
    auto result = reject_unknown_keys(json, context, std::span<const std::string_view>(sv_keys));
    if (!result) {
        return std::unexpected(invalid_request_error(result.error(), context, "unknown_field"));
    }
    return {};
}

ApiResult<MessageRole> parse_role(std::string_view role) {
    if (role == "system") {
        return MessageRole::System;
    }
    if (role == "user") {
        return MessageRole::User;
    }
    if (role == "assistant") {
        return MessageRole::Assistant;
    }
    if (role == "tool") {
        return MessageRole::Tool;
    }

    return std::unexpected(invalid_request_error("Unsupported message role: " + std::string(role),
                                                 "messages", "invalid_role"));
}

ApiResult<ChatMessage> parse_message(const nlohmann::json& json, std::vector<ChatMessage>& seed) {
    static constexpr std::array<const char*, 3> kAllowedMessageKeys = {"role", "content",
                                                                       "tool_call_id"};
    if (auto unknown_keys = reject_api_unknown_keys(json, "message", kAllowedMessageKeys);
        !unknown_keys) {
        return std::unexpected(unknown_keys.error());
    }

    if (!json.contains("role") || !json.at("role").is_string()) {
        return std::unexpected(invalid_request_error("message.role must be a string", "messages"));
    }
    if (!json.contains("content") || !json.at("content").is_string()) {
        return std::unexpected(
            invalid_request_error("message.content must be a string", "messages"));
    }

    auto role = parse_role(json.at("role").get_ref<const std::string&>());
    if (!role) {
        return std::unexpected(role.error());
    }

    // Move content out of the JSON value to avoid a copy. The json object is
    // not used after this point for the content field.
    auto content = json.at("content").get<std::string>();
    ChatMessage message{MessageRole::User, {}, {}};

    if (*role == MessageRole::Tool) {
        if (!json.contains("tool_call_id") || !json.at("tool_call_id").is_string()) {
            return std::unexpected(invalid_request_error(
                "tool messages must include a string tool_call_id", "messages"));
        }
        message = ChatMessage::tool(std::move(content), json.at("tool_call_id").get<std::string>());
    } else {
        if (json.contains("tool_call_id")) {
            return std::unexpected(
                invalid_request_error("tool_call_id is only valid for tool messages", "messages"));
        }

        message.role = *role;
        message.content = std::move(content);
    }

    if (auto validation = validate_message_sequence(seed, message.role); !validation) {
        return std::unexpected(
            invalid_request_error(validation.error(), "messages", "invalid_message_sequence"));
    }
    seed.push_back(message);
    return message;
}

nlohmann::json make_tool_schema(const ToolDefinition& metadata) {
    return nlohmann::json{{"type", "function"},
                          {"function",
                           {{"name", metadata.name},
                            {"description", metadata.description},
                            {"parameters", metadata.parameters_schema}}}};
}

nlohmann::json make_tool_invocation_json(const ToolInvocationRecord& inv) {
    nlohmann::json j{{"id", inv.id}, {"name", inv.name}, {"status", zoo::to_string(inv.status)}};
    try {
        j["arguments"] = nlohmann::json::parse(inv.arguments_json);
    } catch (...) {
        j["arguments"] = inv.arguments_json;
    }
    if (inv.result_json.has_value()) {
        try {
            j["result"] = nlohmann::json::parse(*inv.result_json);
        } catch (...) {
            j["result"] = *inv.result_json;
        }
    }
    if (inv.error.has_value()) {
        j["error"] = inv.error->message;
    }
    return j;
}

nlohmann::json make_chat_completion_body(std::string_view completion_id, std::int64_t created,
                                         std::string_view model_id,
                                         const CompletionResult& response) {
    nlohmann::json tool_invocations = nlohmann::json::array();
    for (const auto& inv : response.tool_invocations) {
        tool_invocations.push_back(make_tool_invocation_json(inv));
    }

    return nlohmann::json{
        {"id", completion_id},
        {"object", "chat.completion"},
        {"created", created},
        {"model", model_id},
        {"choices",
         {{{"index", 0},
           {"message", {{"role", "assistant"}, {"content", response.text}}},
           {"finish_reason", "stop"}}}},
        {"usage",
         {{"prompt_tokens", response.usage.prompt_tokens},
          {"completion_tokens", response.usage.completion_tokens},
          {"total_tokens", response.usage.total_tokens}}},
        {"zoo_metrics",
         {{"latency_ms", response.metrics.latency_ms.count()},
          {"time_to_first_token_ms", response.metrics.time_to_first_token_ms.count()},
          {"tokens_per_second", response.metrics.tokens_per_second}}},
        {"tool_invocations", std::move(tool_invocations)}};
}

nlohmann::json make_session_body(const SessionSummary& summary) {
    return nlohmann::json{{"id", summary.id},
                          {"object", "session"},
                          {"model", summary.model},
                          {"created", summary.created},
                          {"last_used", summary.last_used},
                          {"expires_at", summary.expires_at}};
}

} // namespace

ApiResult<ChatCompletionRequest> parse_chat_completion_request(std::string_view body) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body.begin(), body.end());
    } catch (const std::exception& error) {
        return std::unexpected(
            invalid_request_error(std::string("Invalid JSON body: ") + error.what(), "body"));
    }

    static constexpr std::array<const char*, 11> kAllowedKeys = {
        "model", "messages",       "stream",     "session_id", "temperature", "top_p",
        "top_k", "repeat_penalty", "max_tokens", "seed",       "stop"};
    if (auto unknown_keys = reject_api_unknown_keys(json, "request", kAllowedKeys); !unknown_keys) {
        return std::unexpected(unknown_keys.error());
    }

    if (!json.contains("model") || !json.at("model").is_string()) {
        return std::unexpected(invalid_request_error("model must be a string", "model"));
    }
    if (!json.contains("messages") || !json.at("messages").is_array()) {
        return std::unexpected(invalid_request_error("messages must be an array", "messages"));
    }

    ChatCompletionRequest request;
    request.model = json.at("model").get<std::string>();
    if (request.model.empty()) {
        return std::unexpected(invalid_request_error("model must not be empty", "model"));
    }

    if (auto it = json.find("stream"); it != json.end()) {
        if (!it->is_boolean()) {
            return std::unexpected(invalid_request_error("stream must be a boolean", "stream"));
        }
        request.stream = it->get<bool>();
    }
    if (auto it = json.find("session_id"); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(
                invalid_request_error("session_id must be a string", "session_id"));
        }
        request.session_id = it->get<std::string>();
        if (request.session_id->empty()) {
            return std::unexpected(
                invalid_request_error("session_id must not be empty", "session_id"));
        }
    }

    // Per-request sampling overrides.
    if (auto it = json.find("temperature"); it != json.end()) {
        if (!it->is_number()) {
            return std::unexpected(
                invalid_request_error("temperature must be a number", "temperature"));
        }
        request.temperature = it->get<float>();
    }
    if (auto it = json.find("top_p"); it != json.end()) {
        if (!it->is_number()) {
            return std::unexpected(invalid_request_error("top_p must be a number", "top_p"));
        }
        request.top_p = it->get<float>();
    }
    if (auto it = json.find("top_k"); it != json.end()) {
        if (!it->is_number_integer()) {
            return std::unexpected(invalid_request_error("top_k must be an integer", "top_k"));
        }
        request.top_k = it->get<int>();
    }
    if (auto it = json.find("repeat_penalty"); it != json.end()) {
        if (!it->is_number()) {
            return std::unexpected(
                invalid_request_error("repeat_penalty must be a number", "repeat_penalty"));
        }
        request.repeat_penalty = it->get<float>();
    }
    if (auto it = json.find("max_tokens"); it != json.end()) {
        if (!it->is_number_integer()) {
            return std::unexpected(
                invalid_request_error("max_tokens must be an integer", "max_tokens"));
        }
        request.max_tokens = it->get<int>();
    }
    if (auto it = json.find("seed"); it != json.end()) {
        if (!it->is_number_integer()) {
            return std::unexpected(invalid_request_error("seed must be an integer", "seed"));
        }
        request.seed = it->get<int>();
    }
    if (auto it = json.find("stop"); it != json.end()) {
        if (!it->is_array()) {
            return std::unexpected(
                invalid_request_error("stop must be an array of strings", "stop"));
        }
        std::vector<std::string> stop_sequences;
        for (const auto& entry : *it) {
            if (!entry.is_string()) {
                return std::unexpected(
                    invalid_request_error("each stop entry must be a string", "stop"));
            }
            stop_sequences.push_back(entry.get<std::string>());
        }
        request.stop = std::move(stop_sequences);
    }

    const auto& messages_json = json.at("messages");
    if (messages_json.empty()) {
        return std::unexpected(
            invalid_request_error("messages must contain at least one item", "messages"));
    }

    request.messages.reserve(messages_json.size());
    for (const auto& item : messages_json) {
        auto parsed = parse_message(item, request.messages);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }

    return request;
}

ApiResult<SessionCreateRequest> parse_session_create_request(std::string_view body) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body.begin(), body.end());
    } catch (const std::exception& error) {
        return std::unexpected(
            invalid_request_error(std::string("Invalid JSON body: ") + error.what(), "body"));
    }

    static constexpr std::array<const char*, 2> kAllowedKeys = {"model", "system_prompt"};
    if (auto unknown_keys = reject_api_unknown_keys(json, "request", kAllowedKeys); !unknown_keys) {
        return std::unexpected(unknown_keys.error());
    }

    if (!json.contains("model") || !json.at("model").is_string()) {
        return std::unexpected(invalid_request_error("model must be a string", "model"));
    }

    SessionCreateRequest request;
    request.model = json.at("model").get<std::string>();
    if (request.model.empty()) {
        return std::unexpected(invalid_request_error("model must not be empty", "model"));
    }

    if (auto it = json.find("system_prompt"); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(
                invalid_request_error("system_prompt must be a string", "system_prompt"));
        }
        request.system_prompt = it->get<std::string>();
    }

    return request;
}

nlohmann::json make_error_body(const ApiError& error) {
    nlohmann::json body = {{"error", {{"message", error.message}, {"type", error.type}}}};
    if (error.param.has_value()) {
        body["error"]["param"] = *error.param;
    }
    if (error.code.has_value()) {
        body["error"]["code"] = *error.code;
    }
    return body;
}

drogon::HttpResponsePtr make_json_response(const nlohmann::json& body,
                                           drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCodeAndCustomString(drogon::CT_APPLICATION_JSON, "application/json");
    response->setBody(body.dump());
    return response;
}

drogon::HttpResponsePtr make_error_response(const ApiError& error) {
    return make_json_response(make_error_body(error),
                              static_cast<drogon::HttpStatusCode>(error.http_status));
}

drogon::HttpResponsePtr make_models_response(std::string_view model_id) {
    return make_json_response(nlohmann::json{
        {"object", "list"},
        {"data", {{{"id", model_id}, {"object", "model"}, {"owned_by", "zoo-keeper-server"}}}},
    });
}

drogon::HttpResponsePtr make_tools_response(const std::vector<ToolDefinition>& tools) {
    nlohmann::json data = nlohmann::json::array();
    for (const auto& metadata : tools) {
        data.push_back(make_tool_schema(metadata));
    }

    return make_json_response(nlohmann::json{{"object", "list"}, {"data", std::move(data)}});
}

drogon::HttpResponsePtr make_session_response(const SessionSummary& summary,
                                              drogon::HttpStatusCode status) {
    return make_json_response(make_session_body(summary), status);
}

drogon::HttpResponsePtr make_chat_completion_response(std::string_view completion_id,
                                                      std::int64_t created,
                                                      std::string_view model_id,
                                                      const CompletionResult& response) {
    return make_json_response(
        make_chat_completion_body(completion_id, created, model_id, response));
}

ApiError map_runtime_error_to_api_error(const RuntimeError& error) {
    switch (error.code) {
    case RuntimeErrorCode::InvalidConfig:
    case RuntimeErrorCode::InvalidSamplingParams:
        return invalid_request_error(error.message, std::nullopt, "invalid_config");
    case RuntimeErrorCode::InvalidModelPath:
    case RuntimeErrorCode::InvalidContextSize:
    case RuntimeErrorCode::BackendInitFailed:
    case RuntimeErrorCode::ModelLoadFailed:
        return server_error(error.message, "model_load_failed");
    case RuntimeErrorCode::ContextCreationFailed:
        return server_error(error.message, "context_creation_failed");
    case RuntimeErrorCode::InferenceFailed:
        return server_error(error.message, "inference_failed");
    case RuntimeErrorCode::TokenizationFailed:
        return server_error(error.message, "tokenization_failed");
    case RuntimeErrorCode::ContextWindowExceeded:
        return invalid_request_error(error.message, "messages", "context_window_exceeded");
    case RuntimeErrorCode::InvalidMessageSequence:
        return invalid_request_error(error.message, "messages", "invalid_message_sequence");
    case RuntimeErrorCode::TemplateRenderFailed:
        return server_error(error.message, "template_render_failed");
    case RuntimeErrorCode::AgentNotRunning:
    case RuntimeErrorCode::RequestCancelled:
        return service_unavailable_error(error.message, "agent_not_ready");
    case RuntimeErrorCode::RequestTimeout:
        return server_error(error.message, "request_timeout");
    case RuntimeErrorCode::QueueFull:
        return service_unavailable_error(error.message, "queue_full");
    case RuntimeErrorCode::ToolNotFound:
        return invalid_request_error(error.message, std::nullopt, "tool_not_found");
    case RuntimeErrorCode::ToolExecutionFailed:
    case RuntimeErrorCode::InvalidToolSignature:
    case RuntimeErrorCode::InvalidToolSchema:
    case RuntimeErrorCode::ToolValidationFailed:
        return server_error(error.message, "tool_execution_failed");
    case RuntimeErrorCode::ToolRetriesExhausted:
    case RuntimeErrorCode::ToolLoopLimitReached:
        return server_error(error.message, "tool_retries_exhausted");
    case RuntimeErrorCode::InvalidOutputSchema:
        return invalid_request_error(error.message, std::nullopt, "invalid_output_schema");
    case RuntimeErrorCode::ExtractionFailed:
        return server_error(error.message, "extraction_failed");
    case RuntimeErrorCode::Unknown:
        break;
    }
    return server_error(error.message, "runtime_error");
}

} // namespace zks::server
