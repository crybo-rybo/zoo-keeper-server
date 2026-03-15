# Zoo-Keeper-Server Comprehensive Review Roadmap

**Reviewed:** 2026-03-14
**Scope:** Full codebase audit — architecture, implementation, performance, security, testing, documentation, build system
**Reviewer model:** Claude Opus 4.6 (principal engineer persona)

---

## Executive Summary

Zoo-keeper-server is a well-structured, focused C++23 HTTP server wrapping llama.cpp for local LLM inference. The architecture is clean — single shared agent, clear separation between HTTP routing, service orchestration, session management, and adapter layers. Error handling with `std::expected` is used consistently. The codebase is relatively small (~3,500 lines of project code) and coherent.

That said, a principal-engineer-level review reveals issues across security, code duplication, thread safety, test infrastructure, and performance. Below is every finding, ranked by priority with a recommended completion order.

---

## Severity Levels

| Level | Meaning |
|-------|---------|
| **P0 — Critical** | Security vulnerability or correctness bug that could cause data loss, crashes, or exploitation |
| **P1 — High** | Architectural debt, race conditions, or missing safety nets that will bite as the project scales |
| **P2 — Medium** | Code quality, performance, and maintainability issues that slow development velocity |
| **P3 — Low** | Polish, documentation, and nice-to-haves that demonstrate engineering maturity |

---

## P0 — Critical

### 1. API Key Comparison Is Not Constant-Time
**File:** `src/server/auth.hpp:22`
**Issue:** `auth_header != "Bearer " + *config.api_key` uses `std::string::operator!=`, which short-circuits on first mismatched byte. An attacker can extract the API key one character at a time via timing side-channel.
**Fix:** Implement a constant-time comparison function (length-check first, then XOR-accumulate every byte). This is table-stakes for any secret comparison.
**Impact:** Any deployment with `api_key` set and a network-accessible `bind_address` is vulnerable.

### 2. StreamingSession Mixes Mutex and Atomics Inconsistently
**File:** `src/server/completion_controller.cpp:76-242`
**Issue:** `sent_role_` and `sent_content_` are `std::atomic<bool>` but are read outside the mutex in `push_token()` (line 143-146) and `finish_success()` (line 150-163), while writes to related state (`metadata_ready_`, `stream_`, `closing_`) happen under the mutex. This creates a window where `push_token` sees `metadata_ready_ == true` (checked under mutex at line 136), drops the mutex, then reads stale atomics or races with `finish_success` which also reads/writes these atomics while doing mutex-protected work.
**Fix:** Either protect all shared state under the mutex (remove the atomics) or use a proper lock-free protocol. The current hybrid is the worst of both worlds — it has the overhead of atomics AND the overhead of a mutex, with neither's correctness guarantee.
**Risk:** Corrupted SSE streams, missing tokens, or duplicate role headers under concurrent load.

### 3. Dangling Raw Pointer to `zoo::Agent` in SessionManager Lambdas
**File:** `src/server/chat_service.cpp:153-160`, `src/server/chat_service.hpp:90-91`
**Issue:** In `ZooChatService::create()`, a raw pointer `agent` is extracted from `shared_agent` via `.get()`, then captured by value in two lambdas stored inside `SessionManager`. The `shared_agent` is subsequently moved into `ZooChatService`. C++ destroys members in reverse declaration order. Since `agent_` (line 90) is declared *before* `session_manager_` (line 91), `session_manager_` is destroyed first — which is correct. **However**, if a `CompletionLease` destructor fires after `agent_` is destroyed (e.g., during `ZooChatService` destruction with an in-flight completion), the lambda will dereference a dangling pointer.
**Fix:** Swap the declaration order of `agent_` and `session_manager_` in `chat_service.hpp` so that `session_manager_` is guaranteed to be destroyed before `agent_`. Alternatively, capture `agent_` as a `shared_ptr` in the lambdas.
**Risk:** Use-after-free / undefined behavior during shutdown with active requests.

### 4. Session Creation Double-Lock TOCTOU
**File:** `src/server/session_manager.cpp:141-198`
**Issue:** `create_session` checks capacity under the lock (line 159), releases the lock, allocates the `SessionState`, then re-acquires the lock and checks capacity again (line 188). Between the two checks, N concurrent callers can all pass the first check, all allocate state, and then race on the second check. While the second check prevents over-insertion, the unnecessary first check + release + re-acquire is confusing and the allocation between locks is wasted work.
**Fix:** Acquire the lock once, check capacity, allocate inline, insert. The current pattern suggests the author was trying to avoid allocating under the lock, but `make_shared<SessionState>` is not expensive enough to warrant this complexity.

