#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "server/command_tools.hpp"

#include <cstdlib>
#include <string>

static std::string g_helper_path;

int main(int argc, char** argv) {
    // Extract helper path before doctest consumes args
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            g_helper_path = argv[i];
            break;
        }
    }

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}

namespace {

zks::server::CommandToolConfig make_tool(std::string mode) {
    zks::server::CommandToolConfig tool;
    tool.name = "helper";
    tool.description = "Helper tool for tests.";
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"value"})},
        {"additionalProperties", false},
    };
    tool.command = {g_helper_path, std::move(mode)};
    tool.timeout_ms = 100;
    return tool;
}

} // namespace

TEST_CASE("echo tool returns expected JSON") {
    auto tool = make_tool("echo");
    tool.timeout_ms = 500;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE(result.has_value());
    CHECK(result->contains("received"));
    CHECK((*result)["received"]["value"] == "zoo");
}

TEST_CASE("invalid-json tool fails") {
    auto tool = make_tool("invalid-json");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("valid JSON") != std::string::npos);
}

TEST_CASE("non-object tool fails") {
    auto tool = make_tool("non-object");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("JSON object") != std::string::npos);
}

TEST_CASE("stderr-fail tool reports exit status") {
    auto tool = make_tool("stderr-fail");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("status 17") != std::string::npos);
    CHECK(result.error().message.find("tool helper failed on purpose") != std::string::npos);
}

TEST_CASE("timeout tool reports timeout") {
    auto tool = make_tool("timeout");
    tool.timeout_ms = 10;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().timed_out);
    CHECK(result.error().message.find("timed out") != std::string::npos);
}

TEST_CASE("large-stdout tool fails") {
    auto tool = make_tool("large-stdout");
    tool.timeout_ms = 500;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("stdout exceeded") != std::string::npos);
}

TEST_CASE("tool provider metadata matches config") {
    auto tool = make_tool("echo");
    auto provider = zks::server::make_command_tool_provider({tool});
    REQUIRE(provider.has_value());
    REQUIRE(provider->tools.size() == 1);
    CHECK(provider->tools[0].definition.name == "helper");
    CHECK(provider->tools[0].definition.parameters_schema == tool.parameters_schema);
}

TEST_CASE("tool environment isolation") {
    ::setenv("ZKS_PARENT_SECRET", "top-secret", 1);

    auto tool = make_tool("env-report");
    tool.timeout_ms = 500;
    tool.env.emplace("ZKS_TOOL_FLAG", "present");

    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE(result.has_value());
    CHECK((*result)["parent_secret"] == "<missing>");
    CHECK((*result)["tool_flag"] == "present");

    tool.inherit_environment = true;
    result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    REQUIRE(result.has_value());
    CHECK((*result)["parent_secret"] == "top-secret");
    CHECK((*result)["tool_flag"] == "present");
}
