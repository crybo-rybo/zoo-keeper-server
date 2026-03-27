# ADR-004: Optional Sessions

## Status
Accepted

## Context
Many LLM use cases are stateless (single-turn completions, batch processing, tool-calling agents). Requiring session creation adds unnecessary complexity for these users. However, multi-turn conversation support is essential for chat applications.

## Decision
Sessions are opt-in and disabled by default (`max_sessions = 0`). When disabled, all session endpoints return an error immediately and `SessionStore` operations short-circuit. Chat completions work without a `session_id` — the client provides the full message history per request.

## Consequences
- **Default experience:** Zero-config stateless completions. No session management overhead.
- **Opt-in state:** Set `max_sessions > 0` to enable sessions. The server then manages history, TTL reaping, and active-request tracking.
- **API design:** `session_id` is an optional field on `ChatCompletionRequest`. Session endpoints (`POST /v1/sessions`, `GET/DELETE /v1/sessions/{id}`) are always registered but return errors when disabled.
- **Testing:** Most tests don't need sessions, simplifying test setup.
