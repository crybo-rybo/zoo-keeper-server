#pragma once

#include "server/api_types.hpp"
#include "server/config.hpp"

#include <cstddef>
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
        auth_header.compare(0, kBearerPrefix.size(), kBearerPrefix.data(), kBearerPrefix.size()) != 0) {
        return auth_error("Invalid API key", "invalid_api_key");
    }

    // Constant-time comparison of the key portion to prevent timing side-channel attacks.
    const auto* key_start = auth_header.data() + kBearerPrefix.size();
    const auto& expected = *config.api_key;
    volatile unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(key_start[i]) ^
                static_cast<unsigned char>(expected[i]);
    }
    if (diff != 0) {
        return auth_error("Invalid API key", "invalid_api_key");
    }

    return std::nullopt;
}

} // namespace zks::server
