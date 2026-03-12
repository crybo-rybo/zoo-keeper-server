#pragma once

#include "server/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <zoo/core/types.hpp>

namespace zks::server {

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8080;
    std::string model_id;
    zoo::Config zoo_config;

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] Result<ServerConfig> load_config(const std::filesystem::path& path);

} // namespace zks::server
