#pragma once

#include "server/chat_service.hpp"
#include "server/config.hpp"
#include "server/result.hpp"

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zks::server {

struct CommandToolRunError {
    std::string message;
    bool timed_out = false;
};

using CommandToolRunResult = std::expected<nlohmann::json, CommandToolRunError>;

/// Validates the JSON schema structure of tool parameters (types, enum values, etc.).
/// This is a semantic check on the schema definition itself, independent of filesystem state.
[[nodiscard]] Result<void> validate_tool_parameters_schema(const nlohmann::json& schema);

[[nodiscard]] CommandToolRunResult run_command_tool(const CommandToolConfig& tool,
                                                    const nlohmann::json& arguments);

/// Validates each tool config and resolves filesystem state (executable path, working
/// directory existence). Config-level validation (CommandToolConfig::validate()) runs
/// at parse time; filesystem checks run here at startup to fail fast before serving.
[[nodiscard]] Result<ToolProvider>
make_command_tool_provider(const std::vector<CommandToolConfig>& tools);

} // namespace zks::server
