# Error Reference

Every error response from zoo-keeper-server follows this format:

```json
{
  "error": {
    "message": "Human-readable description",
    "type": "error_type",
    "param": "field_name",
    "code": "machine_readable_code"
  }
}
```

`param` and `code` are optional and may be absent.

## 400 — invalid_request_error

Returned when the request body is malformed, a validation rule is violated, or a request-level
override is invalid.

### With `code`

| Code | Description |
|---|---|
| `unknown_field` | Unrecognized field in the request body. |
| `invalid_role` | Message role is not one of `system`, `user`, `assistant`, or `tool`. |
| `invalid_message_sequence` | Messages violate role-ordering rules. |
| `invalid_config` | Zoo configuration or sampling parameter is invalid. |
| `invalid_output_schema` | A structured extraction output schema is malformed or unsupported. |
| `context_window_exceeded` | The prompt exceeds the model's available context window. |
| `tool_not_found` | A referenced tool name is not registered on the agent. |
| `invalid_model` | The `model` field does not match the server's configured `model_id`. |
| `sessions_disabled` | A session-backed request was made while sessions are disabled. |

### Without `code`

These validation errors set `param` but not `code`:

- Request body is not valid JSON.
- `model` is missing, not a string, or empty.
- `messages` is missing, not an array, or empty.
- `schema` is missing or not an object on extraction requests.
- `message` is missing or not an object on retained-agent chat/history append requests.
- `stream` is present but not a boolean.
- `session_id` is present but not a string, or is empty.
- `system_prompt` is present but not a string.
- `message.role` or `message.content` is missing or not a string.
- `tool_call_id` is required on tool messages but missing or not a string.
- Session-backed chat or extraction requests contain more than one new user message.

## 401 — auth_error

| Code | Description |
|---|---|
| `invalid_api_key` | The `Authorization: Bearer <key>` header is missing or does not match the configured `api_key`. |

Only returned when `api_key` is set in the server config. `/healthz` is exempt from auth.

## 404 — not_found_error

| Code | Description |
|---|---|
| `session_not_found` | The requested session ID does not exist. |
| `request_not_found` | The requested public request ID is unknown or no longer in flight. |

## 409 — conflict_error

| Code | Description |
|---|---|
| `session_busy` | The session already has an active in-flight request. |

## 413 — Request Entity Too Large

Returned by Drogon, not in the standard `error` JSON format, when the request body exceeds
`http.client_max_body_size_bytes`.

## 500 — server_error

Returned when the server encounters an internal failure during inference.

| Code | Description |
|---|---|
| `model_load_failed` | The GGUF model file could not be loaded. |
| `context_creation_failed` | llama.cpp failed to create an inference context. |
| `inference_failed` | Token generation failed during inference. |
| `tokenization_failed` | The prompt could not be tokenized. |
| `template_render_failed` | The chat template could not be rendered. |
| `request_timeout` | The request exceeded the configured timeout. |
| `tool_execution_failed` | A registered tool handler returned an error, including command-tool exit failures, invalid stdout, or timeouts. |
| `tool_retries_exhausted` | Tool-call validation retries were exhausted or the tool loop hit its configured limit. |
| `extraction_failed` | Structured extraction could not produce schema-conforming output. |
| `runtime_error` | Catch-all for unmapped internal errors. |

## 503 — service_unavailable_error

Returned when the server cannot accept or continue the request.

| Code | Description |
|---|---|
| `not_ready` | The server runtime is not yet initialized or is shutting down. |
| `agent_not_ready` | The inference agent is not running, or a request was cancelled while in flight. |
| `queue_full` | The request queue is at capacity. Retry after a short delay. |
| `server_busy` | The server's continuation executor is saturated. Retry after a short delay. |
| `session_capacity_reached` | The server has reached its configured `max_sessions` limit. |
