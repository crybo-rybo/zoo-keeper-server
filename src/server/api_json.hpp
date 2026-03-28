#pragma once

#include "server/api_types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace zks::server {

/// Parses a chat completion request body into a validated request object.
ApiResult<ChatCompletionRequest> parse_chat_completion_request(std::string_view body);
/// Parses a structured extraction request body into a validated request object.
ApiResult<ExtractionRequest> parse_extraction_request(std::string_view body);
/// Parses a retained-agent chat request body into a validated request object.
ApiResult<AgentChatRequest> parse_agent_chat_request(std::string_view body);
/// Parses a session creation request body into a validated request object.
ApiResult<SessionCreateRequest> parse_session_create_request(std::string_view body);
/// Parses a retained-agent history replacement or swap body.
ApiResult<AgentHistoryRequest> parse_agent_history_request(std::string_view body);
/// Parses a retained-agent single-message append body.
ApiResult<AgentHistoryMessageRequest> parse_agent_history_message_request(std::string_view body);
/// Parses a system prompt update body.
ApiResult<SystemPromptRequest> parse_system_prompt_request(std::string_view body);

/// Builds the standard API error response body.
nlohmann::json make_error_body(const ApiError& error);
/// Builds a JSON HTTP error response from an ApiError.
drogon::HttpResponsePtr make_error_response(const ApiError& error);
/// Builds a JSON HTTP response with the provided status code.
drogon::HttpResponsePtr make_json_response(const nlohmann::json& body,
                                           drogon::HttpStatusCode status = drogon::k200OK);

/// Builds the OpenAI-compatible models list response.
drogon::HttpResponsePtr make_models_response(std::string_view model_id);
/// Builds the server tool catalog response.
drogon::HttpResponsePtr make_tools_response(const std::vector<ToolDefinition>& tools);
/// Builds a session summary response.
drogon::HttpResponsePtr make_session_response(const SessionSummary& summary,
                                              drogon::HttpStatusCode status = drogon::k200OK);
/// Builds a non-streaming chat completion response.
drogon::HttpResponsePtr make_chat_completion_response(std::string_view completion_id,
                                                      std::int64_t created,
                                                      std::string_view model_id,
                                                      const CompletionResult& response);
/// Builds a non-streaming extraction response.
drogon::HttpResponsePtr make_extraction_response(std::string_view extraction_id,
                                                 std::int64_t created, std::string_view model_id,
                                                 const ExtractionResult& response);
/// Builds the extraction response body JSON (used by both streaming and non-streaming paths).
nlohmann::json make_extraction_body(std::string_view extraction_id, std::int64_t created,
                                    std::string_view model_id, const ExtractionResult& response);

/// Maps a zoo runtime error into the public API error shape.
ApiError map_runtime_error_to_api_error(const RuntimeError& error);

} // namespace zks::server
