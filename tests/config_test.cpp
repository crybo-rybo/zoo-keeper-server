#include "server/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("zks-config-test-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

bool write_text_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path);
    if (!output) {
        return false;
    }
    output << contents;
    return static_cast<bool>(output);
}

} // namespace

int main() {
    const auto temp_dir = make_temp_dir();
    const auto cleanup = [&temp_dir]() { std::filesystem::remove_all(temp_dir); };

    const auto valid_path = temp_dir / "valid.json";
    const auto invalid_path = temp_dir / "invalid.json";
    const auto empty_api_key_path = temp_dir / "empty_api_key.json";

    if (!write_text_file(valid_path,
                         R"json({
  "bind_address": "127.0.0.1",
  "port": 8081,
  "model_id": "demo-model",
  "sessions": {
    "max_sessions": 3,
    "idle_ttl_seconds": 600
  },
  "zoo": {
    "model_path": "/tmp/demo.gguf",
    "max_tokens": 32
  }
})json")) {
        std::cerr << "Failed to write valid config fixture." << '\n';
        cleanup();
        return 1;
    }

    if (!write_text_file(invalid_path,
                         R"json({
  "model_id": "demo-model",
  "zoo": {
    "model_path": "/tmp/demo.gguf"
  },
  "extra_key": true
})json")) {
        std::cerr << "Failed to write invalid config fixture." << '\n';
        cleanup();
        return 1;
    }

    if (!write_text_file(empty_api_key_path,
                         R"json({
  "model_id": "demo-model",
  "api_key": "",
  "zoo": {
    "model_path": "/tmp/demo.gguf"
  }
})json")) {
        std::cerr << "Failed to write empty api_key config fixture." << '\n';
        cleanup();
        return 1;
    }

    auto valid_config = zks::server::load_config(valid_path);
    if (!valid_config) {
        std::cerr << "Valid config unexpectedly failed: " << valid_config.error() << '\n';
        cleanup();
        return 1;
    }

    if (valid_config->bind_address != "127.0.0.1" || valid_config->port != 8081 ||
        valid_config->model_id != "demo-model" ||
        valid_config->sessions.max_sessions != 3 || valid_config->sessions.idle_ttl_seconds != 600 ||
        valid_config->zoo_config.model_path != "/tmp/demo.gguf" ||
        valid_config->zoo_config.max_tokens != 32) {
        std::cerr << "Valid config parsed with unexpected values." << '\n';
        cleanup();
        return 1;
    }

    auto invalid_config = zks::server::load_config(invalid_path);
    if (invalid_config) {
        std::cerr << "Invalid config unexpectedly parsed successfully." << '\n';
        cleanup();
        return 1;
    }

    if (invalid_config.error().find("Unknown server config key: extra_key") == std::string::npos) {
        std::cerr << "Invalid config failed for the wrong reason: " << invalid_config.error()
                  << '\n';
        cleanup();
        return 1;
    }

    auto empty_api_key_config = zks::server::load_config(empty_api_key_path);
    if (empty_api_key_config) {
        std::cerr << "Empty api_key config unexpectedly parsed successfully." << '\n';
        cleanup();
        return 1;
    }

    if (empty_api_key_config.error().find("api_key must not be empty") == std::string::npos) {
        std::cerr << "Empty api_key config failed for the wrong reason: "
                  << empty_api_key_config.error() << '\n';
        cleanup();
        return 1;
    }

    {
        zks::server::ServerConfig config;
        config.bind_address = "0.0.0.0";
        config.model_id = "demo-model";
        config.zoo_config.model_path = "/tmp/demo.gguf";

        const auto warning = zks::server::startup_warning(config);
        if (!warning.has_value() ||
            warning->find("non-loopback address without api_key auth enabled") ==
                std::string::npos) {
            std::cerr << "Expected startup warning for remote bind without auth." << '\n';
            cleanup();
            return 1;
        }
    }

    {
        zks::server::ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.model_id = "demo-model";
        config.zoo_config.model_path = "/tmp/demo.gguf";

        if (zks::server::startup_warning(config).has_value()) {
            std::cerr << "Loopback bind should not emit a startup warning." << '\n';
            cleanup();
            return 1;
        }
    }

    {
        zks::server::ServerConfig config;
        config.bind_address = "0.0.0.0";
        config.model_id = "demo-model";
        config.api_key = "secret";
        config.zoo_config.model_path = "/tmp/demo.gguf";

        if (zks::server::startup_warning(config).has_value()) {
            std::cerr << "Authenticated remote bind should not emit a startup warning." << '\n';
            cleanup();
            return 1;
        }
    }

    cleanup();
    return 0;
}
