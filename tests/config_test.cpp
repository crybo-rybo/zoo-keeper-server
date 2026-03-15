#include "doctest.h"

#include "server/command_tools.hpp"
#include "server/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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

struct TempDir {
    std::filesystem::path path;
    TempDir() : path(make_temp_dir()) {}
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("valid config parses correctly") {
    TempDir tmp;
    auto file = tmp.path / "valid.json";
    REQUIRE(write_text_file(file,
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
})json"));

    auto config = zks::server::load_config(file);
    REQUIRE(config.has_value());
    CHECK(config->bind_address == "127.0.0.1");
    CHECK(config->port == 8081);
    CHECK(config->model_id == "demo-model");
    CHECK(config->sessions.max_sessions == 3);
    CHECK(config->sessions.idle_ttl_seconds == 600);
    CHECK(config->zoo_config.model_path == "/tmp/demo.gguf");
    CHECK(config->zoo_config.max_tokens == 32);
}

TEST_CASE("unknown server config key rejected") {
    TempDir tmp;
    auto file = tmp.path / "invalid.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "zoo": { "model_path": "/tmp/demo.gguf" },
  "extra_key": true
})json"));

    auto config = zks::server::load_config(file);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().find("Unknown server config key: extra_key") != std::string::npos);
}

TEST_CASE("empty api_key rejected") {
    TempDir tmp;
    auto file = tmp.path / "empty_api_key.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "api_key": "",
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto config = zks::server::load_config(file);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().find("api_key must not be empty") != std::string::npos);
}

TEST_CASE("http defaults when section omitted") {
    TempDir tmp;
    auto file = tmp.path / "no_http.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto config = zks::server::load_config(file);
    REQUIRE(config.has_value());
    CHECK(config->http.client_max_body_size_bytes == 1048576);
    CHECK(config->http.client_max_memory_body_size_bytes == 65536);
    CHECK(config->http.idle_connection_timeout_seconds == 60);
}

TEST_CASE("tools config parses correctly") {
    TempDir tmp;
    auto file = tmp.path / "tools.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "tools": [{
    "name": "env",
    "description": "Print environment",
    "parameters": {
      "type": "object",
      "properties": { "value": { "type": "string" } },
      "required": ["value"],
      "additionalProperties": false
    },
    "command": ["/usr/bin/env"],
    "inherit_environment": true,
    "timeout_ms": 1500,
    "env": { "ZKS_TEST_FLAG": "1" }
  }],
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->tools.size() == 1);
    CHECK(cfg->tools[0].name == "env");
    CHECK(cfg->tools[0].command.size() == 1);
    CHECK(cfg->tools[0].command[0] == "/usr/bin/env");
    CHECK(cfg->tools[0].inherit_environment);
    CHECK(cfg->tools[0].timeout_ms == 1500);
    CHECK(cfg->tools[0].env.at("ZKS_TEST_FLAG") == "1");
}

TEST_CASE("inherit_environment defaults to false") {
    TempDir tmp;
    auto file = tmp.path / "default_env.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "tools": [{
    "name": "env-default",
    "description": "Print environment",
    "parameters": { "type": "object", "properties": {}, "additionalProperties": false },
    "command": ["/usr/bin/env"]
  }],
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->tools.size() == 1);
    CHECK_FALSE(cfg->tools[0].inherit_environment);
}

TEST_CASE("missing tool executable rejected at provider creation") {
    TempDir tmp;
    auto file = tmp.path / "missing_tool.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "tools": [{
    "name": "missing",
    "description": "Missing executable",
    "parameters": { "type": "object", "properties": {}, "additionalProperties": false },
    "command": ["/definitely/missing/tool"]
  }],
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE(cfg.has_value());
    auto provider = zks::server::make_command_tool_provider(cfg->tools);
    REQUIRE_FALSE(provider.has_value());
    CHECK(provider.error().find("executable not found") != std::string::npos);
}

TEST_CASE("bad tool schema rejected") {
    TempDir tmp;
    auto file = tmp.path / "bad_schema.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "tools": [{
    "name": "bad-schema",
    "description": "Unsupported schema",
    "parameters": {
      "type": "object",
      "properties": { "nested": { "type": "object" } }
    },
    "command": ["/usr/bin/env"]
  }],
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("unsupported type: object") != std::string::npos);
}

TEST_CASE("explicit http overrides") {
    TempDir tmp;
    auto file = tmp.path / "http_override.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "http": {
    "client_max_body_size_bytes": 2097152,
    "client_max_memory_body_size_bytes": 131072,
    "idle_connection_timeout_seconds": 120
  },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE(cfg.has_value());
    CHECK(cfg->http.client_max_body_size_bytes == 2097152);
    CHECK(cfg->http.client_max_memory_body_size_bytes == 131072);
    CHECK(cfg->http.idle_connection_timeout_seconds == 120);
}

TEST_CASE("unknown http key rejected") {
    TempDir tmp;
    auto file = tmp.path / "unknown_http.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "http": { "bad_key": 42 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("Unknown http config key: bad_key") != std::string::npos);
}

TEST_CASE("non-positive client_max_body_size_bytes rejected") {
    TempDir tmp;
    auto file = tmp.path / "bad_body.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "http": { "client_max_body_size_bytes": 0 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("client_max_body_size_bytes must be > 0") != std::string::npos);
}

TEST_CASE("non-positive client_max_memory_body_size_bytes rejected") {
    TempDir tmp;
    auto file = tmp.path / "bad_mem.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "http": { "client_max_memory_body_size_bytes": -1 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().find("client_max_memory_body_size_bytes must be > 0") != std::string::npos);
}

TEST_CASE("idle_connection_timeout_seconds = 0 accepted") {
    TempDir tmp;
    auto file = tmp.path / "timeout_zero.json";
    REQUIRE(write_text_file(file,
        R"json({
  "model_id": "demo-model",
  "http": { "idle_connection_timeout_seconds": 0 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    REQUIRE(cfg.has_value());
    CHECK(cfg->http.idle_connection_timeout_seconds == 0);
}

TEST_CASE("startup warning for remote bind without auth") {
    zks::server::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.model_id = "demo-model";
    config.zoo_config.model_path = "/tmp/demo.gguf";

    auto warning = zks::server::startup_warning(config);
    REQUIRE(warning.has_value());
    CHECK(warning->find("non-loopback address without api_key auth enabled") != std::string::npos);
}

TEST_CASE("loopback bind does not emit startup warning") {
    zks::server::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.model_id = "demo-model";
    config.zoo_config.model_path = "/tmp/demo.gguf";

    CHECK_FALSE(zks::server::startup_warning(config).has_value());
}

TEST_CASE("authenticated remote bind does not emit startup warning") {
    zks::server::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.model_id = "demo-model";
    config.api_key = "secret";
    config.zoo_config.model_path = "/tmp/demo.gguf";

    CHECK_FALSE(zks::server::startup_warning(config).has_value());
}
