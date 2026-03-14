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

    const auto auth_header = request->getHeader("Authorization");
    if (auth_header != "Bearer " + *config.api_key) {
        return auth_error("Invalid API key", "invalid_api_key");
    }

    return std::nullopt;
}

} // namespace zks::server
