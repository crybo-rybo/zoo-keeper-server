#include "server/config.hpp"

#include "server/internal_utils.hpp"

#include <array>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <zoo/core/json.hpp>

namespace zks::server {
namespace {

bool is_trusted_local_bind_address(std::string_view bind_address) {
    return bind_address == "localhost" || bind_address == "::1" || bind_address.starts_with("127.");
}

std::string with_path_context(const std::filesystem::path& path, const std::string& message) {
    return path.string() + ": " + message;
}

// Wraps the shared reject_unknown_keys (returns Result<void>) for use in from_json
// where we throw on failure.
void check_unknown_keys(const nlohmann::json& json, std::string_view context,
                        std::span<const std::string_view> allowed_keys) {
    auto result = reject_unknown_keys(json, context, allowed_keys);
    if (!result) {
        throw nlohmann::json::other_error::create(501, result.error(), nullptr);
    }
}

} // namespace

Result<void> SessionConfig::validate() const {
    if (idle_ttl_seconds == 0) {
        return std::unexpected("sessions.idle_ttl_seconds must be >= 1");
    }
    return {};
}

Result<void> HttpConfig::validate() const {
    if (client_max_body_size_bytes <= 0) {
        return std::unexpected("http.client_max_body_size_bytes must be > 0");
    }
    if (client_max_memory_body_size_bytes <= 0) {
        return std::unexpected("http.client_max_memory_body_size_bytes must be > 0");
    }
    if (idle_connection_timeout_seconds < 0) {
        return std::unexpected("http.idle_connection_timeout_seconds must be >= 0");
    }
    return {};
}

Result<void> ServerConfig::validate() const {
    if (bind_address.empty()) {
        return std::unexpected("bind_address cannot be empty");
    }
    if (port == 0) {
        return std::unexpected("port must be in the range [1, 65535]");
    }
    if (model_id.empty()) {
        return std::unexpected("model_id cannot be empty");
    }
    if (api_key.has_value() && api_key->empty()) {
        return std::unexpected("api_key must not be empty");
    }
    if (auto validation = http.validate(); !validation) {
        return std::unexpected(validation.error());
    }
    if (auto validation = sessions.validate(); !validation) {
        return std::unexpected(validation.error());
    }
    for (const auto& tool : tools) {
        if (auto validation = tool.validate(); !validation) {
            return std::unexpected("tools entry '" + tool.name +
                                   "' is invalid: " + validation.error());
        }
    }
    if (auto validation = zoo_config.validate(); !validation) {
        return std::unexpected(validation.error().to_string());
    }
    return {};
}

// --- nlohmann from_json ADL customization ---

void from_json(const nlohmann::json& j, HttpConfig& config) {
    static constexpr std::array<std::string_view, 4> kAllowed = {
        "client_max_body_size_bytes", "client_max_memory_body_size_bytes",
        "idle_connection_timeout_seconds", "cors_allow_origins"};
    check_unknown_keys(j, "http config", kAllowed);

    if (auto it = j.find("client_max_body_size_bytes"); it != j.end()) {
        it->get_to(config.client_max_body_size_bytes);
    }
    if (auto it = j.find("client_max_memory_body_size_bytes"); it != j.end()) {
        it->get_to(config.client_max_memory_body_size_bytes);
    }
    if (auto it = j.find("idle_connection_timeout_seconds"); it != j.end()) {
        it->get_to(config.idle_connection_timeout_seconds);
    }
    if (auto it = j.find("cors_allow_origins"); it != j.end()) {
        if (!it->is_array()) {
            throw nlohmann::json::type_error::create(
                302, "http.cors_allow_origins must be an array", nullptr);
        }
        for (const auto& origin : *it) {
            if (!origin.is_string()) {
                throw nlohmann::json::type_error::create(
                    302, "http.cors_allow_origins entries must be strings", nullptr);
            }
            config.cors_allow_origins.push_back(origin.get<std::string>());
        }
    }
}

void from_json(const nlohmann::json& j, SessionConfig& config) {
    static constexpr std::array<std::string_view, 2> kAllowed = {"max_sessions",
                                                                 "idle_ttl_seconds"};
    check_unknown_keys(j, "sessions config", kAllowed);

    if (auto it = j.find("max_sessions"); it != j.end()) {
        it->get_to(config.max_sessions);
    }
    if (auto it = j.find("idle_ttl_seconds"); it != j.end()) {
        it->get_to(config.idle_ttl_seconds);
    }
}

void from_json(const nlohmann::json& j, CommandToolConfig& tool) {
    static constexpr std::array<std::string_view, 8> kAllowed = {
        "name", "description", "parameters",         "command", "working_directory",
        "env",  "timeout_ms",  "inherit_environment"};
    check_unknown_keys(j, "tool config", kAllowed);

    auto require_string = [&](const char* field) -> std::string {
        if (!j.contains(field) || !j.at(field).is_string()) {
            throw nlohmann::json::type_error::create(302, std::string(field) + " must be a string",
                                                     nullptr);
        }
        return j.at(field).get<std::string>();
    };

    tool.name = require_string("name");
    tool.description = require_string("description");

    if (!j.contains("parameters") || !j.at("parameters").is_object()) {
        throw nlohmann::json::type_error::create(302, "parameters must be an object", nullptr);
    }
    tool.parameters_schema = j.at("parameters");

    if (!j.contains("command") || !j.at("command").is_array()) {
        throw nlohmann::json::type_error::create(302, "command must be an array", nullptr);
    }
    for (const auto& arg : j.at("command")) {
        if (!arg.is_string()) {
            throw nlohmann::json::type_error::create(302, "command entries must be strings",
                                                     nullptr);
        }
        tool.command.push_back(arg.get<std::string>());
    }

    if (auto it = j.find("working_directory"); it != j.end()) {
        if (!it->is_string()) {
            throw nlohmann::json::type_error::create(302, "working_directory must be a string",
                                                     nullptr);
        }
        tool.working_directory = it->get<std::string>();
    }
    if (auto it = j.find("env"); it != j.end()) {
        if (!it->is_object()) {
            throw nlohmann::json::type_error::create(302, "env must be an object", nullptr);
        }
        for (auto env_it = it->begin(); env_it != it->end(); ++env_it) {
            if (!env_it.value().is_string()) {
                throw nlohmann::json::type_error::create(302, "env values must be strings",
                                                         nullptr);
            }
            tool.env.emplace(env_it.key(), env_it.value().get<std::string>());
        }
    }
    if (auto it = j.find("inherit_environment"); it != j.end()) {
        if (!it->is_boolean()) {
            throw nlohmann::json::type_error::create(302, "inherit_environment must be a boolean",
                                                     nullptr);
        }
        tool.inherit_environment = it->get<bool>();
    }
    if (auto it = j.find("timeout_ms"); it != j.end()) {
        if (!it->is_number_unsigned()) {
            throw nlohmann::json::type_error::create(302, "timeout_ms must be a positive integer",
                                                     nullptr);
        }
        tool.timeout_ms = it->get<std::uint32_t>();
    }
}

void from_json(const nlohmann::json& j, ServerConfig& config) {
    static constexpr std::array<std::string_view, 8> kAllowed = {
        "bind_address", "port", "model_id", "api_key", "http", "sessions", "tools", "zoo"};
    check_unknown_keys(j, "server config", kAllowed);

    if (auto it = j.find("bind_address"); it != j.end()) {
        it->get_to(config.bind_address);
    }
    if (auto it = j.find("port"); it != j.end()) {
        unsigned int parsed_port = 0;
        it->get_to(parsed_port);
        if (parsed_port == 0 || parsed_port > 65535) {
            throw nlohmann::json::out_of_range::create(401, "port must be in the range [1, 65535]",
                                                       nullptr);
        }
        config.port = static_cast<uint16_t>(parsed_port);
    }
    if (!j.contains("model_id")) {
        throw nlohmann::json::other_error::create(
            501, "Server config must contain required key: model_id", nullptr);
    }
    j.at("model_id").get_to(config.model_id);

    if (auto it = j.find("api_key"); it != j.end()) {
        if (!it->is_null()) {
            if (!it->is_string()) {
                throw nlohmann::json::type_error::create(302, "api_key must be a string", nullptr);
            }
            config.api_key = it->get<std::string>();
        }
    }
    if (auto it = j.find("http"); it != j.end()) {
        config.http = it->get<HttpConfig>();
    }
    if (auto it = j.find("sessions"); it != j.end()) {
        config.sessions = it->get<SessionConfig>();
    }
    if (auto it = j.find("tools"); it != j.end()) {
        if (!it->is_array()) {
            throw nlohmann::json::type_error::create(302, "tools must be an array", nullptr);
        }
        for (size_t i = 0; i < it->size(); ++i) {
            try {
                config.tools.push_back((*it)[i].get<CommandToolConfig>());
            } catch (const std::exception& e) {
                throw nlohmann::json::other_error::create(
                    501, "tools[" + std::to_string(i) + "]: " + e.what(), nullptr);
            }
        }
    }
    if (!j.contains("zoo")) {
        throw nlohmann::json::other_error::create(
            501, "Server config must contain required key: zoo", nullptr);
    }
    config.zoo_config = j.at("zoo").get<zoo::Config>();
}

Result<ServerConfig> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::unexpected("Unable to open config file: " + path.string());
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception& error) {
        return std::unexpected(with_path_context(path, error.what()));
    }

    try {
        auto config = json.get<ServerConfig>();
        if (auto validation = config.validate(); !validation) {
            return std::unexpected(with_path_context(path, validation.error()));
        }
        return config;
    } catch (const std::exception& error) {
        return std::unexpected(with_path_context(path, error.what()));
    }
}

std::optional<std::string> startup_warning(const ServerConfig& config) {
    const bool has_auth = config.api_key.has_value() && !config.api_key->empty();
    if (!has_auth && !is_trusted_local_bind_address(config.bind_address)) {
        return "Warning: binding a non-loopback address without api_key auth enabled. "
               "Use only on a trusted network.";
    }

    return std::nullopt;
}

} // namespace zks::server
