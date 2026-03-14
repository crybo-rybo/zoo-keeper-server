#include "server/config.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <zoo/core/json.hpp>

namespace zks::server {
namespace {

bool is_trusted_local_bind_address(std::string_view bind_address) {
    return bind_address == "localhost" || bind_address == "::1" ||
           bind_address.starts_with("127.");
}

template <size_t N>
Result<void> reject_unknown_keys(const nlohmann::json& json, const char* context,
                                 const std::array<const char*, N>& allowed_keys) {
    if (!json.is_object()) {
        return std::unexpected(std::string(context) + " must be a JSON object");
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        bool allowed = false;
        for (const char* key : allowed_keys) {
            if (it.key() == key) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return std::unexpected("Unknown " + std::string(context) + " key: " + it.key());
        }
    }

    return {};
}

std::string with_path_context(const std::filesystem::path& path, const std::string& message) {
    return path.string() + ": " + message;
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
            return std::unexpected("tools entry '" + tool.name + "' is invalid: " +
                                   validation.error());
        }
    }
    if (auto validation = zoo_config.validate(); !validation) {
        return std::unexpected(validation.error().to_string());
    }
    return {};
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

    static constexpr std::array<const char*, 8> kAllowedKeys = {
        "bind_address", "port", "model_id", "api_key", "http", "sessions", "tools", "zoo"};

    if (auto unknown_key_check = reject_unknown_keys(json, "server config", kAllowedKeys);
        !unknown_key_check) {
        return std::unexpected(with_path_context(path, unknown_key_check.error()));
    }

