#pragma once

#include "server/api_types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

ApiResult<ChatCompletionRequest> parse_chat_completion_request(std::string_view body);
ApiResult<SessionCreateRequest> parse_session_create_request(std::string_view body);

nlohmann::json make_error_body(const ApiError& error);
drogon::HttpResponsePtr make_error_response(const ApiError& error);
drogon::HttpResponsePtr make_json_response(const nlohmann::json& body,
                                           drogon::HttpStatusCode status = drogon::k200OK);

drogon::HttpResponsePtr make_models_response(std::string_view model_id);
drogon::HttpResponsePtr make_tools_response(const std::vector<zoo::tools::ToolMetadata>& tools);
drogon::HttpResponsePtr make_session_response(const SessionSummary& summary,
                                              drogon::HttpStatusCode status = drogon::k200OK);
drogon::HttpResponsePtr make_chat_completion_response(std::string_view completion_id,
                                                      std::int64_t created,
                                                      std::string_view model_id,
                                                      const zoo::Response& response);

ApiError map_zoo_error_to_api_error(const zoo::Error& error);

} // namespace zks::server
