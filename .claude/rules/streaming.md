---
description: Rules for SSE streaming and completion controller
globs: ["src/server/streaming.*", "src/server/completion_controller.*"]
---

# Streaming & Completion Controller

## SSE wire format
- Each data frame: `data: {json}\n\n`
- Stream terminator: `data: [DONE]\n\n`
- Use `make_sse_data()` and `make_sse_done()` from `streaming.hpp` — never hand-build SSE frames.

## First chunk vs subsequent chunks
- `make_first_streaming_chunk()` — includes `"role": "assistant"` in the delta. Used only for the first token.
- `make_streaming_chunk()` — delta only, no role field. Used for all subsequent tokens.
- Getting this wrong breaks OpenAI-compatible client parsers.

## Deferred-action pattern
The completion controller buffers metadata (completion ID, created timestamp, model) before streaming begins. This ensures the first chunk has correct metadata even though the token callback fires asynchronously. Do not emit any SSE data before `start_completion()` returns successfully.

## Mutex discipline
All mutable state in the streaming path is guarded by `mutex_`. Do not introduce additional atomics alongside the mutex — the codebase has a known issue with mutex/atomic hybrid patterns. Keep synchronization simple: one mutex per shared-state scope.

## Completion lifecycle
- `PendingChatCompletion` holds the `CompletionHandle` (future), cancel function, observer, and `CompletionLease`.
- `CompletionLease` is RAII — it releases the session request slot on destruction. Always ensure the lease is released, even on error paths.
- `finalize_completion()` calls the observer then releases the lease. `release_completion()` releases without observing.
