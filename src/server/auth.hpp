#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>

namespace zks::server {

/// Checks the Authorization header against the configured API key.
/// Returns nullopt if auth passes (or is not configured), or an ApiError to return to the client.
[[nodiscard]] inline std::optional<ApiError> check_auth(const drogon::HttpRequestPtr& request,
                                                        const ServerConfig& config) {
    if (!config.api_key.has_value()) {
        return std::nullopt;
    }

    const auto& auth_header = request->getHeader("Authorization");
    // Avoid allocating "Bearer " + key on every request. Instead, check prefix
    // and compare the suffix against the configured key in-place.
    constexpr std::string_view kBearerPrefix = "Bearer ";
    if (auth_header.size() != kBearerPrefix.size() + config.api_key->size() ||
        auth_header.compare(0, kBearerPrefix.size(), kBearerPrefix.data(), kBearerPrefix.size()) != 0 ||
        auth_header.compare(kBearerPrefix.size(), std::string::npos, *config.api_key) != 0) {
        return auth_error("Invalid API key", "invalid_api_key");
    }

    return std::nullopt;
}

} // namespace zks::server
