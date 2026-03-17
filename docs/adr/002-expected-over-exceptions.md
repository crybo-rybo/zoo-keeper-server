# ADR-002: std::expected Over Exceptions

## Status
Accepted

## Context
C++ exceptions have runtime overhead (stack unwinding, RTTI), make error paths implicit, and are difficult for AI coding agents to reason about because the set of throwable types is not visible in function signatures. The project needs explicit, composable error handling that is clear to both humans and agents.

## Decision
Use `std::expected<T, E>` for all fallible operations:
- `Result<T>` = `std::expected<T, std::string>` — for internal/config errors
- `ApiResult<T>` = `std::expected<T, ApiError>` — for HTTP-layer errors
- `RuntimeResult<T>` = `zoo::Expected<T>` — for agent/inference errors

No `try`/`catch` blocks in the codebase. Errors propagate explicitly via return values.

## Consequences
- **Signatures are honest:** Every function that can fail says so in its return type.
- **No hidden control flow:** Error paths are visible at every call site.
- **Composability:** Errors can be mapped between layers (e.g., `map_runtime_error_to_api_error()`).
- **Constraint:** All team members and agents must propagate errors explicitly — forgetting to check a result is a bug. The `[[nodiscard]]` attribute enforces this at compile time where applied.
