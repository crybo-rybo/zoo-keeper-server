#pragma once

#include "server/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

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

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8080;
    std::string model_id;
    SessionConfig sessions;
    zoo::Config zoo_config;

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] Result<ServerConfig> load_config(const std::filesystem::path& path);

} // namespace zks::server
