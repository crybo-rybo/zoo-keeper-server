# zoo-keeper-server

Pre-MVP bootstrap for a companion server project built around the upstream
[`zoo-keeper`](https://github.com/crybo-rybo/zoo-keeper) library.

## Current Slice

The repo currently includes:

- vendor `zoo-keeper` as `extern/zoo-keeper`
- fetch and build Drogon as the embedded HTTP server framework
- load a narrow JSON server config that embeds `zoo::Config`
- bootstrap one shared `zoo::Agent` at process start
- expose `GET /healthz`, `GET /v1/models`, and `GET /v1/tools`
- expose `POST /v1/chat/completions` with JSON responses or SSE when `stream=true`
- build and run a zero-model link smoke executable
- optionally run a live model smoke executable against a real GGUF file

## Bootstrap

If you cloned without submodules, initialize them first:

```bash
git submodule update --init --recursive
```

Drogon is fetched at configure time. It requires a JsonCpp development package
to be available on the host.

Configure the project:

```bash
cmake -S . -B build
```

Acceleration defaults are set explicitly by this repo:

- macOS: `ZOO_ENABLE_METAL=ON`
- non-macOS: `ZOO_ENABLE_METAL=OFF`
- all platforms: `ZOO_ENABLE_CUDA=OFF`

You can still override them at configure time, for example:

```bash
cmake -S . -B build -DZOO_ENABLE_METAL=OFF
```

Build and run the test suite:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

The default CTest path covers:

- config parsing
- `/healthz` response shaping
- OpenAI-like request parsing and response shaping
- startup failure when the server config omits a valid model path
- the zero-model library link smoke

## Server

The real server executable now expects a JSON config file:

```bash
./build/zoo_keeper_server config/server.example.json
```

Update [`config/server.example.json`](/Users/conorrybacki/Programs/zoo-keeper-server/config/server.example.json#L1)
with a real model path before running it. Once the process boots successfully,
the server exposes:

```text
GET /healthz
GET /v1/models
GET /v1/tools
POST /v1/chat/completions
```

Successful responses return HTTP `200` with a body shaped like:

```json
{
  "status": "ok",
  "ready": true,
  "model": {
    "id": "local-model"
  }
}
```

`GET /v1/models` returns the single configured model id:

```json
{
  "object": "list",
  "data": [
    {
      "id": "local-model",
      "object": "model",
      "owned_by": "zoo-keeper-server"
    }
  ]
}
```

`GET /v1/tools` returns the server-owned startup tool catalog. The initial MVP
catalog may be empty:

```json
{
  "object": "list",
  "data": []
}
```

Non-stream chat requests accept a narrow OpenAI-like JSON body:

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

Streaming requests use server-sent events:

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

## Link Smoke

The zero-model link smoke remains available as a separate executable:

```bash
./build/zoo_keeper_link_smoke
```

It succeeds only when `ZooKeeper::zoo` is linked correctly and
`zoo::Agent::create(zoo::Config{})` fails with the expected
`InvalidModelPath` validation error.

## Live Smoke

An optional live smoke executable is also built:

```bash
./build/zoo_keeper_live_smoke /absolute/path/to/model.gguf
```

You can override the prompt:

```bash
./build/zoo_keeper_live_smoke /absolute/path/to/model.gguf "Say hello in one sentence."
```

To register the live smoke as a CTest during configuration, provide the model
path up front:

```bash
cmake -S . -B build -DBUILD_TESTING=ON \
  -DZKS_LIVE_SMOKE_MODEL=/absolute/path/to/model.gguf
ctest --test-dir build --output-on-failure
```

`live_model_smoke` remains opt-in so the default developer and CI path does not
depend on a local model file.

## CI

GitHub Actions runs the baseline configure, build, and zero-model smoke test on
`ubuntu-latest` and `macos-latest` with recursive submodule checkout plus the
required JsonCpp install step for Drogon.
