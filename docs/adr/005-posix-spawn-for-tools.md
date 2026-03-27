# ADR-005: posix_spawn for Tool Execution

## Status
Accepted

## Context
The server supports command-line tools that LLMs can invoke. Tool commands must execute safely without shell injection vulnerabilities. `system()` and `popen()` invoke a shell, making them vulnerable to argument injection if tool parameters contain shell metacharacters.

## Decision
Use `posix_spawn()` for all tool execution. Commands are passed as an argv array — no shell is involved.

## Consequences
- **No shell injection:** Arguments are passed directly to the executable, bypassing shell interpretation entirely.
- **Explicit environment:** Tool processes get a controlled environment. `inherit_environment` defaults to `false`; specific env vars are set via the `env` config field.
- **FD management:** stdin is closed (tools don't read input), stdout and stderr are captured via pipe FDs. RAII wrappers (`PipePair`) ensure FDs are closed on all paths.
- **Timeout enforcement:** A background thread monitors `waitpid()` and sends `SIGKILL` if the tool exceeds `timeout_ms`.
- **Trade-off:** No shell features (pipes, redirects, globbing) in tool commands. Commands must be direct executables. This is intentional — shell features would reintroduce injection risks.
