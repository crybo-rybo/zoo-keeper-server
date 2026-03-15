#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server/command_tools.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace zks::server {
namespace {

constexpr size_t kMaxStdoutBytes = 64 * 1024;
constexpr size_t kMaxCapturedStderrBytes = 8 * 1024;
constexpr std::chrono::milliseconds kPollInterval{10};
constexpr std::string_view kTimeoutContext = "timeout";

struct CapturedStream {
    std::string data;
    bool truncated = false;
    std::optional<std::string> error;
};

struct PreparedCommandTool {
    CommandToolConfig config;
    std::filesystem::path executable_path;
    std::vector<std::string> env_entries;
};

std::filesystem::path command_base_directory(const CommandToolConfig& tool) {
    if (tool.working_directory.has_value()) {
        return *tool.working_directory;
    }
    return std::filesystem::current_path();
}

bool contains_path_separator(std::string_view value) {
    return value.find('/') != std::string_view::npos;
}

bool is_executable_file(const std::filesystem::path& path) {
    return ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::string> effective_path_env(const CommandToolConfig& tool) {
    if (tool.inherit_environment) {
        const char* path = std::getenv("PATH");
        if (path && *path != '\0') {
            return std::string(path);
        }
    }

    if (auto it = tool.env.find("PATH"); it != tool.env.end()) {
        return it->second;
    }

    if (const char* path = std::getenv("PATH"); path && *path != '\0') {
        return std::string(path);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> resolve_executable_path(const CommandToolConfig& tool) {
    if (tool.command.empty() || tool.command.front().empty()) {
        return std::nullopt;
    }

    const auto& program = tool.command.front();
    if (contains_path_separator(program)) {
        std::filesystem::path candidate(program);
        if (candidate.is_relative()) {
            candidate = command_base_directory(tool) / candidate;
        }
        if (is_executable_file(candidate)) {
            return std::filesystem::weakly_canonical(candidate);
        }
        return std::nullopt;
    }

    const auto path_env = effective_path_env(tool);
    if (!path_env.has_value()) {
        return std::nullopt;
    }

    const auto base_dir = command_base_directory(tool);
    size_t start = 0;
    while (start <= path_env->size()) {
        const size_t end = path_env->find(':', start);
        std::string entry =
            path_env->substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::filesystem::path directory =
            entry.empty() ? base_dir : std::filesystem::path(entry);
        if (directory.is_relative()) {
            directory = base_dir / directory;
        }

        const auto candidate = directory / program;
        if (is_executable_file(candidate)) {
            return std::filesystem::weakly_canonical(candidate);
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return std::nullopt;
}

Result<void> reject_unknown_keys(const nlohmann::json& json, std::string_view context,
                                 std::initializer_list<std::string_view> allowed_keys) {
    if (!json.is_object()) {
        return std::unexpected(std::string(context) + " must be a JSON object");
    }

    for (auto it = json.begin(); it != json.end(); ++it) {
        bool allowed = false;
        for (const auto key : allowed_keys) {
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

Result<void> validate_enum_value_type(const nlohmann::json& value, std::string_view expected_type,
                                      std::string_view property_name) {
    const bool valid = (expected_type == "string" && value.is_string()) ||
                       (expected_type == "integer" && value.is_number_integer()) ||
                       (expected_type == "number" && value.is_number()) ||
                       (expected_type == "boolean" && value.is_boolean());
    if (!valid) {
        return std::unexpected("tools.parameters enum value for property '" +
                               std::string(property_name) + "' does not match declared type '" +
                               std::string(expected_type) + "'");
    }
    return {};
}

Result<void> validate_property_schema(std::string_view property_name, const nlohmann::json& schema) {
    if (auto unknown = reject_unknown_keys(schema, "tools.parameters property",
                                           {"type", "description", "enum"});
        !unknown) {
        return std::unexpected(unknown.error());
    }

    if (!schema.contains("type") || !schema.at("type").is_string()) {
        return std::unexpected("tools.parameters property '" + std::string(property_name) +
                               "' must include a string type");
    }

    const auto type = schema.at("type").get<std::string>();
    if (type != "string" && type != "integer" && type != "number" && type != "boolean") {
        return std::unexpected("tools.parameters property '" + std::string(property_name) +
                               "' uses unsupported type: " + type);
    }

    if (auto it = schema.find("description"); it != schema.end() && !it->is_string()) {
        return std::unexpected("tools.parameters property '" + std::string(property_name) +
                               "' description must be a string");
    }

    if (auto it = schema.find("enum"); it != schema.end()) {
        if (!it->is_array()) {
            return std::unexpected("tools.parameters property '" + std::string(property_name) +
                                   "' enum must be an array");
        }
        for (const auto& value : *it) {
            if (auto valid = validate_enum_value_type(value, type, property_name); !valid) {
                return std::unexpected(valid.error());
            }
        }
    }

    return {};
}

std::string format_stderr(std::string stderr_text, bool truncated) {
    if (stderr_text.empty()) {
        return {};
    }
    if (truncated) {
        stderr_text += "... [truncated]";
    }
    return " | stderr: " + stderr_text;
}

CapturedStream read_stream_until_eof(int fd, size_t max_bytes) {
    CapturedStream captured;
    std::array<char, 4096> buffer{};

    while (true) {
        const auto bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            captured.error = std::string("read failed: ") + std::strerror(errno);
            break;
        }

        const size_t chunk_size = static_cast<size_t>(bytes_read);
        const size_t previous_size = captured.data.size();
        if (previous_size < max_bytes) {
            const size_t append_size = std::min(max_bytes - previous_size, chunk_size);
            captured.data.append(buffer.data(), append_size);
        }
        if (previous_size + chunk_size > max_bytes) {
            captured.truncated = true;
        }
    }

    ::close(fd);
    return captured;
}

CommandToolRunResult make_timeout_error(std::uint32_t timeout_ms) {
    return std::unexpected(CommandToolRunError{
        "Tool command timed out after " + std::to_string(timeout_ms) + " ms",
        true,
    });
}

CommandToolRunResult make_runtime_error(std::string message) {
    return std::unexpected(CommandToolRunError{std::move(message), false});
}

std::map<std::string, std::string, std::less<>> build_environment_map(
    const CommandToolConfig& tool) {
    std::map<std::string, std::string, std::less<>> environment;
    if (tool.inherit_environment) {
        for (char** env = environ; env && *env; ++env) {
            std::string_view entry(*env);
            const auto separator = entry.find('=');
            if (separator == std::string_view::npos) {
                continue;
            }
            environment.emplace(std::string(entry.substr(0, separator)),
                                std::string(entry.substr(separator + 1)));
        }
    }

    for (const auto& [key, value] : tool.env) {
        environment[key] = value;
    }
    return environment;
}

PreparedCommandTool prepare_command_tool(const CommandToolConfig& tool) {
    PreparedCommandTool prepared{.config = tool};
    auto executable = resolve_executable_path(tool);
    if (!executable.has_value()) {
        throw std::runtime_error("tools.command executable not found or not executable: " +
                                 tool.command.front());
    }
    prepared.executable_path = std::move(*executable);

    auto environment = build_environment_map(tool);
    prepared.env_entries.reserve(environment.size());
    for (const auto& [key, value] : environment) {
        prepared.env_entries.push_back(key + "=" + value);
    }
    return prepared;
}

Result<PreparedCommandTool> build_prepared_command_tool(const CommandToolConfig& tool) {
    if (auto validation = tool.validate(); !validation) {
        return std::unexpected(validation.error());
    }

#if !defined(__APPLE__) && !defined(__linux__)
    if (tool.working_directory.has_value()) {
        return std::unexpected("tools.working_directory is not supported on this platform");
    }
#endif

    try {
        return prepare_command_tool(tool);
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}

CommandToolRunResult run_prepared_command_tool(const PreparedCommandTool& prepared,
                                               const nlohmann::json& arguments) {
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    const auto close_pipe = [](int (&pipe_fds)[2]) {
        for (int& fd : pipe_fds) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    };

    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error(std::string("Failed to create tool process pipes: ") +
                                  std::strerror(errno));
    }

    posix_spawn_file_actions_t file_actions;
    if (const int error = posix_spawn_file_actions_init(&file_actions); error != 0) {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error(std::string("Failed to initialize spawn file actions: ") +
                                  std::strerror(error));
    }
    const auto destroy_file_actions = [&file_actions]() { posix_spawn_file_actions_destroy(&file_actions); };

    posix_spawnattr_t spawn_attr;
    if (const int error = posix_spawnattr_init(&spawn_attr); error != 0) {
        destroy_file_actions();
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error(std::string("Failed to initialize spawn attributes: ") +
                                  std::strerror(error));
    }
    const auto destroy_spawn_attr = [&spawn_attr]() { posix_spawnattr_destroy(&spawn_attr); };

    short spawn_flags = 0;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    if (spawn_flags != 0) {
        if (const int error = posix_spawnattr_setflags(&spawn_attr, spawn_flags); error != 0) {
            destroy_spawn_attr();
            destroy_file_actions();
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            close_pipe(stderr_pipe);
            return make_runtime_error(std::string("Failed to configure spawn attributes: ") +
                                      std::strerror(error));
        }
    }

    auto add_action = [&](int error, std::string_view message) -> CommandToolRunResult {
        if (error == 0) {
            return nlohmann::json::object();
        }
        destroy_spawn_attr();
        destroy_file_actions();
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error(std::string(message) + ": " + std::strerror(error));
    };

    if (auto result =
            add_action(posix_spawn_file_actions_adddup2(&file_actions, stdin_pipe[0], STDIN_FILENO),
                       "Failed to wire tool stdin");
        !result) {
        return result;
    }
    if (auto result = add_action(
            posix_spawn_file_actions_adddup2(&file_actions, stdout_pipe[1], STDOUT_FILENO),
            "Failed to wire tool stdout");
        !result) {
        return result;
    }
    if (auto result = add_action(
            posix_spawn_file_actions_adddup2(&file_actions, stderr_pipe[1], STDERR_FILENO),
            "Failed to wire tool stderr");
        !result) {
        return result;
    }
    if (auto result =
            add_action(posix_spawn_file_actions_addclose(&file_actions, stdin_pipe[1]),
                       "Failed to close child stdin write end");
        !result) {
        return result;
    }
    if (auto result =
            add_action(posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[0]),
                       "Failed to close child stdout read end");
        !result) {
        return result;
    }
    if (auto result =
            add_action(posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[0]),
                       "Failed to close child stderr read end");
        !result) {
        return result;
    }

    if (prepared.config.working_directory.has_value()) {
#if defined(__APPLE__) || defined(__linux__)
        if (auto result = add_action(
                posix_spawn_file_actions_addchdir_np(&file_actions,
                                                     prepared.config.working_directory->c_str()),
                "Failed to configure child working directory");
            !result) {
            return result;
        }
#else
        destroy_spawn_attr();
        destroy_file_actions();
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error("tools.working_directory is not supported on this platform");
#endif
    }

    std::vector<char*> argv;
    argv.reserve(prepared.config.command.size() + 1);
    for (const auto& arg : prepared.config.command) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<char*> envp;
    envp.reserve(prepared.env_entries.size() + 1);
    for (const auto& entry : prepared.env_entries) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int spawn_error =
        ::posix_spawn(&pid, prepared.executable_path.c_str(), &file_actions, &spawn_attr,
                      argv.data(), envp.data());
    destroy_spawn_attr();
    destroy_file_actions();

    if (spawn_error != 0) {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return make_runtime_error(std::string("Failed to spawn tool process: ") +
                                  std::strerror(spawn_error));
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    stdin_pipe[0] = -1;
    stdout_pipe[1] = -1;
    stderr_pipe[1] = -1;

    auto stdout_future = std::async(std::launch::async, [fd = stdout_pipe[0]] {
        return read_stream_until_eof(fd, kMaxStdoutBytes);
    });
    auto stderr_future = std::async(std::launch::async, [fd = stderr_pipe[0]] {
        return read_stream_until_eof(fd, kMaxCapturedStderrBytes);
    });

    std::string stdin_payload = arguments.dump();
    stdin_payload.push_back('\n');
    size_t offset = 0;
    while (offset < stdin_payload.size()) {
        const auto written = ::write(stdin_pipe[1], stdin_payload.data() + offset,
                                     stdin_payload.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EPIPE) {
                ::close(stdin_pipe[1]);
                stdin_pipe[1] = -1;
                ::kill(pid, SIGKILL);
                int status = 0;
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                stdout_future.wait();
                stderr_future.wait();
                return make_runtime_error(std::string("Failed to write tool stdin: ") +
                                          std::strerror(errno));
            }
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (stdin_pipe[1] >= 0) {
        ::close(stdin_pipe[1]);
    }

    int status = 0;
    bool timed_out = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(prepared.config.timeout_ms);
    while (true) {
        const pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            break;
        }
        if (wait_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            stdout_future.wait();
            stderr_future.wait();
            return make_runtime_error(std::string("Failed while waiting on tool process: ") +
                                      std::strerror(errno));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    const auto stdout_capture = stdout_future.get();
    const auto stderr_capture = stderr_future.get();
    if (stdout_capture.error.has_value()) {
        return make_runtime_error("Failed to read tool stdout: " + *stdout_capture.error);
    }
    if (stderr_capture.error.has_value()) {
        return make_runtime_error("Failed to read tool stderr: " + *stderr_capture.error);
    }
    if (timed_out) {
        return make_timeout_error(prepared.config.timeout_ms);
    }
    if (stdout_capture.truncated) {
        return make_runtime_error("Tool command stdout exceeded " +
                                  std::to_string(kMaxStdoutBytes) + " bytes");
    }

    if (WIFSIGNALED(status)) {
        return make_runtime_error("Tool command terminated by signal " +
                                  std::to_string(WTERMSIG(status)) +
                                  format_stderr(stderr_capture.data, stderr_capture.truncated));
    }
    if (!WIFEXITED(status)) {
        return make_runtime_error("Tool command ended unexpectedly" +
                                  format_stderr(stderr_capture.data, stderr_capture.truncated));
    }
    if (WEXITSTATUS(status) != 0) {
        return make_runtime_error("Tool command exited with status " +
                                  std::to_string(WEXITSTATUS(status)) +
                                  format_stderr(stderr_capture.data, stderr_capture.truncated));
    }

    nlohmann::json output;
    try {
        output = nlohmann::json::parse(stdout_capture.data);
    } catch (const std::exception& error) {
        return make_runtime_error(std::string("Tool command stdout was not valid JSON: ") +
                                  error.what());
    }
    if (!output.is_object()) {
        return make_runtime_error("Tool command stdout must be a JSON object");
    }
    return output;
}

} // namespace

Result<void> validate_tool_parameters_schema(const nlohmann::json& schema) {
    if (auto unknown = reject_unknown_keys(schema, "tools.parameters",
                                           {"type", "properties", "required",
                                            "additionalProperties"});
        !unknown) {
        return std::unexpected(unknown.error());
    }

    if (!schema.contains("type") || !schema.at("type").is_string() ||
        schema.at("type").get<std::string>() != "object") {
        return std::unexpected("tools.parameters must declare top-level type 'object'");
    }
    if (!schema.contains("properties") || !schema.at("properties").is_object()) {
        return std::unexpected("tools.parameters must include an object-valued properties map");
    }

    const auto& properties = schema.at("properties");
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!it.value().is_object()) {
            return std::unexpected("tools.parameters property '" + it.key() +
                                   "' must be a JSON object");
        }
        if (auto valid = validate_property_schema(it.key(), it.value()); !valid) {
            return std::unexpected(valid.error());
        }
    }

    if (auto it = schema.find("required"); it != schema.end()) {
        if (!it->is_array()) {
            return std::unexpected("tools.parameters required must be an array");
        }
        for (const auto& value : *it) {
            if (!value.is_string()) {
                return std::unexpected("tools.parameters required entries must be strings");
            }
            if (!properties.contains(value.get<std::string>())) {
                return std::unexpected("tools.parameters required references unknown property: " +
                                       value.get<std::string>());
            }
        }
    }

    if (auto it = schema.find("additionalProperties"); it != schema.end()) {
        if (!it->is_boolean() || it->get<bool>()) {
            return std::unexpected(
                "tools.parameters only supports additionalProperties omitted or false");
        }
    }
    return {};
}

Result<void> CommandToolConfig::validate() const {
    if (name.empty()) {
        return std::unexpected("tools.name must not be empty");
    }
    if (description.empty()) {
        return std::unexpected("tools.description must not be empty");
    }
    if (command.empty()) {
        return std::unexpected("tools.command must contain at least one argument");
    }
    for (const auto& arg : command) {
        if (arg.empty()) {
            return std::unexpected("tools.command entries must not be empty");
        }
    }
    if (timeout_ms == 0) {
        return std::unexpected("tools.timeout_ms must be >= 1");
    }
    if (auto schema_validation = validate_tool_parameters_schema(parameters_schema);
        !schema_validation) {
        return std::unexpected(schema_validation.error());
    }
    if (working_directory.has_value()) {
        std::error_code error;
        const bool is_dir = std::filesystem::is_directory(*working_directory, error);
        if (error || !is_dir) {
            return std::unexpected("tools.working_directory must point to an existing directory");
        }
    }
    for (const auto& [key, value] : env) {
        if (key.empty()) {
            return std::unexpected("tools.env keys must not be empty");
        }
        if (key.find('=') != std::string::npos) {
            return std::unexpected("tools.env keys must not contain '='");
        }
        (void)value;
    }
    if (!resolve_executable_path(*this).has_value()) {
        return std::unexpected("tools.command executable not found or not executable: " +
                               command.front());
    }
    return {};
}

CommandToolRunResult run_command_tool(const CommandToolConfig& tool, const nlohmann::json& arguments) {
    auto prepared = build_prepared_command_tool(tool);
    if (!prepared) {
        return make_runtime_error(prepared.error());
    }
    return run_prepared_command_tool(*prepared, arguments);
}

Result<ToolProvider> make_command_tool_provider(const std::vector<CommandToolConfig>& tools) {
    std::vector<PreparedCommandTool> prepared_tools;
    prepared_tools.reserve(tools.size());

    ToolProvider provider;
    provider.metadata.reserve(tools.size());

    for (const auto& tool : tools) {
        auto prepared = build_prepared_command_tool(tool);
        if (!prepared) {
            return std::unexpected("Invalid tool '" + tool.name + "': " + prepared.error());
        }

        provider.metadata.push_back(zoo::tools::ToolMetadata{tool.name, tool.description,
                                                             tool.parameters_schema, {}});
        prepared_tools.push_back(std::move(*prepared));
    }

    provider.install = [prepared_tools = std::move(prepared_tools)](zoo::Agent& agent) -> Result<void> {
        for (const auto& prepared : prepared_tools) {
            auto register_result = agent.register_tool(
                prepared.config.name, prepared.config.description, prepared.config.parameters_schema,
                [prepared](const nlohmann::json& arguments) -> zoo::Expected<nlohmann::json> {
                    auto result = run_prepared_command_tool(prepared, arguments);
                    if (!result) {
                        return std::unexpected(zoo::Error{
                            zoo::ErrorCode::ToolExecutionFailed,
                            result.error().message,
                            result.error().timed_out ? std::optional<std::string>(kTimeoutContext)
                                                     : std::nullopt,
                        });
                    }
                    return *result;
                });
            if (!register_result) {
                return std::unexpected("Failed to register tool '" + prepared.config.name +
                                       "': " + register_result.error().to_string());
            }
        }
        return {};
    };

    return provider;
}

} // namespace zks::server
