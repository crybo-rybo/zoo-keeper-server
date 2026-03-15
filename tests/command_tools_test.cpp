#include "server/command_tools.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

zks::server::CommandToolConfig make_tool(std::string helper_path, std::string mode) {
    zks::server::CommandToolConfig tool;
    tool.name = "helper";
    tool.description = "Helper tool for tests.";
    tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"value"})},
        {"additionalProperties", false},
    };
    tool.command = {std::move(helper_path), std::move(mode)};
    tool.timeout_ms = 100;
    return tool;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("Expected helper executable path argument.");
    }

    const std::string helper_path = argv[1];

    {
        auto tool = make_tool(helper_path, "echo");
        tool.timeout_ms = 500;
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (!result) {
            std::cerr << "Echo tool unexpectedly failed: " << result.error().message << '\n';
            return 1;
        }
        if (!result->contains("received") || (*result)["received"]["value"] != "zoo") {
            return fail("Echo tool returned unexpected JSON.");
        }
    }

    {
        auto tool = make_tool(helper_path, "invalid-json");
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (result) {
            return fail("invalid-json tool unexpectedly succeeded.");
        }
        if (result.error().message.find("valid JSON") == std::string::npos) {
            return fail("invalid-json tool failed for the wrong reason.");
        }
    }

    {
        auto tool = make_tool(helper_path, "non-object");
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (result) {
            return fail("non-object tool unexpectedly succeeded.");
        }
        if (result.error().message.find("JSON object") == std::string::npos) {
            return fail("non-object tool failed for the wrong reason.");
        }
    }

    {
        auto tool = make_tool(helper_path, "stderr-fail");
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (result) {
            return fail("stderr-fail tool unexpectedly succeeded.");
        }
        if (result.error().message.find("status 17") == std::string::npos ||
            result.error().message.find("tool helper failed on purpose") == std::string::npos) {
            return fail("stderr-fail tool failed for the wrong reason.");
        }
    }

    {
        auto tool = make_tool(helper_path, "timeout");
        tool.timeout_ms = 10;
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (result) {
            return fail("timeout tool unexpectedly succeeded.");
        }
        if (!result.error().timed_out ||
            result.error().message.find("timed out") == std::string::npos) {
            return fail("timeout tool did not report a timeout correctly.");
        }
    }

    {
        auto tool = make_tool(helper_path, "large-stdout");
        tool.timeout_ms = 500;
        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (result) {
            return fail("large-stdout tool unexpectedly succeeded.");
        }
        if (result.error().message.find("stdout exceeded") == std::string::npos) {
            return fail("large-stdout tool failed for the wrong reason.");
        }
    }

    {
        auto tool = make_tool(helper_path, "echo");
        auto provider = zks::server::make_command_tool_provider({tool});
        if (!provider) {
            std::cerr << "Tool provider unexpectedly failed: " << provider.error() << '\n';
            return 1;
        }
        if (provider->metadata.size() != 1 || provider->metadata[0].name != "helper" ||
            provider->metadata[0].parameters_schema != tool.parameters_schema) {
            return fail("Tool provider metadata did not match the tool config.");
        }
    }

    {
        ::setenv("ZKS_PARENT_SECRET", "top-secret", 1);

        auto tool = make_tool(helper_path, "env-report");
        tool.timeout_ms = 500;
        tool.env.emplace("ZKS_TOOL_FLAG", "present");

        auto result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (!result) {
            std::cerr << "env-report tool unexpectedly failed: " << result.error().message << '\n';
            return 1;
        }
        if ((*result)["parent_secret"] != "<missing>" || (*result)["tool_flag"] != "present") {
            return fail("Tool environment was not scrubbed by default.");
        }

        tool.inherit_environment = true;
        result = zks::server::run_command_tool(tool, nlohmann::json{{"value", "zoo"}});
        if (!result) {
            std::cerr << "env-report inherited tool unexpectedly failed: "
                      << result.error().message << '\n';
            return 1;
        }
        if ((*result)["parent_secret"] != "top-secret" || (*result)["tool_flag"] != "present") {
            return fail("Tool environment inheritance did not behave as expected.");
        }
    }

    return 0;
}
