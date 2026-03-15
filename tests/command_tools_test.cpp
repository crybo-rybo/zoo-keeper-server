#include "server/command_tools.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

static std::string g_helper_path;

int main(int argc, char** argv) {
    // Extract helper path before GTest consumes args
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            g_helper_path = argv[i];
            break;
        }
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
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

TEST(CommandToolsTest, EchoToolReturnsExpectedJson) {
    auto tool = make_tool("echo");
    tool.timeout_ms = 500;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->contains("received"));
    EXPECT_EQ((*result)["received"]["value"], "zoo");
}

TEST(CommandToolsTest, InvalidJsonToolFails) {
    auto tool = make_tool("invalid-json");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("valid JSON"), std::string::npos);
}

TEST(CommandToolsTest, NonObjectToolFails) {
    auto tool = make_tool("non-object");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("JSON object"), std::string::npos);
}

TEST(CommandToolsTest, StderrFailReportsExitStatus) {
    auto tool = make_tool("stderr-fail");
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("status 17"), std::string::npos);
    EXPECT_NE(result.error().message.find("tool helper failed on purpose"), std::string::npos);
}

TEST(CommandToolsTest, TimeoutToolReportsTimeout) {
    auto tool = make_tool("timeout");
    tool.timeout_ms = 10;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().timed_out);
    EXPECT_NE(result.error().message.find("timed out"), std::string::npos);
}

TEST(CommandToolsTest, LargeStdoutToolFails) {
    auto tool = make_tool("large-stdout");
    tool.timeout_ms = 500;
    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("stdout exceeded"), std::string::npos);
}

TEST(CommandToolsTest, ToolProviderMetadataMatchesConfig) {
    auto tool = make_tool("echo");
    auto provider = zks::server::make_command_tool_provider({tool});
    ASSERT_TRUE(provider.has_value());
    ASSERT_EQ(provider->tools.size(), 1u);
    EXPECT_EQ(provider->tools[0].definition.name, "helper");
    EXPECT_EQ(provider->tools[0].definition.parameters_schema, tool.parameters_schema);
}

TEST(CommandToolsTest, ToolEnvironmentIsolation) {
    ::setenv("ZKS_PARENT_SECRET", "top-secret", 1);

    auto tool = make_tool("env-report");
    tool.timeout_ms = 500;
    tool.env.emplace("ZKS_TOOL_FLAG", "present");

    auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["parent_secret"], "<missing>");
    EXPECT_EQ((*result)["tool_flag"], "present");

    tool.inherit_environment = true;
    result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["parent_secret"], "top-secret");
    EXPECT_EQ((*result)["tool_flag"], "present");
}
