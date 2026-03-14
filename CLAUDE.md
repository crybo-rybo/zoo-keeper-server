# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Initialize submodules (required first time)
git submodule update --init --recursive

# Configure and build
cmake -S . -B build
cmake --build build --parallel

# Build with tests
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel

# Build with optional test UI (served at /_test)
cmake -S . -B build-test-ui -DZKS_ENABLE_TEST_UI=ON
cmake --build build-test-ui --parallel
```

## Run

```bash
./build/zoo_keeper_server config/server.example.json
```

## Tests

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test by name (use -R for regex match)
ctest --test-dir build --output-on-failure -R config_test
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
HTTP Request → api_routes.cpp → ChatService → SessionManager (optional)
                                           → zoo::Agent → llama.cpp
                                                       → SSE or JSON response
```

**Key components:**

- **`ServerRuntime`** (`src/server/runtime.*`) — Singleton lifecycle manager. Owns `ChatService` and background task pool. Manages graceful shutdown.

- **`ZooChatService`** (`src/server/chat_service.*`) — Owns the shared `zoo::Agent` instance and `SessionManager`. Implements `start_completion()`, which returns a `PendingChatCompletion` handle. This is the central orchestrator.

- **`SessionManager`** (`src/server/session_manager.*`) — Per-session conversation history, TTL-based reaping, `max_sessions` enforcement, and history trimming. Sessions are optional (disabled when `max_sessions = 0`).

- **`api_routes`** (`src/server/api_routes.*`) — All HTTP endpoint handlers registered with Drogon. Manages SSE streaming lifecycle, client disconnect cancellation via `DisconnectRegistry`, and request logging.

- **`ApiResult<T>`** (`src/server/api_types.hpp`) — `std::expected`-based error handling used throughout the API layer.

**Single shared agent:** One `zoo::Agent` is loaded at startup and shared across all requests. Sessions do not get their own agent instances; history is managed in `SessionManager` and injected per-request.

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
  "sessions": {
    "max_sessions": 0,        // 0 = sessions disabled
    "idle_ttl_seconds": 900
  },
  "zoo": {
    "model_path": "/path/to/model.gguf",
    "context_size": 2048,
    "n_gpu_layers": -1,
    "max_tokens": -1,
    "system_prompt": "You are a helpful assistant."
  }
}
```

## Code Style

C++23. Follow the `.clang-format` in `extern/zoo-keeper/`: 4-space indent, 100-column limit, snake_case for functions/files, PascalCase for types.
