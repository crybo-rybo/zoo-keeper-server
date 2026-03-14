#pragma once

#include "server/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <zoo/core/types.hpp>

namespace zks::server {

struct SessionConfig {
    size_t max_sessions = 0;
    std::uint32_t idle_ttl_seconds = 900;

    [[nodiscard]] bool enabled() const noexcept {
        return max_sessions > 0;
    }

    [[nodiscard]] Result<void> validate() const;
};

struct HttpConfig {
    int64_t client_max_body_size_bytes = 1048576;        // 1 MiB
    int64_t client_max_memory_body_size_bytes = 65536;   // 64 KiB
    int32_t idle_connection_timeout_seconds = 60;

    [[nodiscard]] Result<void> validate() const;
};

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8080;
    std::string model_id;
    std::optional<std::string> api_key;
    HttpConfig http;
    SessionConfig sessions;
    zoo::Config zoo_config;

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] Result<ServerConfig> load_config(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> startup_warning(const ServerConfig& config);

} // namespace zks::server
