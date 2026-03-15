#pragma once

#include "server/api_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zks::server {

std::string make_sse_data(const nlohmann::json& payload);

// Returns the SSE stream termination marker. Returns std::string because
// callers pass it to push_frame() which takes std::string by value.
std::string make_sse_done();
std::string make_chat_completion_chunk(std::string_view completion_id, std::int64_t created,
                                       std::string_view model_id,
                                       std::optional<std::string_view> content, bool include_role,
                                       std::optional<std::string_view> finish_reason);

} // namespace zks::server
