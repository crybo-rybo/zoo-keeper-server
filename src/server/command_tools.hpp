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

[[nodiscard]] Result<void> validate_tool_parameters_schema(const nlohmann::json& schema);
[[nodiscard]] CommandToolRunResult run_command_tool(const CommandToolConfig& tool,
                                                    const nlohmann::json& arguments);
[[nodiscard]] Result<ToolProvider>
make_command_tool_provider(const std::vector<CommandToolConfig>& tools);

} // namespace zks::server