---

## P1 — High

### 4. Pervasive Code Duplication
**Files:** Multiple
**Issue:** At least four functions are copy-pasted across files:
- `with_metrics()` — identical in `api_routes.cpp:17-28` and `completion_controller.cpp:21-32`
- `combine_system_prompts()` — identical in `chat_service.cpp:57-66` and `session_manager.cpp:18-27`
- `now_seconds()` — identical in `chat_service.cpp:125-129` and `session_manager.cpp:12-16`
- `reject_unknown_keys()` — three different implementations across `config.cpp:19-40`, `api_json.cpp:9-34`, and `command_tools.cpp:133-154`, each with slightly different signatures but identical logic
**Fix:** Extract into shared internal utilities. For `reject_unknown_keys`, consolidate into a single implementation (the `std::initializer_list<std::string_view>` variant in `command_tools.cpp` is the cleanest).
**Impact:** Any bug fix to one copy will not propagate to the others. This is how subtle inconsistencies accumulate.

### 5. `ServerRuntime` Constructor Is Public Despite Factory Pattern
**File:** `src/server/runtime.hpp:28`
**Issue:** `ServerRuntime::create()` is a static factory that validates config and creates the chat service, but the constructor is public. Tests (e.g., `http_integration_test.cpp:59`) bypass `create()` and construct directly with a `FakeChatService`, skipping all validation. The constructor even takes `ServerConfig` by value — a moved-from config may have invalid state.
**Fix:** Make the constructor private and add a friend declaration for tests, or provide a separate test-only factory. The current API is an invitation for misuse.

### 6. No Test Framework
**Files:** All `tests/*.cpp`
**Issue:** Every test file reimplements the same pattern: `main()` → sequential `if/cerr/return 1` checks. There are no test names in output, no assertion macros, no test isolation, no parallel execution, no setup/teardown. The `fail()` helper is defined identically in 5+ files. A failing test gives you a string but no file/line number.
**Fix:** Adopt a lightweight test framework. Catch2 or doctest would integrate trivially with CMake and CTest. This is the single highest-leverage infrastructure investment for the project.
**Impact:** Writing new tests is friction-heavy, failures are hard to debug, and there's no test discovery.

### 7. `prepare_messages` Copies and Inserts at Front
**File:** `src/server/chat_service.cpp:264-279`
**Issue:** `auto messages = request.messages;` copies the entire vector, then `messages.insert(messages.begin(), ...)` shifts every element right. For a 100-message transcript, that's a full copy plus O(n) shift.
**Fix:** Construct the output vector with the system message first, then append. Or return a view/span if the caller doesn't need ownership.

### 8. Per-Token JSON Allocation in SSE Streaming
**File:** `src/server/streaming.cpp:13-35`
**Issue:** `make_chat_completion_chunk` constructs a full `nlohmann::json` object tree per token, serializes it to string, and discards it. For a 500-token response, that's 500 JSON object allocations, 500 `json::dump()` calls, each producing ~200 bytes of mostly-static structure.
**Fix:** Use a pre-formatted template string with token content spliced in. The SSE chunk format is fixed — only `content` and `finish_reason` vary. A `fmt::format` or string concatenation approach would be 10-50x faster for the hot path.

### 9. Session History Copied Every Completion
**File:** `src/server/session_manager.cpp:308`
**Issue:** `full_messages = session->history;` copies the entire conversation history vector (which can contain large message strings) under the session lock. This copy is necessary because the lock must be released before calling `completion_starter_`, but the cost grows linearly with conversation length.
**Fix:** Consider moving the messages out (the session will rebuild history from the completion result), or using a copy-on-write data structure. At minimum, `reserve()` the output vector.

### 10. No Streaming Completion Tests
**Files:** `tests/`
**Issue:** There are zero tests covering the SSE streaming path. `StreamingSession` (240 lines of complex mutex + atomic + buffered frame logic) is entirely untested. The `FakeChatService` has a `StreamingSuccess` mode that is defined but never exercised in any test file.
**Fix:** Add tests for: (a) token-by-token streaming, (b) early client disconnect during streaming, (c) metadata arriving before/after tokens, (d) error during streaming after partial content sent.

