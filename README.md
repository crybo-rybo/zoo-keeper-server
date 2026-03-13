# zoo-keeper-server

`zoo-keeper-server` is a small HTTP server built on top of
[`zoo-keeper`](https://github.com/crybo-rybo/zoo-keeper). It is aimed at
backend and web-app integrations that want a local model behind a simple
OpenAI-like API.

Today the server:

- vendors `zoo-keeper` in `extern/zoo-keeper`
- boots one shared `zoo::Agent` at process start
- serves `GET /healthz`, `GET /v1/models`, `GET /v1/tools`, and `POST /v1/chat/completions`
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

The shipped config in [config/server.example.json](/Users/conorrybacki/Programs/zoo-keeper-server/config/server.example.json)
must be updated with a real GGUF `model_path` before startup will succeed.

Config fields:

- `bind_address`
- `port`
- `model_id`
- `zoo`

The `zoo` object is parsed directly as `zoo::Config`.

## API

Available routes:

```text
GET  /healthz
GET  /v1/models
GET  /v1/tools
POST /v1/chat/completions
```

`/healthz` returns `200` when the runtime is ready and `503` otherwise.
`/v1/models` returns the single configured model id. `/v1/tools` returns the
server-owned tool catalog; it is currently empty by default.

`/v1/chat/completions` accepts a narrow OpenAI-like request shape:

- `model` as a string
- `messages` as an array of string-content messages
- optional `stream: true` for SSE

Supported roles are `system`, `user`, `assistant`, and `tool`. The server does
not currently accept client-supplied tools or extra OpenAI fields.

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

## Smoke Executables

`zoo_keeper_link_smoke` verifies the library links correctly without requiring a
model file.

`zoo_keeper_live_smoke` runs a real model prompt against a GGUF path:

```bash
./build/zoo_keeper_live_smoke /absolute/path/to/model.gguf
```
