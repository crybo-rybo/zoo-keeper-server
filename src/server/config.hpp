#pragma once

#include "server/result.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <zoo/core/types.hpp>

namespace zks::server {

struct CommandToolConfig {
    std::string name;
    std::string description;
    nlohmann::json parameters_schema;
    std::vector<std::string> command;
    std::optional<std::filesystem::path> working_directory;
    bool inherit_environment = false;
    std::map<std::string, std::string, std::less<>> env;
    std::uint32_t timeout_ms = 30000;

    [[nodiscard]] Result<void> validate() const;
};

struct SessionConfig {
    size_t max_sessions = 0;
    std::uint32_t idle_ttl_seconds = 900;

    [[nodiscard]] bool enabled() const noexcept {
        return max_sessions > 0;
    }

    [[nodiscard]] Result<void> validate() const;
};

struct HttpConfig {
    int64_t client_max_body_size_bytes = 1048576;      // 1 MiB
    int64_t client_max_memory_body_size_bytes = 65536; // 64 KiB
    int32_t idle_connection_timeout_seconds = 60;
    std::vector<std::string> cors_allow_origins; // empty = CORS disabled

    [[nodiscard]] Result<void> validate() const;
};

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8080;
    std::string model_id;
    std::optional<std::string> api_key;
    HttpConfig http;
    SessionConfig sessions;
    std::vector<CommandToolConfig> tools;
    zoo::Config zoo_config;

    [[nodiscard]] Result<void> validate() const;
};

[[nodiscard]] Result<ServerConfig> load_config(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> startup_warning(const ServerConfig& config);

} // namespace zks::server
