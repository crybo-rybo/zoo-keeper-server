# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
scripts/build              # Configure + build (auto-inits submodules)
scripts/build --test-ui    # Build with browser test UI at /_test
scripts/build --sanitize   # Build with ASan/UBSan
scripts/test               # Build + run all tests
scripts/test -R config     # Build + run tests matching pattern
scripts/format             # Format all source files
scripts/format-check       # Check formatting (exits non-zero on violations)
```

## Run

```bash
./build/zoo_keeper_server config/server.example.json
```

## CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `ZKS_ENABLE_TEST_UI` | OFF | Enables browser test UI at `/_test` |
| `ZKS_LIVE_SMOKE_MODEL` | (empty) | Path to GGUF model for live smoke test |
| `ZOO_ENABLE_METAL` | ON (macOS) | Apple Metal GPU acceleration |
| `ZOO_ENABLE_CUDA` | OFF | CUDA GPU acceleration |

## Architecture

Zoo-Keeper-Server is an HTTP API server for local LLM inference. It wraps the `zoo-keeper` agent library (in `extern/zoo-keeper/`, a git submodule backed by llama.cpp) in an OpenAI-compatible REST API using Drogon (C++ HTTP framework, auto-fetched by CMake).

**Request flow:**

```
HTTP Request → api_routes.cpp → ZooChatService → SessionStore (optional)
                                               → zoo::Agent → llama.cpp
                                                             → SSE or JSON response
```

**Key components:**

- **`ServerRuntime`** (`src/server/runtime.*`) — Singleton lifecycle manager. Owns `ChatService` and background task pool. Manages graceful shutdown.

- **`ZooChatService`** (`src/server/chat_service.*`) — Owns the shared `zoo::Agent` instance and `SessionStore`. Handles completion orchestration for both session and non-session paths via `start_completion()`, which returns a `PendingChatCompletion` handle. This is the central orchestrator.

- **`SessionStore`** (`src/server/session_manager.*`) — Pure state store for per-session conversation history. Handles TTL-based reaping, `max_sessions` enforcement, history trimming, and active-request tracking. Has no knowledge of `zoo::Agent` or completion infrastructure. Sessions are optional (disabled when `max_sessions = 0`).

- **`api_routes`** (`src/server/api_routes.*`) — All HTTP endpoint handlers registered with Drogon. Manages SSE streaming lifecycle, client disconnect cancellation via `DisconnectRegistry`, and request logging.

- **`ApiResult<T>`** (`src/server/api_types.hpp`) — `std::expected`-based error handling used throughout the API layer. Core types (`MessageRole`, `ChatMessage`, `RuntimeError`, etc.) are `using` aliases to `zoo::` types from the controlled submodule.

**Single shared agent:** One `zoo::Agent` is loaded at startup and shared across all requests. Sessions do not get their own agent instances; history is managed in `SessionStore` and injected per-request by `ZooChatService`.

## API Endpoints

| Endpoint | Method | Notes |
|----------|--------|-------|
| `/healthz` | GET | 200 if ready, 503 otherwise |
| `/v1/models` | GET | Returns the configured `model_id` |
| `/v1/tools` | GET | Server-owned tools (currently empty) |
| `/v1/sessions` | POST | Create session |
| `/v1/sessions/{id}` | GET / DELETE | Session metadata / teardown |
| `/v1/chat/completions` | POST | OpenAI-compatible; supports `stream: true` (SSE) |

Chat completions accept `session_id` to associate requests with a session for history continuity.

## Configuration

`config/server.example.json` is the runtime config template. Key fields:

```json
{
  "bind_address": "127.0.0.1",
  "port": 8080,
  "model_id": "local-model",
  "api_key": null,
  "http": {
    "client_max_body_size_bytes": 1048576,
    "client_max_memory_body_size_bytes": 65536,
    "idle_connection_timeout_seconds": 60
  },
  "sessions": {
    "max_sessions": 0,
    "idle_ttl_seconds": 900
  },
  "tools": [],
  "zoo": {
    "model_path": "/path/to/model.gguf",
    "context_size": 2048,
    "n_gpu_layers": -1,
    "max_tokens": -1,
    "system_prompt": "You are a helpful assistant.",
    "sampling": {
      "temperature": 0.7,
      "top_p": 0.9,
      "top_k": 40,
      "repeat_penalty": 1.1,
      "seed": -1
    },
    "use_mmap": true,
    "use_mlock": false,
    "max_history_messages": 64,
    "request_queue_capacity": 64
  }
}
```

`api_key`: set to a non-null string to require `Authorization: Bearer <key>` auth. Omit or `null` for trusted-localhost mode.

`sessions.max_sessions = 0` disables sessions entirely. `tools` is an array of `CommandToolConfig` objects; see the README for the full schema.

## Code Style

C++23. Follow the `.clang-format` in the project root: 4-space indent, 100-column limit, snake_case for functions/files, PascalCase for types.

## Context Pointers

| What | Where |
|------|-------|
| Path-scoped agent rules | `.claude/rules/` |
| Reusable agent commands | `.claude/commands/` |
| Architecture decisions | `docs/adr/` |
| OpenAPI specification | `docs/openapi.yaml` |
| Error type/code catalog | `docs/error-reference.md` |
