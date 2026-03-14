# Operational Hardening Plan

## Summary

This should be the first post-`v0.0.1` changeset.

It addresses `hardened operational environments` before `public-internet
production use` or `multi-tenant deployments`, because both of those next
steps depend on stronger request limits, clearer shutdown behavior, better
observability, and real end-to-end HTTP coverage.

This plan is intentionally app-level only. It does not include TLS,
reverse-proxy policy, rate limiting, or tenant isolation.

## Key Changes

### Server Config

Add a new top-level `http` section to the server config with conservative
defaults:

- `client_max_body_size_bytes: 1048576`
- `client_max_memory_body_size_bytes: 65536`
- `idle_connection_timeout_seconds: 60`

Validation rules:

- reject unknown `http` keys
- reject non-positive values for both body-size settings
- allow `idle_connection_timeout_seconds = 0` only as an explicit opt-out that
  disables the idle timeout

### HTTP Runtime Limits

Apply the new config fields during Drogon bootstrap before `run()`:

- set max request body size explicitly
- set max in-memory request body size explicitly
- set idle connection timeout explicitly

This makes request-size and idle-timeout behavior part of the intentional
server contract rather than relying on Drogon defaults.

Do not add inference execution deadlines in this changeset. Time spent inside
the model runtime should remain governed by queueing, cancellation, and the
underlying `zoo-keeper` runtime.

### Shutdown Behavior

Make shutdown behavior explicit and logged:

- install TERM and INT handlers through Drogon
- log shutdown start when one of those signals is handled
- call `app().quit()` from the signal handler
- keep `runtime->stop()` after the Drogon event loop exits
- log shutdown completion after runtime teardown

The shutdown path should remain idempotent.

### Metrics

Keep existing `/metrics` fields unchanged and add these counters:

- `requests_cancelled_total`
- `requests_queue_rejected_total`
- `stream_disconnects_total`

Increment them from server-owned events only:

- increment `requests_cancelled_total` when a request finishes with the
  `RequestCancelled` zoo error
- increment `requests_queue_rejected_total` when request start fails with
  `queue_full`
- increment `stream_disconnects_total` when an active streaming request is
  cancelled from the disconnect path

The `/metrics` response should grow additively. No existing field should be
renamed or have its meaning changed in this changeset.

### Live HTTP Coverage

Add a new live HTTP integration test executable built on the real Drogon app:

- boot the server in a background thread
- bind to port `0`
- read the assigned listener port from `app().getListeners()` once startup
  completes
- drive requests with Drogon `HttpClient`

Use a fake `ChatService` so the tests exercise the real HTTP stack without
depending on a model file.

## Public Interfaces

### Config Contract

Add a top-level `http` object with:

- `client_max_body_size_bytes`
- `client_max_memory_body_size_bytes`
- `idle_connection_timeout_seconds`

Update the example config and docs to show defaults and accepted values.

### Metrics Contract

Append these fields to `/metrics`:

- `requests_cancelled_total`
- `requests_queue_rejected_total`
- `stream_disconnects_total`

Keep the existing fields stable:

- `requests_total`
- `requests_errors`
- `active_sessions`
- `model_id`
- `uptime_seconds`

### Non-Goals

Do not change:

- auth model
- session ownership semantics
- chat request schema
- TLS or reverse-proxy deployment model
- tenant-aware quotas or isolation

## Test Plan

Extend config coverage to include:

- `http` defaults when omitted
- explicit `http` overrides
- unknown `http` keys rejected
- invalid `http` numeric values rejected

Add a new live HTTP integration test that verifies:

- `GET /healthz` returns success when the runtime is ready
- protected endpoints reject bad or missing auth
- oversized `POST /v1/chat/completions` returns `413` when the configured body
  limit is intentionally set low
- a normal stateless completion returns `200` with the expected JSON body
- `/metrics` includes both the old fields and the new additive counters

Add targeted metric-path coverage for:

- `queue_full` increments `requests_queue_rejected_total`
- `RequestCancelled` increments `requests_cancelled_total`
- streaming disconnect cancellation increments `stream_disconnects_total`

Keep the full existing CTest suite green after adding the new test.

## Assumptions

- this changeset is app-level only
- framework-generated `413` responses may keep Drogon's default body shape
- public-internet readiness should come after this hardening pass
- multi-tenant support should come after public-internet readiness, not before
