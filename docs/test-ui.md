# Test UI

This repository can compile a small browser-based test console into the server
binary. The console is served by the same Drogon process and is only available
when the project is built with `ZKS_ENABLE_TEST_UI=ON`.

When enabled, the server exposes:

- `GET /_test` for the browser UI
- the existing API routes the UI talks to:
  - `GET /healthz`
  - `GET /v1/models`
  - `GET /v1/tools`
  - `POST /v1/sessions`
  - `DELETE /v1/sessions/{id}`
  - `POST /v1/chat/completions`

## Build

Default builds do not include the UI.

Configure a separate build directory with the UI enabled:

```bash
cmake -S . -B build-test-ui -DZKS_ENABLE_TEST_UI=ON
cmake --build build-test-ui --parallel
```

The server executable is then:

```bash
./build-test-ui/zoo_keeper_server
```

## Server Config

The test UI does not add any new JSON config fields. It uses the same server
config file as the normal binary.

Start from [config/server.example.json](/Users/conorrybacki/Programs/zoo-keeper-server/config/server.example.json)
and update at least:

- `bind_address`: network interface the server listens on
- `port`: TCP port the server listens on
- `model_id`: model name shown by `/v1/models` and used by the UI
- `zoo.model_path`: real path to a GGUF model file

Relevant optional settings:

- `sessions.max_sessions`: set greater than `0` to enable session creation from
  the UI
- `sessions.idle_ttl_seconds`: session idle timeout in seconds
- `api_key`: when set, the browser requests must include
  `Authorization: Bearer <key>`
- `zoo.system_prompt`: base prompt used by the shared runtime

If `sessions.max_sessions` is `0`, the page still works, but only in stateless
mode.

If `bind_address` is non-loopback and `api_key` is unset, the server emits a
startup warning because that configuration should only be used on a trusted
network.

## Run

Start the UI-enabled binary with a config file:

```bash
./build-test-ui/zoo_keeper_server config/server.example.json
```

If the model path is invalid or missing, startup fails before the server begins
listening.

## Connect

The test page is always served at:

```text
http://<host>:<port>/_test
```

How to fill in `<host>` depends on your server config.

### Local machine

If your config uses:

```json
{
  "bind_address": "127.0.0.1",
  "port": 8080
}
```

open:

```text
http://127.0.0.1:8080/_test
```

This only accepts connections from the same machine.

### Another device on your network

If you want to open the page from another machine, change the server config to
listen on a non-loopback interface, for example:

```json
{
  "bind_address": "0.0.0.0",
  "port": 8080
}
```

Then connect to the host machine's LAN IP address:

```text
http://192.168.x.y:8080/_test
```

Notes:

- `0.0.0.0` means "listen on all interfaces"; it is not the address you type
  into the browser
- the browser should use the actual IP or DNS name of the machine running
  `zoo_keeper_server`
- local firewall rules must allow inbound connections on the configured port

## How the Page Works

The page is same-origin with the API and uses the existing server endpoints
directly.

- Refresh server data loads `/healthz`, `/v1/models`, and `/v1/tools`
- `/v1/tools` currently loads the server-owned tool catalog, which is empty in
  the default build
- Stateless chat sends the full local transcript to
  `/v1/chat/completions`
- Session chat first creates a session with `POST /v1/sessions`, then sends one
  new `user` message plus `session_id` on each turn
- Streaming mode uses the existing SSE response from
  `POST /v1/chat/completions`

Reloading the page clears the browser-side transcript. Server-side session
state remains until it expires or is deleted.

## What You Can Configure From the Page

The page lets you:

- select the configured model returned by `/v1/models`
- send stateless chat requests
- create and delete a session if sessions are enabled
- provide an optional per-session `system_prompt` when creating a session
- toggle streaming on and off

The page does not let you change server bootstrap settings such as
`bind_address`, `port`, or `zoo.model_path`. Those still come from the JSON
config file used at process startup.
