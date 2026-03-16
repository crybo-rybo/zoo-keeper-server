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
    constexpr std::string_view kBearerPrefix = "Bearer ";
    const auto& expected = *config.api_key;

    // Always perform the constant-time comparison loop regardless of whether
    // the header length matches, to avoid leaking key length via timing.
    const size_t key_start_offset = kBearerPrefix.size();
    const size_t expected_total = key_start_offset + expected.size();
    const size_t compare_len = std::max(expected.size(), std::size_t{1});

    volatile unsigned char diff = 0;

    // Check that the header starts with "Bearer "
    if (auth_header.size() < key_start_offset ||
        auth_header.compare(0, kBearerPrefix.size(), kBearerPrefix.data(),
                            kBearerPrefix.size()) != 0) {
        diff = 1;
    }

    // Length mismatch flag — folded into diff after the loop
    const unsigned char length_mismatch =
        (auth_header.size() != expected_total) ? 1 : 0;

    // Constant-time comparison of the key portion
    for (std::size_t i = 0; i < compare_len; ++i) {
        const unsigned char a =
            (key_start_offset + i < auth_header.size())
                ? static_cast<unsigned char>(auth_header[key_start_offset + i])
                : 0xFF;
        const unsigned char b =
            (i < expected.size()) ? static_cast<unsigned char>(expected[i]) : 0xFF;
        diff |= a ^ b;
    }

    diff |= length_mismatch;

    if (diff != 0) {
        return auth_error("Invalid API key", "invalid_api_key");
    }

    return std::nullopt;
}

} // namespace zks::server
