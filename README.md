# zoo-keeper-server

`zoo-keeper-server` is a small HTTP server built on top of
[`zoo-keeper`](https://github.com/crybo-rybo/zoo-keeper). It is aimed at
backend and web-app integrations that want a local model behind a simple
OpenAI-like API.

Today the server:

- vendors `zoo-keeper` in `extern/zoo-keeper`
- boots one shared stateless `zoo::Agent` at process start
- can create optional in-memory sessions for durable conversation context (all
  sessions share the single `zoo::Agent`)
- serves `GET /healthz`, `GET /v1/models`, `GET /v1/tools`,
  `POST /v1/sessions`, `GET /v1/sessions/{id}`, `DELETE /v1/sessions/{id}`,
  and `POST /v1/chat/completions`
- supports JSON completions and SSE streaming
- uses stateless request-scoped chat completion on top of the upstream
  `zoo::Agent::complete(...)` API

## Build

Clone with submodules:

```bash
git submodule update --init --recursive
```

Drogon is fetched at CMake configure time. A JsonCpp development package must
be available on the host.

Configure and build:

```bash
cmake -S . -B build
cmake --build build --parallel
```

To compile the test-only browser console at `/_test`, opt in explicitly:

```bash
cmake -S . -B build-test-ui -DZKS_ENABLE_TEST_UI=ON
cmake --build build-test-ui --parallel
```

Defaults in this repo:

- macOS: `ZOO_ENABLE_METAL=ON`
- non-macOS: `ZOO_ENABLE_METAL=OFF`
- all platforms: `ZOO_ENABLE_CUDA=OFF`

## Test

Run the default server test suite:

```bash
ctest --test-dir build --output-on-failure
```

This covers config parsing, health responses, API request/response shaping, the
missing-model startup failure path, and the baseline link smoke.

## Run

Start the server with the example config:

```bash
./build/zoo_keeper_server config/server.example.json
```

The shipped config in [config/server.example.json](config/server.example.json)
must be updated with a real GGUF `model_path` before startup will succeed.

If you built with `-DZKS_ENABLE_TEST_UI=ON`, a test-only browser console is
also available at `http://127.0.0.1:8080/_test`. It uses the same in-process
routes as the API and is omitted entirely from default builds.

Config fields:

- `bind_address`
- `port`
- `model_id`
- `sessions`
- `zoo`

The `zoo` object is parsed directly as `zoo::Config`.
The `sessions` object controls optional in-memory session support:

- `max_sessions`
- `idle_ttl_seconds`

Set `sessions.max_sessions` to `0` to disable sessions entirely. This is the
default because sessions are in-memory process-lifetime state with no
persistence — enabling them is an explicit opt-in.

## API

Available routes:

```text
GET  /healthz
GET  /v1/models
GET  /v1/tools
POST /v1/sessions
GET  /v1/sessions/{id}
DELETE /v1/sessions/{id}
POST /v1/chat/completions
```

`/healthz` returns `200` when the runtime is ready and `503` otherwise.
`/v1/models` returns the single configured model id. `/v1/tools` returns the
server-owned tool catalog; it is currently empty by default.
`/v1/sessions` manages in-memory process-lifetime chat sessions.

`/v1/chat/completions` accepts a narrow OpenAI-like request shape:

- `model` as a string
- `messages` as an array of string-content messages
- optional `stream: true` for SSE
- optional `session_id` for server-owned session context

Supported roles are `system`, `user`, `assistant`, and `tool`. The server does
not currently accept client-supplied tools or extra OpenAI fields.

When `session_id` is omitted, the request stays stateless and `messages[]`
should contain the full request-scoped transcript.

When `session_id` is present, `messages[]` must contain exactly one new `user`
message. Prior conversation context comes from the server-owned session.

Create a session:

```bash
curl -s http://127.0.0.1:8080/v1/sessions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "system_prompt": "You are a concise assistant."
  }'
```

Example request:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "messages": [
      {"role": "user", "content": "Say hello in one short sentence."}
    ]
  }'
```

Sessioned request:

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "session_id": "sess-1",
    "messages": [
      {"role": "user", "content": "Continue our conversation in one sentence."}
    ]
  }'
```

Streaming example:

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model": "local-model",
    "stream": true,
    "messages": [
      {"role": "user", "content": "Say hello in one short sentence."}
    ]
  }'
```

## Known Limitations

- **No authentication.** The server is designed as a trusted local backend. Do
  not expose it directly to untrusted networks.
- **Sessions are in-memory and process-lifetime only.** There is no persistence;
  sessions are lost on restart.
- **macOS + Metal + sessions: OOM during inference will abort the process.**
  On macOS with Metal enabled, a device out-of-memory condition deep in
  inference triggers a fatal abort in the upstream `llama.cpp` Metal backend
  before `zoo-keeper` can surface a recoverable error. If you hit this, reduce
  `n_gpu_layers` or `context_size`, or disable sessions (`max_sessions = 0`).
  Tracked upstream in `crybo-rybo/zoo-keeper`; see
  `docs/zoo-keeper-metal-oom-issue.md` for details.

## Smoke Executables

`zoo_keeper_link_smoke` verifies the library links correctly without requiring a
model file.

`zoo_keeper_live_smoke` runs a real model prompt against a GGUF path:

```bash
./build/zoo_keeper_live_smoke /absolute/path/to/model.gguf
```