---

## P2 — Medium

### 11. `BoundedExecutor` Is Untested
**File:** `src/server/executor.cpp`
**Issue:** The thread pool executor has no dedicated unit tests. It's only indirectly tested via `runtime_test.cpp` which exercises `submit_background` → `stop`. There are no tests for: queue-full rejection, multiple concurrent tasks, stop-while-tasks-queued behavior, exception handling in workers.
**Fix:** Add dedicated executor tests.

### 12. `zoo_adapter` Layer Is Untested
**File:** `src/server/zoo_adapter.cpp`
**Issue:** 248 lines of type-conversion code between `zks::server` types and `zoo::` types with no tests. Every `switch` statement has an unreachable default that returns a fallback value — if a new enum variant is added upstream, the adapter silently returns a wrong value instead of failing.
**Fix:** Add round-trip tests for every conversion function. Consider `static_assert`ing enum sizes or using `[[fallthrough]]` / `__builtin_unreachable()` to make missing cases a compile error.

### 13. `reject_unknown_keys` Uses O(n*m) Linear Scan
**Files:** `config.cpp:26-29`, `api_json.cpp:19-22`, `command_tools.cpp:141-143`
**Issue:** For each key in the JSON object, all three implementations iterate the allowed-keys array. For small N this is fine, but it's also trivially fixable and the pattern is repeated in the hottest code path (request parsing).
**Fix:** Use `std::unordered_set` or `std::array` with `std::ranges::find`. Or since the arrays are compile-time constants, a `constexpr` sorted array with binary search.

### 14. No `.clang-format` in Project Root
**Issue:** CLAUDE.md says "Follow the `.clang-format` in `extern/zoo-keeper/`" but there's no `.clang-format` in the project root. Developers must know to copy or symlink it. Some editors won't traverse into submodules to find format configs.
**Fix:** Add a `.clang-format` at the project root (or symlink to `extern/zoo-keeper/.clang-format`).

### 15. Test UI HTML Embedded as 1050-Line String Literal
**File:** `src/server/test_ui.cpp:20-1053`
**Issue:** Over 1000 lines of HTML/CSS/JS are embedded as `constexpr std::string_view` literals. This makes the UI unmaintainable (no syntax highlighting, no linting, no hot reload) and inflates the binary by ~30KB even in non-test-UI builds (the strings are in the translation unit even though the file is conditionally compiled — this is fine, but the maintainability cost is real).
**Fix:** Move the HTML to a separate file and either embed via `cmrc` / `xxd` at build time, or serve from the filesystem in test-only mode.

### 16. No CORS Headers
**Issue:** The test UI at `/_test` makes same-origin requests, but if anyone tries to use the API from a browser-based client on a different origin, all requests will fail silently due to missing `Access-Control-Allow-Origin` headers. There's no OPTIONS handler for preflight requests.
**Fix:** Add configurable CORS support. At minimum, the test UI should work cross-origin during development.

### 17. `health.hpp` vs `api_routes.hpp` Constness Inconsistency
**Files:** `src/server/health.hpp:13`, `src/server/api_routes.hpp:33`
**Issue:** `register_health_routes` takes `std::shared_ptr<const ServerRuntime>` but `register_api_routes` takes `std::shared_ptr<ServerRuntime>`. This inconsistency means health routes can work with a const runtime but API routes require a mutable one. The distinction is correct (API routes modify metrics), but the inconsistency should be documented or the API routes should take a non-const reference to the mutable parts only.

### 18. Command Tool `PreparedCommandTool` Copied Into Lambda
**File:** `src/server/command_tools.cpp:863-877`
**Issue:** `make_command_tool_provider` captures `PreparedCommandTool` (which contains `CommandToolConfig` with `nlohmann::json` schema, `std::vector<std::string>` command, `std::map<std::string, std::string>` env, plus `std::vector<std::string>` env_entries) by copy into each tool's invoke lambda. Every tool invocation re-reads these copies. The copies are made once at startup, so the performance cost is at lambda creation, not per-invocation — but the memory footprint is doubled unnecessarily.
**Fix:** Use `std::shared_ptr<const PreparedCommandTool>` to share the prepared tool across the lambda and the owner.