    try {
        ServerConfig config;
        if (auto it = json.find("bind_address"); it != json.end()) {
            it->get_to(config.bind_address);
        }
        if (auto it = json.find("port"); it != json.end()) {
            unsigned int parsed_port = 0;
            it->get_to(parsed_port);
            if (parsed_port == 0 || parsed_port > 65535) {
                return std::unexpected(with_path_context(
                    path, "port must be in the range [1, 65535]"));
            }
            config.port = static_cast<uint16_t>(parsed_port);
        }
        if (!json.contains("model_id")) {
            return std::unexpected(
                with_path_context(path, "Server config must contain required key: model_id"));
        }
        json.at("model_id").get_to(config.model_id);
        if (auto it = json.find("api_key"); it != json.end()) {
            if (!it->is_null()) {
                if (!it->is_string()) {
                    return std::unexpected(with_path_context(path, "api_key must be a string"));
                }
                config.api_key = it->get<std::string>();
            }
        }
        if (auto it = json.find("http"); it != json.end()) {
            static constexpr std::array<const char*, 3> kAllowedHttpKeys = {
                "client_max_body_size_bytes", "client_max_memory_body_size_bytes",
                "idle_connection_timeout_seconds"};
            if (auto unknown_keys =
                    reject_unknown_keys(*it, "http config", kAllowedHttpKeys);
                !unknown_keys) {
                return std::unexpected(with_path_context(path, unknown_keys.error()));
            }

            if (auto http_it = it->find("client_max_body_size_bytes"); http_it != it->end()) {
                http_it->get_to(config.http.client_max_body_size_bytes);
            }
            if (auto http_it = it->find("client_max_memory_body_size_bytes");
                http_it != it->end()) {
                http_it->get_to(config.http.client_max_memory_body_size_bytes);
            }
            if (auto http_it = it->find("idle_connection_timeout_seconds");
                http_it != it->end()) {
                http_it->get_to(config.http.idle_connection_timeout_seconds);
            }
        }
        if (auto it = json.find("sessions"); it != json.end()) {
            static constexpr std::array<const char*, 2> kAllowedSessionKeys = {
                "max_sessions", "idle_ttl_seconds"};
            if (auto unknown_keys =
                    reject_unknown_keys(*it, "sessions config", kAllowedSessionKeys);
                !unknown_keys) {
                return std::unexpected(with_path_context(path, unknown_keys.error()));
            }

            if (auto session_it = it->find("max_sessions"); session_it != it->end()) {
                session_it->get_to(config.sessions.max_sessions);
            }
            if (auto session_it = it->find("idle_ttl_seconds"); session_it != it->end()) {
                session_it->get_to(config.sessions.idle_ttl_seconds);
            }
        }
        if (auto it = json.find("tools"); it != json.end()) {
            if (!it->is_array()) {
                return std::unexpected(with_path_context(path, "tools must be an array"));
            }

            for (size_t index = 0; index < it->size(); ++index) {
                const auto& tool_json = (*it)[index];
                static constexpr std::array<const char*, 7> kAllowedToolKeys = {
                    "name", "description", "parameters", "command", "working_directory", "env",
                    "timeout_ms"};
                if (auto unknown_keys =
                        reject_unknown_keys(tool_json, "tool config", kAllowedToolKeys);
                    !unknown_keys) {
                    return std::unexpected(with_path_context(path, unknown_keys.error()));
                }
                if (!tool_json.contains("name") || !tool_json.at("name").is_string()) {
                    return std::unexpected(with_path_context(
                        path, "tools[" + std::to_string(index) + "].name must be a string"));
                }
                if (!tool_json.contains("description") || !tool_json.at("description").is_string()) {
                    return std::unexpected(with_path_context(
                        path,
                        "tools[" + std::to_string(index) + "].description must be a string"));
                }
                if (!tool_json.contains("parameters") || !tool_json.at("parameters").is_object()) {
                    return std::unexpected(with_path_context(
                        path,
                        "tools[" + std::to_string(index) + "].parameters must be an object"));
                }
                if (!tool_json.contains("command") || !tool_json.at("command").is_array()) {
                    return std::unexpected(with_path_context(
                        path, "tools[" + std::to_string(index) + "].command must be an array"));
                }

                CommandToolConfig tool;
                tool.name = tool_json.at("name").get<std::string>();
                tool.description = tool_json.at("description").get<std::string>();
                tool.parameters_schema = tool_json.at("parameters");

                for (const auto& arg_json : tool_json.at("command")) {
                    if (!arg_json.is_string()) {
                        return std::unexpected(with_path_context(
                            path,
                            "tools[" + std::to_string(index) +
                                "].command entries must be strings"));
                    }
                    tool.command.push_back(arg_json.get<std::string>());
                }

                if (auto dir_it = tool_json.find("working_directory");
                    dir_it != tool_json.end()) {
                    if (!dir_it->is_string()) {
                        return std::unexpected(with_path_context(
                            path, "tools[" + std::to_string(index) +
                                      "].working_directory must be a string"));
                    }
                    tool.working_directory = dir_it->get<std::string>();
                }
                if (auto env_it = tool_json.find("env"); env_it != tool_json.end()) {
                    if (!env_it->is_object()) {
                        return std::unexpected(with_path_context(
                            path, "tools[" + std::to_string(index) + "].env must be an object"));
                    }
                    for (auto env_value = env_it->begin(); env_value != env_it->end(); ++env_value) {
                        if (!env_value.value().is_string()) {
                            return std::unexpected(with_path_context(
                                path, "tools[" + std::to_string(index) +
                                          "].env values must be strings"));
                        }
                        tool.env.emplace(env_value.key(), env_value.value().get<std::string>());
                    }
                }
                if (auto timeout_it = tool_json.find("timeout_ms");
                    timeout_it != tool_json.end()) {
                    if (!timeout_it->is_number_unsigned()) {
                        return std::unexpected(with_path_context(
                            path,
                            "tools[" + std::to_string(index) + "].timeout_ms must be a positive integer"));
                    }
                    tool.timeout_ms = timeout_it->get<std::uint32_t>();
                }

                config.tools.push_back(std::move(tool));
            }
        }
        if (!json.contains("zoo")) {
            return std::unexpected(
                with_path_context(path, "Server config must contain required key: zoo"));
        }
        config.zoo_config = json.at("zoo").get<zoo::Config>();

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
