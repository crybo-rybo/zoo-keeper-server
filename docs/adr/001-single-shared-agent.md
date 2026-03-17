# ADR-001: Single Shared Agent

## Status
Accepted

## Context
LLM models are large (hundreds of MB to tens of GB). Loading a model into memory is slow (seconds to minutes) and consumes significant RAM/VRAM. A naive per-session or per-request agent design would either require loading multiple model copies or serializing all requests through a single load cycle anyway.

## Decision
One `zoo::Agent` instance is loaded at startup and shared across all requests. Sessions manage conversation history only — they do not get their own agent instances. A request queue (`request_queue_capacity`) serializes access to the agent.

## Consequences
- **Memory:** Only one model copy in memory regardless of concurrent sessions.
- **Startup:** Model loads once; subsequent requests are fast.
- **Session design:** `SessionStore` is a pure history store with no agent or completion knowledge.
- **Concurrency:** Requests are queued, not parallelized. Throughput is bounded by single-model inference speed. The `request_queue_capacity` config controls backpressure.
- **Scaling:** To serve more concurrent users, run multiple server instances behind a load balancer rather than loading multiple models in one process.
