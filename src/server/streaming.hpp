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

/// First streaming chunk — includes the "role":"assistant" field.
std::string make_first_streaming_chunk(std::string_view completion_id, std::int64_t created,
                                       std::string_view model_id,
                                       std::optional<std::string_view> content,
                                       std::optional<std::string_view> finish_reason);

/// Subsequent streaming chunk — delta only, no role field.
std::string make_streaming_chunk(std::string_view completion_id, std::int64_t created,
                                 std::string_view model_id, std::optional<std::string_view> content,
                                 std::optional<std::string_view> finish_reason);

} // namespace zks::server
