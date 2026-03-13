# zoo-keeper-server 1.0 Plan

## Summary

- Treat the current repository as a solid stateless MVP: one configured model,
  OpenAI-like chat completions, SSE streaming, health, and model/tool
  discovery.
- Define `1.0` as a trusted-backend server with two modes:
  - stateless requests exactly like today
  - optional in-memory durable conversation sessions implemented with
    per-session `zoo::Agent` instances
- Keep `1.0` narrow: no auth, no database persistence, no embeddings or RAG,
  and no broad OpenAI parity beyond chat completions plus session management.

## Public Interfaces

- Extend server config with:
  - `sessions.max_sessions` with default `0`
  - `sessions.idle_ttl_seconds` with default `900`
- Extend `POST /v1/chat/completions` with optional `session_id`.
- Define sessioned chat behavior:
  - no `session_id`: current stateless `messages[]` behavior remains unchanged
  - with `session_id`: require exactly one new `user` message; prior context
    comes from the server-owned session
- Add session endpoints:
  - `POST /v1/sessions`
  - `GET /v1/sessions/{id}`
  - `DELETE /v1/sessions/{id}`
- Use one session response shape:
  - `id`
  - `object: "session"`
  - `model`
  - `created`
  - `last_used`
  - `expires_at`
- Add stable API errors:
  - `400`: `sessions_disabled`, `invalid_model`, invalid session message shape
  - `404`: `session_not_found`
  - `409`: `session_busy`
  - `503`: `session_capacity_reached`, `queue_full`, `agent_not_ready`

## Epic 1: Harden the Stateless Core

### Phase 1.1

- Replace detached request waiter threads with runtime-owned tracked worker
  tasks so shutdown is deterministic.
- Add graceful shutdown to `ServerRuntime`: stop new work, cancel in-flight
  requests, join waiter tasks, release agents cleanly.
- Fix disconnect tracking so multiple simultaneous streaming requests on one TCP
  connection are handled correctly.

### Phase 1.2

- Add structured logs for request start, finish, cancel, error, token usage,
  and latency.
- Extend health output with session-capability fields even before sessions are
  enabled.
- Expand HTTP integration coverage around streaming cancellation, queue-full
  behavior, and shutdown safety.

## Epic 2: Add Session Infrastructure

### Phase 2.1

- Add session config parsing, validation, and health reporting.
- Add `SessionManager` that owns session metadata, per-session agents,
  last-used timestamps, idle expiry, and capacity enforcement.
- Make session creation eager so `POST /v1/sessions` returns only after the
  backing agent is ready.

### Phase 2.2

- Apply the same startup tool catalog and base prompt policy to both the shared
  stateless agent and all session agents.
- Support optional per-session `system_prompt` at session creation by combining
  it with the server base prompt.
- Enforce one in-flight request per session and surface `409 session_busy`.

## Epic 3: Add Sessioned Chat Behavior

### Phase 3.1

- Route `POST /v1/chat/completions` by mode:
  - stateless path uses the shared agent and full `messages[]`
  - sessioned path uses the session agent and one new `user` message
- Preserve JSON and SSE response shapes across both modes.
- Ensure stream disconnect cancels the correct stateless or sessioned request.

### Phase 3.2

- Implement `GET /v1/sessions/{id}` and `DELETE /v1/sessions/{id}`.
- Deleting a session must cancel any in-flight request and release the session
  agent.
- Idle reaping must never remove a session with active work.

## Epic 4: 1.0 Readiness

### Phase 4.1

- Add unit coverage for session request parsing, session JSON bodies, error
  mapping, disconnect registry behavior, and session expiry logic.
- Add HTTP integration coverage for create/get/delete session, sessioned
  non-stream chat, sessioned streaming chat, disabled sessions, unknown
  session, invalid session message shape, busy session, and capacity reached.
- Add a live-model smoke scenario proving a second sessioned turn sees prior
  context without resending the transcript.

### Phase 4.2

- Update docs to cover session config, session lifecycle endpoints, stateless
  vs sessioned chat behavior, and operational limits.
- Keep CI running the default stateless suite and make session coverage part of
  the default test path.
- Define the 1.0 release gate: all default tests green on macOS and Ubuntu,
  live-model smoke documented and runnable locally, clean shutdown verified, and
  session limits enforced.

## Acceptance Criteria

- The current stateless API remains backward-compatible.
- Session mode can be fully disabled by config.
- When session mode is enabled, clients can create a session, send one-turn
  chat requests with `session_id`, and get context continuity across turns.
- Server shutdown, streaming disconnects, session deletion, and idle expiry do
  not leak threads or leave orphaned in-flight work.
- Health, logs, and tests are strong enough to operate the server as a trusted
  backend service.

## Assumptions

- Trusted backend only for `1.0`; no auth or rate limiting.
- Narrow OpenAI-style compatibility only; do not add broader request knobs or
  new endpoint families.
- Conversation durability is process-lifetime in-memory only.
- Shipping `1.0` with an empty built-in tool catalog is acceptable as long as
  tool registration and discovery remain consistent.
