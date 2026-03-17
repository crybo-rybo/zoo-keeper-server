---
description: Rules for session state management
globs: ["src/server/session_manager.*"]
---

# Session Management

## Pure state store
`SessionStore` is a pure state store — it has no knowledge of `zoo::Agent`, `CompletionHandle`, or token callbacks. Completion orchestration lives in `ZooChatService`. Do not add agent or completion logic here.

## Thread safety
All public methods acquire the internal `mutex_`. Individual sessions also have their own `mutex` for fine-grained locking during `begin_request` / `commit_result` / `release_request`.

## TTL reaping
`reap_expired_sessions()` is called periodically by the background task pool in `ServerRuntime`. It collects expired sessions under the store lock, then destroys them outside the lock to avoid holding it during cleanup.

## Disabled mode
When `max_sessions = 0`, the store is disabled. All session operations immediately return `disabled_error()`. The `enabled()` check is the first thing in every public method — do not add code paths that bypass it.

## Active request tracking
Each session tracks at most one `active_request` (by request ID). `begin_request()` sets it, `commit_result()` / `release_request()` clears it. A session with an active request rejects new requests with a 409 conflict. This prevents concurrent completions from corrupting history.

## History management
`commit_result()` appends both the user message and assistant response to history on success. `max_history_messages_` trims oldest messages (preserving the system prompt) to bound memory.
