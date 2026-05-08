<p align="center">
  <img src="docs/images/logo.png" alt="Zoo Keeper Server Logo" width="200" />
</p>

<h1 align="center">Zoo Keeper Server</h1>

<p align="center">
  <b>A local LLM inference server with an OpenAI-compatible subset REST API.</b><br/>
  <sub>llama.cpp-backed &bull; SSE streaming &bull; Sessions &bull; Command tools &bull; Metrics &bull; API key auth</sub>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue" alt="C++23" />
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License" />
  <img src="https://img.shields.io/badge/tests-ctest%20passing-success" alt="Tests" />
</p>

## About

`zoo-keeper-server` wraps the [zoo-keeper](https://github.com/crybo-rybo/zoo-keeper)
agent library in a clean HTTP server. Drop in a GGUF model, point the config at
it, and get an OpenAI-compatible subset of `/v1/chat/completions` with
streaming, optional sessions, server-owned command tools, and an observability
metrics endpoint. Built with C++23 and
[Drogon](https://github.com/drogonframework/drogon).

## Build

Clone with submodules:

```bash
git submodule update --init --recursive
```

Drogon and zoo-keeper's transitive build dependencies are fetched at CMake configure time.

Configure and build:

```bash
scripts/build
```

The equivalent raw CMake commands are:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
```

To compile the test-only browser console at `/_test`:

```bash
scripts/build --test-ui
```

| Option | Default | Purpose |
|--------|---------|---------|
| `ZKS_ENABLE_TEST_UI` | OFF | Browser test UI at `/_test` |
| `ZKS_ENABLE_ZOO_HUB` | ON | Build zoo-keeper hub support for GGUF inspection and auto-config |
| `ZKS_LIVE_SMOKE_MODEL` | (empty) | Path to GGUF model for live smoke test |
| `ZOO_ENABLE_METAL` | ON (macOS) | Apple Metal GPU acceleration |
| `ZOO_ENABLE_CUDA` | OFF | CUDA GPU acceleration |

## Test

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/zoo_keeper_server config/server.example.json
```

Update `config/server.example.json` with a real GGUF `model_path` before startup will succeed.

## Configuration

```json
{
  "bind_address": "127.0.0.1",
  "port": 8080,
  "model_id": "local-model",
  "api_key": null,
  "http": {
    "client_max_body_size_bytes": 1048576,
    "client_max_memory_body_size_bytes": 65536,
    "idle_connection_timeout_seconds": 60,
    "cors_allow_origins": []
  },
  "sessions": {
    "max_sessions": 0,
    "idle_ttl_seconds": 900
  },
  "tools": [],
  "zoo": {
    "model_path": "/path/to/model.gguf",
    "auto_configure_model": false,
    "context_size": 2048,
    "n_gpu_layers": -1,
    "max_tokens": -1,
    "system_prompt": "You are a helpful assistant."
  }
}
```

Set `api_key` to a non-empty string to require `Authorization: Bearer <key>` on
all non-`/healthz` endpoints. Omit or set to `null` for trusted localhost mode.

Use `http.client_max_body_size_bytes`,
`http.client_max_memory_body_size_bytes`, and
`http.idle_connection_timeout_seconds` to tune Drogon request limits and idle
connection handling. `http.cors_allow_origins` defaults to an empty array,
which disables CORS response headers. Set exact browser origins, or `"*"`,
when cross-origin browser clients need access.

Set `sessions.max_sessions` to `0` (the default) to disable sessions entirely.

Set `tools` to an array of server-owned command tools to let the shared
`zoo::Agent` call local executables during completion. Each tool declares a
name, description, manual-schema `parameters` object, and a `command` argv
array. The server resolves that executable at startup, sends tool arguments as
JSON on stdin, and expects one JSON object on stdout.

When `bind_address` is non-loopback and `api_key` is unset, the server emits a
startup warning because that configuration should only be used on a trusted
network.

Set `zoo.auto_configure_model` to `true` to inspect the configured GGUF through
zoo-keeper's hub layer and seed `context_size`, `n_gpu_layers`, `use_mmap`, and
`use_mlock` from model metadata. Explicit fields in the same `zoo` block still
override the inspected defaults.

The `zoo` object is mapped onto zoo-keeper's split `ModelConfig`,
`AgentConfig`, and `GenerationOptions`. See
[zoo-keeper](https://github.com/crybo-rybo/zoo-keeper) for the full set of
options including sampling parameters.

## Configuration Examples

The `config/` directory contains ready-to-use templates:

| File | Purpose |
|------|---------|
| `server.example.json` | Full-featured template; starting point for new deployments |
| `api-key-enabled.json` | Same as above but with `api_key` set; use when binding to non-loopback |
| `cpu-only.json` | `n_gpu_layers: 0`, sessions disabled; for machines without GPU |
| `metal.json` | `context_size: 4096`, `n_gpu_layers: -1`; macOS Metal GPU tuning baseline |
| `sessions-disabled.json` | `max_sessions: 0`; stateless deployment |

## API

| Endpoint | Method | Notes |
|----------|--------|-------|
| `/healthz` | GET | `200` when ready, `503` otherwise |
| `/v1/models` | GET | Returns the configured `model_id` |
| `/v1/tools` | GET | Server-owned tool catalog from `tools` config |
| `/v1/sessions` | POST | Create a session |
| `/v1/sessions/{id}` | GET / DELETE | Session metadata / teardown |
| `/v1/chat/completions` | POST | OpenAI-compatible subset; supports `stream: true` |
| `/v1/extractions` | POST | Structured extraction; supports `stream: true` |
| `/v1/agent/chat` | POST | Chat against retained shared-agent history |
| `/v1/agent/history` | GET / PUT / DELETE | Inspect, replace, or clear retained shared-agent history |
| `/v1/agent/history:swap` | POST | Atomically swap retained shared-agent history |
| `/v1/agent/history/messages` | POST | Append one retained shared-agent history message |
| `/v1/agent/system-prompt` | GET / PUT | Inspect or replace the retained agent system prompt |
| `/v1/requests/{id}/cancel` | POST | Cancel an in-flight request by public request id |
| `/v1/runtime` | GET | Runtime configuration snapshot |
| `/metrics` | GET | Request counters and uptime |

The machine-readable API contract lives in [docs/openapi.yaml](docs/openapi.yaml).

### Chat completions

`POST /v1/chat/completions` accepts only these top-level fields:

- `model` — string
- `messages` — array of message objects; accepted fields are `role`, `content`,
  `tool_call_id` for `tool` messages, and assistant `tool_calls`
- `stream` — optional boolean for SSE
- `session_id` — optional; associates the request with a server-owned session

When `session_id` is omitted the request is stateless and `messages` should contain the full transcript. When present, `messages` must contain exactly one new `user` message — prior context comes from the session.

Unsupported top-level request fields currently return `400` with
`error.code = "unknown_field"`.

Responses include a `tool_invocations` array and a `zoo_metrics` object with
latency and throughput data. When `tools` are configured, the shared agent
registers them at startup, `/v1/tools` returns their schemas, and each
completion reports per-invocation outcomes in `tool_invocations`.

Under extreme load, the server's bounded continuation executor may reject new
completion follow-up work with `503` and `error.code = "server_busy"` rather
than growing threads without bound.

### Structured extraction and retained agent APIs

`POST /v1/extractions` accepts the same chat-style `messages` input as chat
completion requests plus a required JSON `schema`. Non-streaming responses
include both raw model `text` and parsed `data`; streaming responses emit
incremental `delta` events, then the final extraction payload, then `[DONE]`.

The `/v1/agent/*` endpoints expose zoo-keeper's retained shared-agent state for
advanced workflows. They are intentionally separate from session-backed chat:
sessions keep per-client history in `SessionStore`, while retained-agent
endpoints mutate the single shared `zoo::Agent` history and system prompt.

### Metrics

`GET /metrics` returns:

```json
{
  "requests_total": 142,
  "requests_errors": 3,
  "requests_cancelled_total": 2,
  "requests_queue_rejected_total": 1,
  "stream_disconnects_total": 4,
  "tool_invocations_total": 18,
  "tool_failures_total": 2,
  "tool_timeouts_total": 1,
  "active_sessions": 2,
  "model_id": "local-model",
  "uptime_seconds": 3600
}
```

The current counters are HTTP-response based. Streaming failures that happen
after the initial `200 OK` response are not surfaced as structured metric
errors yet.

### Command tools

Example tool declaration:

```json
{
  "tools": [
    {
      "name": "echo_input",
      "description": "Echo a string back to the model.",
      "parameters": {
        "type": "object",
        "properties": {
          "value": { "type": "string", "description": "Value to echo" }
        },
        "required": ["value"],
        "additionalProperties": false
      },
      "command": ["/absolute/path/to/tool-binary", "echo"],
      "inherit_environment": false,
      "timeout_ms": 5000
    }
  ]
}
```

Tool rules:

- `command` is executed as argv, not through a shell
- the resolved executable path is pinned at startup and that same path is executed later
- tool arguments arrive as JSON on stdin
- stdout must be a single JSON object
- tool subprocesses run with a scrubbed environment by default; set `inherit_environment` to `true` only when a tool genuinely needs parent environment variables
- non-zero exit, invalid stdout, or timeout are reported as `tool_execution_failed`
- the current implementation supports the upstream flat-object manual schema subset only

## Examples

Create a session:

```bash
curl -s http://127.0.0.1:8080/v1/sessions \
  -H 'content-type: application/json' \
  -d '{"model": "local-model", "system_prompt": "You are a concise assistant."}'
```

Stateless completion:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "messages": [{"role": "user", "content": "Say hello in one sentence."}]
  }'
```

Streaming:

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "stream": true,
    "messages": [{"role": "user", "content": "Say hello in one sentence."}]
  }'
```

With API key auth:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'authorization: Bearer my-secret-key' \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "messages": [{"role": "user", "content": "Say hello in one sentence."}]
  }'
```

## Architecture

```text
HTTP Request → api_routes.cpp → ZooChatService → SessionStore (optional)
                                               → zoo::Agent → llama.cpp
                                                             → SSE or JSON response
```

One `zoo::Agent` is loaded at startup and shared across all requests. Sessions do not get their own agent instances — history is managed in `SessionStore` and injected per-request by `ZooChatService`.

## Known Limitations

- **Sessions are in-memory and process-lifetime only.** There is no persistence; sessions are lost on restart.
- **macOS + Metal + sessions: OOM during inference will abort the process.**
  On macOS with Metal enabled, a device out-of-memory condition during inference triggers a fatal abort in the upstream `llama.cpp` Metal backend before `zoo-keeper` can surface a recoverable error. Reduce `n_gpu_layers` or `context_size`, or disable sessions (`max_sessions = 0`) if you hit this. Tracked upstream.

## Releases

Release notes are tracked in [CHANGELOG.md](CHANGELOG.md).

## License

MIT. See [LICENSE](LICENSE).