### 19. `CommandToolConfig::validate()` Split Across Two Layers
**Files:** `src/server/command_tools.cpp:801-833` (validate), `src/server/command_tools.cpp:310-334` (build_prepared_command_tool)
**Issue:** `validate()` checks syntactic validity (non-empty name, valid schema, etc.) but doesn't check if the executable exists. That happens in `build_prepared_command_tool()`. Config parsing calls `validate()`, so `load_config` succeeds for a missing executable — the error only surfaces later in `ServerRuntime::create()`. This two-phase validation is confusing.
**Fix:** Either document this clearly or move executable resolution into `validate()` so config loading fails fast.

### 20. No RAII Wrappers for POSIX Resources in `command_tools.cpp`
**File:** `src/server/command_tools.cpp:338-746`
**Issue:** `run_prepared_command_tool` manually manages six file descriptors, `posix_spawn_file_actions_t`, and `posix_spawnattr_t` with ad-hoc lambdas and manual cleanup at every error path. The function is 400+ lines long. Every early-return path must remember to close pipes, destroy spawn attributes, and kill/wait the child process. This is the kind of code that leaks FDs in edge cases.
**Fix:** Introduce RAII wrappers: `ScopedFd`, `ScopedPipePair`, `ScopedSpawnActions`. This would halve the function length and eliminate entire categories of resource leak bugs.

### 21. `MetricsSnapshot` Manually Populated
**File:** `src/server/runtime.cpp:77-91`
**Issue:** `metrics_snapshot()` manually copies every field from `ServerMetrics` atomics into `MetricsSnapshot`. If a new metric is added to `ServerMetrics`, it's easy to forget to add it to the snapshot, the `MetricsSnapshot` struct, AND the JSON serialization in `api_routes.cpp:228-239`.
**Fix:** Either generate the snapshot from a list of metric definitions, or have `ServerMetrics` itself produce the snapshot via a `snapshot()` method.

---

## P3 — Low

### 21. `CompletionHandle::get()` Declared `const` But Mutates Internal State
**File:** `src/server/api_types.hpp:163`
**Issue:** `RuntimeResult<CompletionResult> get() const;` is declared `const` but calls through to `CompletionSource::get()` which is non-const (sets `consumed_ = true`). The constness is a lie enabled by `shared_ptr` indirection. Callers are misled about whether `get()` is safe to call multiple times or from const contexts.
**Fix:** Remove `const` from `CompletionHandle::get()`. This is a semantic correctness fix — the method is a consuming operation.

### 22. No Doxygen Comments on Public Headers
**Files:** All `src/server/*.hpp`
**Issue:** Only `auth.hpp` has a single `///` comment. No public class or function has documentation. `ChatService` is a key interface with 7 pure virtual methods and zero documentation on expected behavior, thread safety guarantees, or ownership semantics.
**Fix:** Add `///` documentation to at least: `ChatService`, `ServerRuntime`, `SessionManager`, `BoundedExecutor`, `CompletionSource`, and all public factory functions.

### 22. No OpenAPI / Swagger Specification
**Issue:** The API is documented only in `README.md` prose and `docs/error-reference.md`. There's no machine-readable API spec. This makes client SDK generation, contract testing, and API documentation hosting impossible.
**Fix:** Add an `openapi.yaml` or serve one from the server itself.

### 23. No `install()` Target in CMake
**File:** `CMakeLists.txt`
**Issue:** `cmake --install` does nothing. There's no way to package or deploy the binary via standard CMake mechanisms.
**Fix:** Add `install(TARGETS zoo_keeper_server DESTINATION bin)` and optionally install config templates.

### 24. Version Not Embedded in Binary or API
**Issue:** `project(zoo-keeper-server VERSION 0.0.1)` is the only version marker. The binary has no `--version` flag, `/healthz` doesn't include a version field, and there's no compile-time version header.
**Fix:** Generate a `version.hpp` from CMake, add `--version` to main, include version in `/healthz` response.

### 25. `README.md` References Non-Existent Docs
**File:** `README.md:68-69`
**Issue:** README references `docs/v0.0.1-roadmap.md` and `docs/v0.0.1-release-notes.md` but the `docs/` directory only contains `error-reference.md`, `images/`, and `test-ui.md`. These are dead links.
**Fix:** Either create the referenced files or remove the links.

