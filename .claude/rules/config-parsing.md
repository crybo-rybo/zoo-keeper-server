---
description: Rules for config parsing and command tool execution
globs: ["src/server/config.*", "src/server/command_tools.*"]
---

# Config & Command Tools

## Config parsing
- `load_config()` reads JSON from disk and returns `Result<ServerConfig>`.
- Every config struct has a `validate()` method returning `Result<void>`.
- Use `reject_unknown_keys()` on every JSON object to catch typos. When adding a new config field, add its key to the allowed-keys list in the relevant `from_json()` overload in `config.cpp`.
- Error handling uses `Result<T>` (`std::expected<T, std::string>`) — not exceptions.

## Config structure
```
ServerConfig
├── bind_address, port, model_id, api_key
├── HttpConfig (body limits, timeouts, CORS)
├── SessionConfig (max_sessions, idle_ttl_seconds)
├── vector<CommandToolConfig> (tools)
└── zoo::Config (model_path, context_size, sampling, etc.)
```

## CommandToolConfig
Each tool defines: `name`, `description`, `parameters_schema` (JSON Schema), `command` (argv), optional `working_directory`, `inherit_environment`, `env`, and `timeout_ms`.
- `validate()` checks config-level constraints (name format, non-empty command).
- `make_command_tool_provider()` does filesystem validation at startup (executable exists, working directory exists).
- `validate_tool_parameters_schema()` validates the JSON Schema structure itself.

## Tool execution
`run_command_tool()` uses `posix_spawn` — no shell involved, which prevents injection. Arguments are passed as argv, not concatenated into a shell string. The tool runs with explicit FD management (stdin closed, stdout/stderr captured) and a timeout enforced via `waitpid` + `SIGKILL`.

## Example configs
`config/server.example.json` is the canonical template. Keep it portable (no absolute paths, no secrets).
