---
description: Rules for HTTP endpoint handlers and API response formatting
globs: ["src/server/api_routes.*", "src/server/api_json.*", "src/server/api_types.*"]
---

# API Handlers

## Response format
All responses follow the OpenAI-compatible format. Errors return:
```json
{"error": {"type": "...", "code": "...", "message": "...", "param": "..."}}
```
Use the `ApiError` struct and helpers (`invalid_request_error`, `not_found_error`, etc.) in `api_types.hpp`. Never construct raw JSON error bodies.

## Error handling
- `ApiResult<T>` is `std::expected<T, ApiError>` — propagate errors with `std::unexpected`, not exceptions.
- Map `RuntimeError` (from zoo::Agent) to `ApiError` via `map_runtime_error_to_api_error()` in `api_json.cpp`.

## Request validation
- All JSON parsing uses `reject_unknown_keys()` (defined in `internal_utils.hpp`) to reject unrecognized fields. When adding new request fields, add them to the allowed-keys list in `api_json.cpp`.

## Route registration pattern
- Routes are registered as lambdas in `register_api_routes()` (`api_routes.cpp`).
- Chat completion has its own file: `completion_controller.cpp`.
- Wrap response callbacks with `with_metrics()` from `route_utils.hpp` to increment request/error counters.

## Client disconnect cancellation
- `DisconnectRegistry` tracks TCP connections to in-flight requests. When a client disconnects mid-stream, the registered callback cancels the completion. Always register streaming requests with the disconnect registry and clear them on completion.

## Key references
- `docs/openapi.yaml` — OpenAPI spec for all endpoints
- `docs/error-reference.md` — Error type/code catalog
