#include "server/command_tools.hpp"
#include "server/config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path =
        std::filesystem::temp_directory_path() / ("zks-config-test-" + std::to_string(stamp));
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
    ~TempDir() {
        std::filesystem::remove_all(path);
    }
};

} // namespace

TEST(ConfigTest, ValidConfigParsesCorrectly) {
    TempDir tmp;
    auto file = tmp.path / "valid.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->bind_address, "127.0.0.1");
    EXPECT_EQ(config->port, 8081);
    EXPECT_EQ(config->model_id, "demo-model");
    EXPECT_EQ(config->sessions.max_sessions, 3u);
    EXPECT_EQ(config->sessions.idle_ttl_seconds, 600u);
    EXPECT_EQ(config->model_config.model_path, "/tmp/demo.gguf");
    EXPECT_EQ(config->default_generation.max_tokens, 32);
}

TEST(ConfigTest, UnknownKeyRejected) {
    TempDir tmp;
    auto file = tmp.path / "invalid.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": { "model_path": "/tmp/demo.gguf" },
  "extra_key": true
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("Unknown server config key: extra_key"), std::string::npos);
}

TEST(ConfigTest, EmptyApiKeyRejected) {
    TempDir tmp;
    auto file = tmp.path / "empty_api_key.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "api_key": "",
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("api_key must not be empty"), std::string::npos);
}

TEST(ConfigTest, HttpDefaultsWhenSectionOmitted) {
    TempDir tmp;
    auto file = tmp.path / "no_http.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->http.client_max_body_size_bytes, 1048576);
    EXPECT_EQ(config->http.client_max_memory_body_size_bytes, 65536);
    EXPECT_EQ(config->http.idle_connection_timeout_seconds, 60);
}

TEST(ConfigTest, ToolsConfigParsesCorrectly) {
    TempDir tmp;
    auto file = tmp.path / "tools.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(cfg->tools.size(), 1u);
    EXPECT_EQ(cfg->tools[0].name, "env");
    EXPECT_EQ(cfg->tools[0].command.size(), 1u);
    EXPECT_EQ(cfg->tools[0].command[0], "/usr/bin/env");
    EXPECT_TRUE(cfg->tools[0].inherit_environment);
    EXPECT_EQ(cfg->tools[0].timeout_ms, 1500);
    EXPECT_EQ(cfg->tools[0].env.at("ZKS_TEST_FLAG"), "1");
}

TEST(ConfigTest, InheritEnvironmentDefaultsFalse) {
    TempDir tmp;
    auto file = tmp.path / "default_env.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_TRUE(cfg.has_value());
    ASSERT_EQ(cfg->tools.size(), 1u);
    EXPECT_FALSE(cfg->tools[0].inherit_environment);
}

TEST(ConfigTest, MissingToolExecutableRejectedAtProviderCreation) {
    TempDir tmp;
    auto file = tmp.path / "missing_tool.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_TRUE(cfg.has_value());
    auto provider = zks::server::make_command_tool_provider(cfg->tools);
    ASSERT_FALSE(provider.has_value());
    EXPECT_NE(provider.error().find("executable not found"), std::string::npos);
}

TEST(ConfigTest, BadToolSchemaRejected) {
    TempDir tmp;
    auto file = tmp.path / "bad_schema.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("unsupported type: object"), std::string::npos);
}

TEST(ConfigTest, ExplicitHttpOverrides) {
    TempDir tmp;
    auto file = tmp.path / "http_override.json";
    ASSERT_TRUE(write_text_file(file,
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
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->http.client_max_body_size_bytes, 2097152);
    EXPECT_EQ(cfg->http.client_max_memory_body_size_bytes, 131072);
    EXPECT_EQ(cfg->http.idle_connection_timeout_seconds, 120);
}

TEST(ConfigTest, UnknownHttpKeyRejected) {
    TempDir tmp;
    auto file = tmp.path / "unknown_http.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "http": { "bad_key": 42 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("Unknown http config key: bad_key"), std::string::npos);
}

TEST(ConfigTest, NonPositiveBodySizeRejected) {
    TempDir tmp;
    auto file = tmp.path / "bad_body.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "http": { "client_max_body_size_bytes": 0 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("client_max_body_size_bytes must be > 0"), std::string::npos);
}

TEST(ConfigTest, NonPositiveMemoryBodySizeRejected) {
    TempDir tmp;
    auto file = tmp.path / "bad_mem.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "http": { "client_max_memory_body_size_bytes": -1 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    ASSERT_FALSE(cfg.has_value());
    EXPECT_NE(cfg.error().find("client_max_memory_body_size_bytes must be > 0"), std::string::npos);
}

TEST(ConfigTest, IdleTimeoutZeroAccepted) {
    TempDir tmp;
    auto file = tmp.path / "timeout_zero.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "http": { "idle_connection_timeout_seconds": 0 },
  "zoo": { "model_path": "/tmp/demo.gguf" }
})json"));

    auto cfg = zks::server::load_config(file);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->http.idle_connection_timeout_seconds, 0);
}

TEST(ConfigTest, StartupWarningForRemoteBindWithoutAuth) {
    zks::server::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.model_id = "demo-model";
    config.model_config.model_path = "/tmp/demo.gguf";

    auto warning = zks::server::startup_warning(config);
    ASSERT_TRUE(warning.has_value());
    EXPECT_NE(warning->find("non-loopback address without api_key auth enabled"),
              std::string::npos);
}

TEST(ConfigTest, LoopbackBindNoWarning) {
    zks::server::ServerConfig config;
    config.bind_address = "127.0.0.1";
    config.model_id = "demo-model";
    config.model_config.model_path = "/tmp/demo.gguf";

    EXPECT_FALSE(zks::server::startup_warning(config).has_value());
}

TEST(ConfigTest, AuthenticatedRemoteBindNoWarning) {
    zks::server::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.model_id = "demo-model";
    config.api_key = "secret";
    config.model_config.model_path = "/tmp/demo.gguf";

    EXPECT_FALSE(zks::server::startup_warning(config).has_value());
}

TEST(ConfigTest, ZooBlockPopulatesSplitConfigTypes) {
    TempDir tmp;
    auto file = tmp.path / "split.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": {
    "model_path": "/tmp/demo.gguf",
    "context_size": 4096,
    "n_gpu_layers": 8,
    "use_mmap": false,
    "max_history_messages": 32,
    "request_queue_capacity": 16,
    "max_tokens": 64,
    "system_prompt": "Be helpful.",
    "sampling": { "temperature": 0.5 }
  }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->model_config.model_path, "/tmp/demo.gguf");
    EXPECT_EQ(config->model_config.context_size, 4096);
    EXPECT_EQ(config->model_config.n_gpu_layers, 8);
    EXPECT_FALSE(config->model_config.use_mmap);
    EXPECT_EQ(config->agent_config.max_history_messages, 32u);
    EXPECT_EQ(config->agent_config.request_queue_capacity, 16u);
    EXPECT_EQ(config->default_generation.max_tokens, 64);
    EXPECT_FLOAT_EQ(config->default_generation.sampling.temperature, 0.5f);
    EXPECT_EQ(config->system_prompt, "Be helpful.");
}

TEST(ConfigTest, UnknownZooKeyRejected) {
    TempDir tmp;
    auto file = tmp.path / "bad_zoo.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": {
    "model_path": "/tmp/demo.gguf",
    "bogus_key": true
  }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("Unknown"), std::string::npos);
    EXPECT_NE(config.error().find("bogus_key"), std::string::npos);
}