### 26. `CLAUDE.md` Missing Documentation for Several Config Sections
**File:** `CLAUDE.md`
**Issue:** The `http` config section (body size limits, idle timeout), `api_key`, `tools`, and the `zoo.request_queue_capacity` / `zoo.max_history_messages` fields are not documented in CLAUDE.md. The config example shows only the basic fields.
**Fix:** Update the CLAUDE.md config section to match the full config schema as documented in README.md.

### 27. Multiple Config Example Files Without Explanation
**Files:** `config/api-key-enabled.json`, `config/cpu-only.json`, `config/metal.json`, `config/sessions-disabled.json`
**Issue:** Four additional config files beyond `server.example.json` exist but are not documented anywhere. Their purpose is only discoverable by reading them.
**Fix:** Add a brief comment or a `config/README.md` explaining what each config is for.

### 28. CI Workflow Missing Coverage, Sanitizers, and Format Check
**File:** `.github/workflows/build_and_test.yml`
**Issue:** The CI only builds and runs tests. There's no:
- clang-format check
- clang-tidy or static analysis
- Address/Thread/UB sanitizer builds
- Code coverage reporting
**Fix:** Add sanitizer builds (at least ASan + UBSan), a format check step, and optionally coverage.

### 29. Detached Thread in `FakeChatService`
**File:** `tests/fake_chat_service.hpp:183`
**Issue:** `std::thread(...).detach()` in `StreamingSuccess` mode. Detached threads can outlive the test process and cause spurious failures or resource leaks. Even though this is test code, it sets a bad precedent.
**Fix:** Store the thread and join in the destructor.

