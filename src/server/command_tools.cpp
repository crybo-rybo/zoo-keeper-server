#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server/command_tools.hpp"

#include "server/internal_utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace zks::server {
namespace {

constexpr size_t kMaxStdoutBytes = 64 * 1024;
constexpr size_t kMaxCapturedStderrBytes = 8 * 1024;
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
        std::filesystem::path directory = entry.empty() ? base_dir : std::filesystem::path(entry);
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

Result<void> validate_property_schema(std::string_view property_name,
                                      const nlohmann::json& schema) {
    static constexpr std::array<std::string_view, 3> kAllowedPropertyKeys = {"type", "description",
                                                                             "enum"};
    if (auto unknown =
            reject_unknown_keys(schema, "tools.parameters property", kAllowedPropertyKeys);
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

Result<void> set_non_blocking(int fd, std::string_view label) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected("Failed to inspect " + std::string(label) + ": " +
                               std::strerror(errno));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return std::unexpected("Failed to make " + std::string(label) +
                               " non-blocking: " + std::strerror(errno));
    }
    return {};
}

Result<bool> read_available(int fd, CapturedStream& captured, size_t max_bytes) {
    std::array<char, 4096> buffer{};

    while (true) {
        const auto bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read == 0) {
            return false;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return std::unexpected(std::string("read failed: ") + std::strerror(errno));
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

std::map<std::string, std::string, std::less<>>
build_environment_map(const CommandToolConfig& tool) {
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

/// Validates config semantics then checks filesystem state (executable exists, working
/// directory exists). The split from CommandToolConfig::validate() is intentional:
/// validate() checks field values/schema at parse time, this function checks runtime
/// filesystem preconditions at startup.
Result<PreparedCommandTool> build_prepared_command_tool(const CommandToolConfig& tool) {
    if (auto validation = tool.validate(); !validation) {
        return std::unexpected(validation.error());
    }

    if (tool.working_directory.has_value()) {
        std::error_code error;
        const bool is_dir = std::filesystem::is_directory(*tool.working_directory, error);
        if (error || !is_dir) {
            return std::unexpected("tools.working_directory must point to an existing directory");
        }
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

// --- RAII wrappers for POSIX resources ---

class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        reset();
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }
    explicit operator bool() const noexcept {
        return valid();
    }

    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_;
};

struct ScopedPipe {
    ScopedFd read_end;
    ScopedFd write_end;

    static Result<ScopedPipe> create() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            return std::unexpected(std::string("Failed to create pipe: ") + std::strerror(errno));
        }
        return ScopedPipe{ScopedFd(fds[0]), ScopedFd(fds[1])};
    }
};

class ScopedSpawnFileActions {
  public:
    ScopedSpawnFileActions() noexcept = default;
    ~ScopedSpawnFileActions() {
        if (initialized_) {
            posix_spawn_file_actions_destroy(&actions_);
        }
    }

    ScopedSpawnFileActions(const ScopedSpawnFileActions&) = delete;
    ScopedSpawnFileActions& operator=(const ScopedSpawnFileActions&) = delete;
    ScopedSpawnFileActions(ScopedSpawnFileActions&&) = delete;
    ScopedSpawnFileActions& operator=(ScopedSpawnFileActions&&) = delete;

    [[nodiscard]] int init() {
        int err = posix_spawn_file_actions_init(&actions_);
        if (err == 0) {
            initialized_ = true;
        }
        return err;
    }

    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept {
        return &actions_;
    }

  private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_ = false;
};

class ScopedSpawnAttr {
  public:
    ScopedSpawnAttr() noexcept = default;
    ~ScopedSpawnAttr() {
        if (initialized_) {
            posix_spawnattr_destroy(&attr_);
        }
    }

    ScopedSpawnAttr(const ScopedSpawnAttr&) = delete;
    ScopedSpawnAttr& operator=(const ScopedSpawnAttr&) = delete;
    ScopedSpawnAttr(ScopedSpawnAttr&&) = delete;
    ScopedSpawnAttr& operator=(ScopedSpawnAttr&&) = delete;

    [[nodiscard]] int init() {
        int err = posix_spawnattr_init(&attr_);
        if (err == 0) {
            initialized_ = true;
        }
        return err;
    }

    [[nodiscard]] posix_spawnattr_t* get() noexcept {
        return &attr_;
    }

  private:
    posix_spawnattr_t attr_{};
    bool initialized_ = false;
};

class ScopedChildProcess {
  public:
    explicit ScopedChildProcess(pid_t pid = -1) noexcept : pid_(pid) {}
    ~ScopedChildProcess() {
        kill_and_wait();
    }

    ScopedChildProcess(const ScopedChildProcess&) = delete;
    ScopedChildProcess& operator=(const ScopedChildProcess&) = delete;
    ScopedChildProcess(ScopedChildProcess&& other) noexcept
        : pid_(other.pid_), status_(other.status_), waited_(other.waited_) {
        other.pid_ = -1;
        other.waited_ = true;
    }
    ScopedChildProcess& operator=(ScopedChildProcess&&) = delete;

    [[nodiscard]] pid_t pid() const noexcept {
        return pid_;
    }
    [[nodiscard]] int status() const noexcept {
        return status_;
    }
    [[nodiscard]] bool waited() const noexcept {
        return waited_;
    }

    Result<bool> try_wait() {
        if (waited_) {
            return true;
        }
        while (true) {
            const pid_t result = ::waitpid(pid_, &status_, WNOHANG);
            if (result == pid_) {
                waited_ = true;
                return true;
            }
            if (result == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::string("Failed while waiting on tool process: ") +
                                   std::strerror(errno));
        }
    }

    void kill_and_wait() noexcept {
        if (waited_ || pid_ < 0) {
            return;
        }
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {
        }
        waited_ = true;
    }

  private:
    pid_t pid_;
    int status_ = 0;
    bool waited_ = false;
};

CommandToolRunResult run_prepared_command_tool(const PreparedCommandTool& prepared,
                                               const nlohmann::json& arguments) {
    auto stdin_pipe = ScopedPipe::create();
    if (!stdin_pipe) {
        return make_runtime_error(stdin_pipe.error());
    }
    auto stdout_pipe = ScopedPipe::create();
    if (!stdout_pipe) {
        return make_runtime_error(stdout_pipe.error());
    }
    auto stderr_pipe = ScopedPipe::create();
    if (!stderr_pipe) {
        return make_runtime_error(stderr_pipe.error());
    }

    ScopedSpawnFileActions file_actions;
    if (const int err = file_actions.init(); err != 0) {
        return make_runtime_error(std::string("Failed to initialize spawn file actions: ") +
                                  std::strerror(err));
    }

    ScopedSpawnAttr spawn_attr;
    if (const int err = spawn_attr.init(); err != 0) {
        return make_runtime_error(std::string("Failed to initialize spawn attributes: ") +
                                  std::strerror(err));
    }

    short spawn_flags = 0;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    if (spawn_flags != 0) {
        if (const int err = posix_spawnattr_setflags(spawn_attr.get(), spawn_flags); err != 0) {
            return make_runtime_error(std::string("Failed to configure spawn attributes: ") +
                                      std::strerror(err));
        }
    }

    auto add_action = [&](int err, std::string_view message) -> std::optional<std::string> {
        if (err == 0) {
            return std::nullopt;
        }
        return std::string(message) + ": " + std::strerror(err);
    };

    if (auto err = add_action(posix_spawn_file_actions_adddup2(
                                  file_actions.get(), stdin_pipe->read_end.get(), STDIN_FILENO),
                              "Failed to wire tool stdin")) {
        return make_runtime_error(std::move(*err));
    }
    if (auto err = add_action(posix_spawn_file_actions_adddup2(
                                  file_actions.get(), stdout_pipe->write_end.get(), STDOUT_FILENO),
                              "Failed to wire tool stdout")) {
        return make_runtime_error(std::move(*err));
    }
    if (auto err = add_action(posix_spawn_file_actions_adddup2(
                                  file_actions.get(), stderr_pipe->write_end.get(), STDERR_FILENO),
                              "Failed to wire tool stderr")) {
        return make_runtime_error(std::move(*err));
    }
    if (auto err = add_action(
            posix_spawn_file_actions_addclose(file_actions.get(), stdin_pipe->write_end.get()),
            "Failed to close child stdin write end")) {
        return make_runtime_error(std::move(*err));
    }
    if (auto err = add_action(
            posix_spawn_file_actions_addclose(file_actions.get(), stdout_pipe->read_end.get()),
            "Failed to close child stdout read end")) {
        return make_runtime_error(std::move(*err));
    }
    if (auto err = add_action(
            posix_spawn_file_actions_addclose(file_actions.get(), stderr_pipe->read_end.get()),
            "Failed to close child stderr read end")) {
        return make_runtime_error(std::move(*err));
    }

    if (prepared.config.working_directory.has_value()) {
#if defined(__APPLE__) || defined(__linux__)
        if (auto err =
                add_action(posix_spawn_file_actions_addchdir_np(
                               file_actions.get(), prepared.config.working_directory->c_str()),
                           "Failed to configure child working directory")) {
            return make_runtime_error(std::move(*err));
        }
#else
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
        ::posix_spawn(&pid, prepared.executable_path.c_str(), file_actions.get(), spawn_attr.get(),
                      argv.data(), envp.data());

    if (spawn_error != 0) {
        return make_runtime_error(std::string("Failed to spawn tool process: ") +
                                  std::strerror(spawn_error));
    }

    ScopedChildProcess child(pid);

    // Close child-side ends; keep parent-side ends.
    stdin_pipe->read_end.reset();
    stdout_pipe->write_end.reset();
    stderr_pipe->write_end.reset();

    if (auto result = set_non_blocking(stdin_pipe->write_end.get(), "tool stdin"); !result) {
        return make_runtime_error(result.error());
    }
    if (auto result = set_non_blocking(stdout_pipe->read_end.get(), "tool stdout"); !result) {
        return make_runtime_error(result.error());
    }
    if (auto result = set_non_blocking(stderr_pipe->read_end.get(), "tool stderr"); !result) {
        return make_runtime_error(result.error());
    }

    std::string stdin_payload = arguments.dump();
    stdin_payload.push_back('\n');
    size_t offset = 0;
    bool timed_out = false;
    CapturedStream stdout_capture;
    CapturedStream stderr_capture;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(prepared.config.timeout_ms);

    while (true) {
        auto wait_result = child.try_wait();
        if (!wait_result) {
            child.kill_and_wait();
            return make_runtime_error(wait_result.error());
        }

        if (!child.waited() && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            stdin_pipe->write_end.reset();
            child.kill_and_wait();
        }

        if (child.waited() && !stdout_pipe->read_end && !stderr_pipe->read_end) {
            break;
        }

        std::array<pollfd, 3> poll_fds{};
        nfds_t fd_count = 0;
        int stdin_index = -1;
        int stdout_index = -1;
        int stderr_index = -1;

        if (stdin_pipe->write_end && offset < stdin_payload.size()) {
            stdin_index = static_cast<int>(fd_count);
            poll_fds[fd_count++] = pollfd{stdin_pipe->write_end.get(), POLLOUT, 0};
        }
        if (stdout_pipe->read_end) {
            stdout_index = static_cast<int>(fd_count);
            poll_fds[fd_count++] =
                pollfd{stdout_pipe->read_end.get(), static_cast<short>(POLLIN | POLLHUP), 0};
        }
        if (stderr_pipe->read_end) {
            stderr_index = static_cast<int>(fd_count);
            poll_fds[fd_count++] =
                pollfd{stderr_pipe->read_end.get(), static_cast<short>(POLLIN | POLLHUP), 0};
        }

        if (fd_count == 0) {
            continue;
        }

        int poll_timeout_ms = 10;
        if (!child.waited()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            poll_timeout_ms = remaining.count() <= 0
                                  ? 0
                                  : static_cast<int>(std::min<std::int64_t>(remaining.count(), 10));
        }

        const int poll_result = ::poll(poll_fds.data(), fd_count, poll_timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_runtime_error(std::string("Failed while polling tool pipes: ") +
                                      std::strerror(errno));
        }
        if (poll_result == 0) {
            continue;
        }

        if (stdin_index >= 0) {
            const auto revents = poll_fds[stdin_index].revents;
            if ((revents & (POLLERR | POLLHUP)) != 0) {
                stdin_pipe->write_end.reset();
            } else if ((revents & POLLOUT) != 0) {
                while (offset < stdin_payload.size()) {
                    const auto written =
                        ::write(stdin_pipe->write_end.get(), stdin_payload.data() + offset,
                                stdin_payload.size() - offset);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EPIPE) {
                            break;
                        }
                        return make_runtime_error(std::string("Failed to write tool stdin: ") +
                                                  std::strerror(errno));
                    }
                    offset += static_cast<size_t>(written);
                }

                if (offset >= stdin_payload.size()) {
                    stdin_pipe->write_end.reset();
                }
            }
        }

        if (stdout_index >= 0 &&
            (poll_fds[stdout_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            auto read_result =
                read_available(stdout_pipe->read_end.get(), stdout_capture, kMaxStdoutBytes);
            if (!read_result) {
                return make_runtime_error("Failed to read tool stdout: " + read_result.error());
            }
            if (!*read_result) {
                stdout_pipe->read_end.reset();
            }
        }

        if (stderr_index >= 0 &&
            (poll_fds[stderr_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            auto read_result = read_available(stderr_pipe->read_end.get(), stderr_capture,
                                              kMaxCapturedStderrBytes);
            if (!read_result) {
                return make_runtime_error("Failed to read tool stderr: " + read_result.error());
            }
            if (!*read_result) {
                stderr_pipe->read_end.reset();
            }
        }
    }

    if (timed_out) {
        return make_timeout_error(prepared.config.timeout_ms);
    }
    if (stdout_capture.truncated) {
        return make_runtime_error("Tool command stdout exceeded " +
                                  std::to_string(kMaxStdoutBytes) + " bytes");
    }

    const int status = child.status();
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
    static constexpr std::array<std::string_view, 4> kAllowedSchemaKeys = {
        "type", "properties", "required", "additionalProperties"};
    if (auto unknown = reject_unknown_keys(schema, "tools.parameters", kAllowedSchemaKeys);
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
    for (const auto& [key, value] : env) {
        if (key.empty()) {
            return std::unexpected("tools.env keys must not be empty");
        }
        if (key.find('=') != std::string::npos) {
            return std::unexpected("tools.env keys must not contain '='");
        }
        (void)value;
    }
    return {};
}

CommandToolRunResult run_command_tool(const CommandToolConfig& tool,
                                      const nlohmann::json& arguments) {
    auto prepared = build_prepared_command_tool(tool);
    if (!prepared) {
        return make_runtime_error(prepared.error());
    }
    return run_prepared_command_tool(*prepared, arguments);
}

Result<ToolProvider> make_command_tool_provider(const std::vector<CommandToolConfig>& tools) {
    ToolProvider provider;
    provider.tools.reserve(tools.size());

    for (const auto& tool : tools) {
        auto prepared = build_prepared_command_tool(tool);
        if (!prepared) {
            return std::unexpected("Invalid tool '" + tool.name + "': " + prepared.error());
        }

        auto shared = std::make_shared<const PreparedCommandTool>(std::move(*prepared));
        provider.tools.push_back(RegisteredTool{
            .definition = ToolDefinition{tool.name, tool.description, tool.parameters_schema},
            .invoke = [shared](const nlohmann::json& arguments) -> RuntimeResult<nlohmann::json> {
                auto result = run_prepared_command_tool(*shared, arguments);
                if (!result) {
                    return std::unexpected(RuntimeError{
                        RuntimeErrorCode::ToolExecutionFailed,
                        result.error().message,
                        result.error().timed_out ? std::optional<std::string>(kTimeoutContext)
                                                 : std::nullopt,
                    });
                }
                return *result;
            },
        });
    }

    return provider;
}

} // namespace zks::server
