#include "server/config.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <zoo/core/json.hpp>

namespace zks::server {
namespace {

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

    static constexpr std::array<const char*, 4> kAllowedKeys = {
        "bind_address", "port", "model_id", "zoo"};

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

} // namespace zks::server
