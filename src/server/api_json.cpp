#include "server/api_json.hpp"

#include <array>
#include <string>

namespace zks::server {
namespace {

template <size_t N>
ApiResult<void> reject_unknown_keys(const nlohmann::json& json, const char* context,
                                    const std::array<const char*, N>& allowed_keys) {
    if (!json.is_object()) {
        return std::unexpected(
            invalid_request_error(std::string(context) + " must be a JSON object"));
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        bool allowed = false;
        for (const char* key : allowed_keys) {
            if (it.key() == key) {
                allowed = true;
                break;
            }
        }

        if (!allowed) {
            return std::unexpected(
                invalid_request_error("Unknown " + std::string(context) + " field: " + it.key(),
                                      context, "unknown_field"));
        }
    }

    return {};
}

ApiResult<zoo::Message> parse_message(const nlohmann::json& json, std::vector<zoo::Message>& seed) {
    static constexpr std::array<const char*, 3> kAllowedMessageKeys = {"role", "content",
                                                                       "tool_call_id"};
    if (auto unknown_keys = reject_unknown_keys(json, "message", kAllowedMessageKeys);
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

    const auto role = json.at("role").get<std::string>();
    const auto content = json.at("content").get<std::string>();

    zoo::Message message = zoo::Message::user(content);
    if (role == "system") {
        if (json.contains("tool_call_id")) {
            return std::unexpected(
                invalid_request_error("tool_call_id is only valid for tool messages", "messages"));
        }
        message = zoo::Message::system(content);
    } else if (role == "user") {
        if (json.contains("tool_call_id")) {
            return std::unexpected(
                invalid_request_error("tool_call_id is only valid for tool messages", "messages"));
        }
        message = zoo::Message::user(content);
    } else if (role == "assistant") {
        if (json.contains("tool_call_id")) {
            return std::unexpected(
                invalid_request_error("tool_call_id is only valid for tool messages", "messages"));
        }
        message = zoo::Message::assistant(content);
    } else if (role == "tool") {
        if (!json.contains("tool_call_id") || !json.at("tool_call_id").is_string()) {
            return std::unexpected(invalid_request_error(
                "tool messages must include a string tool_call_id", "messages"));
        }
        message = zoo::Message::tool(content, json.at("tool_call_id").get<std::string>());
    } else {
        return std::unexpected(
            invalid_request_error("Unsupported message role: " + role, "messages", "invalid_role"));
    }

    if (auto validation = zoo::validate_role_sequence(seed, message.role); !validation) {
        return std::unexpected(invalid_request_error(validation.error().message, "messages",
                                                     "invalid_message_sequence"));
    }
    seed.push_back(message);
    return message;
}

nlohmann::json make_tool_schema(const zoo::tools::ToolMetadata& metadata) {
    return nlohmann::json{{"type", "function"},
                          {"function",
                           {{"name", metadata.name},
                            {"description", metadata.description},
                            {"parameters", metadata.parameters_schema}}}};
}

nlohmann::json make_chat_completion_body(std::string_view completion_id, std::int64_t created,
                                         std::string_view model_id, const zoo::Response& response) {
    return nlohmann::json{{"id", completion_id},
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
                            {"total_tokens", response.usage.total_tokens}}}};
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

    static constexpr std::array<const char*, 4> kAllowedKeys = {"model", "messages", "stream",
                                                                "session_id"};
    if (auto unknown_keys = reject_unknown_keys(json, "request", kAllowedKeys); !unknown_keys) {
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
    if (auto unknown_keys = reject_unknown_keys(json, "request", kAllowedKeys); !unknown_keys) {
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

drogon::HttpResponsePtr make_tools_response(const std::vector<zoo::tools::ToolMetadata>& tools) {
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
                                                      const zoo::Response& response) {
    return make_json_response(
        make_chat_completion_body(completion_id, created, model_id, response));
}

ApiError map_zoo_error_to_api_error(const zoo::Error& error) {
    using zoo::ErrorCode;

    switch (error.code) {
    case ErrorCode::InvalidMessageSequence:
        return invalid_request_error(error.message, "messages", "invalid_message_sequence");
    case ErrorCode::ContextWindowExceeded:
        return invalid_request_error(error.message, "messages", "context_window_exceeded");
    case ErrorCode::QueueFull:
        return service_unavailable_error(error.message, "queue_full");
    case ErrorCode::AgentNotRunning:
    case ErrorCode::RequestCancelled:
        return service_unavailable_error(error.message, "agent_not_ready");
    default:
        return server_error(error.to_string(), "runtime_error");
    }
}

} // namespace zks::server