### 30. `fail()` Helper Redefined in Every Test File
**Files:** `tests/session_manager_test.cpp:16-19`, `tests/http_integration_test.cpp:22-25`, `tests/runtime_test.cpp:15-18`, `tests/auth_test.cpp:7-10`, `tests/metrics_test.cpp:6-9`
**Issue:** Five identical `fail()` functions. This is a direct consequence of having no test framework.
**Fix:** Will be resolved by adopting a test framework (item #6).

---

## Recommended Completion Order

The ordering below accounts for dependency chains, risk reduction, and development velocity impact.

### Phase 1 — Security & Correctness (Week 1)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 1 | Constant-time API key comparison | P0 | S |
| 2 | Fix StreamingSession mutex/atomic inconsistency | P0 | M |
| 3 | Fix dangling raw pointer in SessionManager lambdas | P0 | S |
| 4 | Fix session creation double-lock TOCTOU | P0 | S |

### Phase 2 — Test Infrastructure (Week 2)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 6 | Adopt test framework (Catch2 or doctest) | P1 | M |
| 30 | Remove duplicated `fail()` helpers (free with #6) | P3 | — |
| 10 | Add streaming completion tests | P1 | M |
| 11 | Add BoundedExecutor tests | P2 | S |
| 12 | Add zoo_adapter round-trip tests | P2 | S |

### Phase 3 — Architecture Cleanup (Week 3)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 5 | Eliminate code duplication (4 functions) | P1 | M |
| 6 | Make ServerRuntime constructor private | P1 | S |
| 15 | Add `.clang-format` to project root | P2 | S |
| 18 | Resolve constness inconsistency in route registration | P2 | S |
| 20 | Add RAII wrappers for POSIX resources in command_tools | P2 | M |
| 19 | Consolidate command tool validation | P2 | S |
| 21 | Auto-generate MetricsSnapshot from ServerMetrics | P2 | M |
| 22 | Fix `CompletionHandle::get()` const-correctness | P2 | S |

### Phase 4 — Performance (Week 4)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 7 | Fix prepare_messages copy + insert-at-front | P1 | S |
| 8 | Optimize per-token SSE chunk serialization | P1 | M |
| 9 | Reduce session history copy overhead | P1 | M |
| 13 | Optimize reject_unknown_keys | P2 | S |
| 18 | Share PreparedCommandTool via shared_ptr | P2 | S |

### Phase 5 — Documentation & Polish (Week 5)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 21 | Add Doxygen comments to public headers | P3 | M |
| 22 | Create OpenAPI spec | P3 | M |
| 25 | Fix dead doc links in README | P3 | S |
| 26 | Update CLAUDE.md config documentation | P3 | S |
| 27 | Document config example files | P3 | S |

### Phase 6 — Build & DevOps (Week 6)
| # | Item | Priority | Effort |
|---|------|----------|--------|
| 23 | Add CMake install target | P3 | S |
| 24 | Embed version in binary and API | P3 | S |
| 28 | Add sanitizers, format check, coverage to CI | P3 | M |
| 15 | Extract test UI HTML to external file | P2 | M |
| 16 | Add CORS support | P2 | M |
| 29 | Fix detached thread in test fake | P3 | S |

---

## Effort Key

| Size | Meaning |
|------|---------|
| **S** | < 1 hour, single file change |
| **M** | 1-4 hours, multi-file change |
| **L** | 4+ hours, architectural change |

---

## What's Good

It would be dishonest to present only criticism. Here's what the codebase gets right:

1. **Clean `std::expected` error handling** — No exceptions in the API layer. Error types are structured with HTTP status, type, param, and code. The `ApiResult<T>` and `RuntimeResult<T>` aliases are used consistently.

2. **Solid session lifecycle** — TTL-based expiry, capacity enforcement, active-request tracking, history trimming, and cancellation on delete. The `CompletionLease` RAII pattern for cleanup is elegant.

3. **Bounded executor with backpressure** — Rather than unbounded thread spawning, the `BoundedExecutor` rejects work when saturated and surfaces that as a `503` to the client. This is a mature pattern.

4. **Graceful shutdown ordering** — `stop()` is idempotent, reaper thread joins on CV notification, executor drains, then chat service stops. The ordering is correct.

5. **Strict request parsing** — Unknown fields are rejected with `400`, message sequences are validated, config keys are whitelisted. This is more rigorous than many production servers.

6. **Good separation of concerns** — `zoo_adapter` cleanly isolates the `zoo::` dependency. `ChatService` is a proper interface with a production and test implementation. Route registration is separated from business logic.

7. **Command tool process isolation** — `posix_spawn` with scrubbed environment, non-blocking I/O, timeout enforcement, stdout/stderr capture limits, and working directory support. This is well-implemented.

8. **CI exists and runs on both macOS and Linux** — Cross-platform testing from day one.

9. **The error-reference.md is excellent** — Comprehensive, well-organized, and accurate. More projects should have this.

10. **Config validation rejects unknown keys** — Typos in config files are caught immediately rather than silently ignored. This is a strong design choice.

---

## Appendix: Performance Optimizations Applied

The performance review applied the following optimizations directly to the codebase (11 files, +162/-54 lines). These changes should be reviewed and tested before merging:

| File | Change | Impact |
|------|--------|--------|
| `streaming.cpp` | Replace per-token `nlohmann::json` construction with direct string building via `append_json_escaped()` | **High** — eliminates ~6 heap allocations per generated token |
| `auth.hpp` | Replace `"Bearer " + key` string allocation with in-place `string::compare()` | **Medium** — eliminates per-request allocation on every authenticated endpoint |
| `chat_service.cpp` | Build `prepare_messages` result with `reserve()` + direct insertion instead of copy + insert-at-front | **Medium** — eliminates O(n) shift and unnecessary deep copy |
| `api_json.cpp` | Use `get_ref<const string&>()` and `std::move` in `parse_message`; `parse_role` takes `string_view` | **Medium** — eliminates redundant string copies during request parsing |
| `session_manager.hpp` | Add `TransparentStringHash`/`TransparentStringEqual` for heterogeneous `string_view` lookup | **Medium** — eliminates `std::string` allocation on every session lookup |
| `session_manager.cpp` | Add `reserve()` on history copy; use heterogeneous `find()` | **Low** — avoids reallocation on history + user message append |
| `completion_controller.cpp` | Capture only `session_id` and `stream` in background lambdas instead of full `ChatCompletionRequest` | **Medium** — eliminates deep copy of entire message array per request |
| `api_routes.cpp` | Add `callbacks.reserve()` in `handle_connection_event` | **Low** |
| `zoo_adapter.cpp` | Remove unreachable duplicate `Tool` case in `from_zoo_message` | **Low** — dead code cleanup |
| `test_ui.cpp` | Single `reserve()` + three `append()` calls instead of `string + string + string` | **Low** |

**Note:** These changes have not been tested. Run `cmake --build build --parallel && ctest --test-dir build --output-on-failure` before accepting them.
