# Zoo-Keeper v1.1.0 Migration Design

## Context

The zoo-keeper submodule (`extern/zoo-keeper/`) is pinned to v1.0.1 (commit `344b170`). The v1.1.0 release (73 commits ahead) introduces breaking API changes across configuration, request handling, response types, and tool calling. This migration updates zoo-keeper-server to consume v1.1.0 while preserving backward compatibility in the server's external config format and adding per-request sampling overrides to match the OpenAI API.

## Scope

**In scope:**
- Bump submodule to v1.1.0
- Adapt all integration points (~40 touchpoints across 12+ source files)
- Add per-request sampling parameters to chat completions (OpenAI-compatible)
- Preserve existing `server.example.json` config format (no breaking change for users)
- Refactor: eliminate `adapt_zoo_future`, remove `build_tool_system_prompt` call (removed in v1.1.0)

**Out of scope:**
- New `/v1/extractions` endpoint (future PR)
- New extraction-related types in the server API layer

## Breaking Changes in v1.1.0 (Confirmed from Headers)

| Change | Old (v1.0.1) | New (v1.1.0) |
|--------|-------------|-------------|
| Config | Single `zoo::Config` | `ModelConfig` + `AgentConfig` + `GenerationOptions` |
| Agent creation | `Agent::create(Config)` | `Agent::create(ModelConfig, AgentConfig, GenerationOptions)` |
| Request handle | `RequestHandle` struct, `.id` / `.future` fields | `RequestHandle<TextResponse>` template, `.id()` / `.await_result()` methods |
| Response | `zoo::Response` | `zoo::TextResponse` with `optional<ToolTrace>` |
| Complete API | `complete(vector<Message>, callback)` | `complete(ConversationView, GenerationOptions, AsyncTextCallback)` |
| Callback type | `TokenCallback` (already `string_view`) | `AsyncTextCallback = function<void(string_view)>` (same signature) |
| Tool invocations | `vector<ToolInvocation>` on Response | `optional<ToolTrace>` on TextResponse (only when `record_tool_trace = true`) |
| `build_tool_system_prompt()` | Exists on Agent | **Removed** -- native template-driven tool calling |
| `system_prompt` in config | Part of `zoo::Config` | Not in any config type; use `Agent::set_system_prompt()` |
| `get_config()` | Returns `const Config&` | Replaced by `model_config()`, `agent_config()`, `default_generation_options()` |
| New error codes | -- | `InvalidOutputSchema (600)`, `ExtractionFailed (601)` |

**Preserved:** `using Message = OwnedMessage` transitional alias exists in v1.1.0. `zoo::Message` still compiles. `ToolInvocation`, `ToolInvocationStatus`, `ToolMetadata`, `use_mlock` all preserved.

## Architecture

### Config Mapping (no external breaking change)

The flat `"zoo"` JSON block in `server.example.json` is unchanged. The server writes its own `from_json` that reads the flat block and delegates to zoo's individual `from_json` overloads for `ModelConfig`, `AgentConfig`, and `GenerationOptions` (all three have `from_json` in `zoo/core/json.hpp`).

**Note:** The old `zoo::Config` `from_json` no longer exists. The server's custom parser must handle `reject_unknown_keys` for the combined flat key space.

```
"zoo" JSON block
├── model_path, context_size, n_gpu_layers, use_mmap, use_mlock  →  ModelConfig  (via zoo's from_json)
├── max_history_messages, request_queue_capacity,                 →  AgentConfig  (via zoo's from_json)
│   max_tool_iterations, max_tool_retries
├── max_tokens, sampling: {...}                                   →  GenerationOptions (via zoo's from_json)
├── system_prompt                                                 →  ServerConfig::system_prompt (server-owned)
└── (tools are configured separately)
```

`ServerConfig` changes from:
```cpp
zoo::Config zoo_config;
```
to:
```cpp
zoo::ModelConfig model_config;
zoo::AgentConfig agent_config;
zoo::GenerationOptions default_generation;
std::string system_prompt;  // server-owned; set via Agent::set_system_prompt() after creation
```

**System prompt flow:** `system_prompt` is no longer part of any zoo config type. The server reads it from the `"zoo"` block, stores it in `ServerConfig`, and calls `agent->set_system_prompt(config.system_prompt)` during agent creation in `chat_service.cpp`. This replaces the old `base_config.system_prompt` access in `create_configured_agent()`.

### Per-Request Sampling Overrides

`ChatCompletionRequest` gains optional sampling fields:
```cpp
std::optional<float> temperature;
std::optional<float> top_p;
std::optional<int> top_k;
std::optional<float> repeat_penalty;
std::optional<int> max_tokens;
std::optional<int> seed;
std::optional<std::vector<std::string>> stop;  // maps to GenerationOptions::stop_sequences
```

A new `merge_request_overrides()` function in `zoo_adapter` merges these onto the server's `default_generation` to produce per-request `GenerationOptions`:
```cpp
zoo::GenerationOptions merge_request_overrides(
    const zoo::GenerationOptions& defaults,
    const ChatCompletionRequest& request);
```

Called in `start_completion` and `start_session_completion` before `agent_->complete()`.

### Request Handle Adaptation

`wrap_zoo_handle()` absorbs the template change and eliminates `adapt_zoo_future()`:

```cpp
CompletionHandle wrap_zoo_handle(zoo::RequestHandle<zoo::TextResponse> handle) {
    auto request_id = static_cast<uint64_t>(handle.id());
    auto future = std::async(std::launch::async, [h = std::move(handle)]() mutable {
        auto result = h.await_result();
        if (!result) return RuntimeResult<CompletionResult>{std::unexpected(result.error())};
        return RuntimeResult<CompletionResult>{from_zoo_response(*result)};
    });
    return make_completion_handle(request_id, std::move(future));
}
```

### Response Adaptation

`from_zoo_response` changes from `zoo::Response` to `zoo::TextResponse`:
- `response.tool_invocations` → `response.tool_trace->invocations` (guarded by `if (response.tool_trace)`)
- All other field mappings (text, usage, metrics) are structurally identical
- `CompletionResult::tool_invocations` vector stays the same server-side; the adaptation extracts from the optional `ToolTrace`

### Type Alias Updates (`api_types.hpp`)

| Current Alias | v1.1.0 Status | Action |
|--------------|---------------|--------|
| `ChatMessage = zoo::Message` | `using Message = OwnedMessage` preserved | **Keep as-is** (transitional alias) |
| `ToolInvocationRecord = zoo::ToolInvocation` | Unchanged | Keep |
| `ToolInvocationStatus = zoo::ToolInvocationStatus` | Unchanged | Keep |
| `RuntimeError = zoo::Error` | Unchanged | Keep |
| `RuntimeResult<T> = zoo::Expected<T>` | Unchanged | Keep |
| `TokenCallback` | Server already uses `function<void(string_view)>` | Keep |

### Tool Registration

`Agent::register_tool()` still exists with the same JSON-handler overload used by the server. **However, `build_tool_system_prompt()` is removed.** In v1.1.0, tool calling uses native llama.cpp chat templates — the model's template knows how to format tool definitions. The `configure_tools` function in `chat_service.cpp` must:

1. Keep calling `agent->register_tool(...)` (signature unchanged)
2. Remove the `build_tool_system_prompt()` call (line 62)
3. The `request_system_prompt` concept simplifies — it's just the user's `system_prompt`, no longer concatenated with tool instructions

### Tool Trace Opt-In

v1.1.0 only materializes `ToolTrace` when `GenerationOptions::record_tool_trace = true`. The server currently exposes `tool_invocations` in the API response, so **`record_tool_trace` must be set to `true` in the default generation options when tools are configured**. This should be done in `configure_tools()` in `chat_service.cpp` — after registering tools, set `default_generation_.record_tool_trace = true`. When no tools are configured, leave it `false` (no trace to record).

### ConversationView Construction

`ConversationView` accepts `std::span<const OwnedMessage>` directly. Since `ChatMessage = zoo::Message = OwnedMessage`, the existing `std::vector<ChatMessage>` from `prepare_messages()` converts implicitly:

```cpp
auto messages = prepare_messages(request);
auto view = zoo::ConversationView(std::span<const ChatMessage>(messages));
auto handle = agent_->complete(view, gen_options, std::move(callback));
```

Lifetime is safe because `messages` lives through the `complete()` call (it queues the request synchronously).

## Files to Modify

### Config Layer
- `src/server/config.hpp` — Replace `zoo::Config zoo_config` with `ModelConfig`, `AgentConfig`, `GenerationOptions`, `system_prompt`
- `src/server/config.cpp` — Custom `from_json`: extract sub-objects from flat `"zoo"` block, delegate to zoo's individual `from_json` overloads, own `reject_unknown_keys` for the combined key space, handle `system_prompt` separately. Update `validate()`.
- `src/server/runtime.cpp` — `config.zoo_config.request_queue_capacity` → `config.agent_config.request_queue_capacity`

### Type/Adapter Layer
- `src/server/api_types.hpp` — Add sampling fields to `ChatCompletionRequest`. Type aliases unchanged.
- `src/server/zoo_adapter.hpp` — `zoo::Response` → `zoo::TextResponse`, add `merge_request_overrides()` declaration
- `src/server/zoo_adapter.cpp` — Implement updated `from_zoo_response` (ToolTrace extraction), implement `merge_request_overrides`

### Agent/Completion Layer
- `src/server/chat_service.hpp` — Add `default_generation_` member to `ZooChatService`, update constructor
- `src/server/chat_service.cpp` — Agent creation with 3 configs + `set_system_prompt()`. `complete()` with `ConversationView` + `GenerationOptions`. Remove `adapt_zoo_future()`. Update `wrap_zoo_handle()`. Remove `build_tool_system_prompt()` call. Simplify `request_system_prompt_` (no longer tool-concatenated).

### Session Layer
- `src/server/session_manager.hpp` — Uses `ChatMessage` (alias still works, no changes needed to header)
- `src/server/session_manager.cpp` — Uses `ChatMessage::system()`, `ChatMessage::assistant()` static factories (still work via `OwnedMessage` transitional alias). **No changes expected**, but verify compilation.

### JSON/API Layer
- `src/server/api_json.cpp` — Parse new sampling fields in `parse_chat_completion_request`, add to allowed keys. Map new error codes (`InvalidOutputSchema`, `ExtractionFailed`) in `map_runtime_error_to_api_error`.
- `src/server/completion_controller.cpp` — Uses `CompletionResult::tool_invocations` (server type, not zoo type). **No changes needed** — adaptation is contained in `zoo_adapter.cpp::from_zoo_response()`.

### Command Tools
- `src/server/command_tools.cpp` — Uses `ToolProvider` (server type). Tool registration still uses same `register_tool` API. **No changes expected**, verify compilation.

### Smoke Tests
- `src/link_smoke.cpp` — `zoo::Config{}` → `zoo::ModelConfig{}`, check `InvalidModelPath` still works
- `src/live_smoke.cpp` — Split config into 3 types, `Agent::create(model, agent, gen)`, replace `chat()` with `complete()`, use `handle.await_result()` instead of `handle.future.get()`

### Documentation
- `docs/openapi.yaml` — Add sampling parameters to `ChatCompletionRequest` schema, add error codes 600/601
- `docs/error-reference.md` — Add `InvalidOutputSchema` and `ExtractionFailed` error codes

### Tests
- `tests/config_test.cpp` — Verify flat JSON populates all three config types. Add regression test for `reject_unknown_keys` on the zoo block (server now owns this).
- `tests/api_test.cpp` — Update `UnknownRequestFieldRejected` (temperature now valid). Add tests for: parsing with sampling overrides, partial overrides, boundary values.
- `tests/runtime_test.cpp` / `tests/http_integration_test.cpp` — Update any direct `zoo::Config` construction to three config types
- New: `tests/zoo_adapter_test.cpp` or add to existing — Unit tests for `merge_request_overrides()`: no overrides (passthrough), partial, boundary values (temperature=0.0, seed=-1)

## Verification

1. **Compile** — `scripts/build` must pass cleanly
2. **Unit tests** — `scripts/test` must pass all existing + new tests
3. **Format** — `scripts/format-check` must pass
4. **Config backward compat** — Existing `server.example.json` loads without changes
5. **reject_unknown_keys** — Typos in `"zoo"` block still rejected (regression test)
6. **Smoke test** — `link_smoke` passes (config validation)
7. **Integration test** — If a GGUF model is available, `live_smoke` + manual curl with streaming + per-request sampling
8. **Per-request sampling** — Dedicated unit tests for `merge_request_overrides()` covering passthrough, partial, and boundary cases

## Implementation Order

1. Bump `extern/zoo-keeper` submodule to v1.1.0
2. Attempt compile to discover all breaks
3. Config layer — config.hpp, config.cpp, runtime.cpp
4. Type aliases and adapter — api_types.hpp, zoo_adapter.hpp/cpp
5. Chat service — chat_service.hpp/cpp (largest change, includes tool registration cleanup)
6. JSON/API parsing — api_json.cpp (per-request sampling fields, new error codes)
7. Smoke tests — link_smoke.cpp, live_smoke.cpp
8. Unit test updates — config_test, api_test, adapter tests, integration tests
9. Documentation — openapi.yaml, error-reference.md
10. Final verification — full build + test suite

## Risks

- **`zoo::Config` `from_json` removed**: The server currently delegates all zoo JSON parsing to the submodule's `from_json(json, Config&)`. In v1.1.0, this overload no longer exists. The server must write its own flat-to-split parser. This is the most labor-intensive config change. Mitigated by reusing zoo's individual `from_json` overloads for each sub-config.
- **`build_tool_system_prompt()` removed**: The server calls this to generate tool instructions for the system prompt. v1.1.0 uses native template-driven tool calling instead. The `request_system_prompt_` field and `configure_tools` flow must be simplified. Tools are still registered via `register_tool()`.
- **ConversationView lifetime**: `prepare_messages()` returns by value, so the vector lives through `agent_->complete()` which queues synchronously. Safe.
- **`validate_role_sequence` now templated**: v1.1.0 templates this over the history container type. The server's `validate_message_sequence` calls zoo's validator — if the signature changed, the wrapper adapts. The new template should accept `vector<OwnedMessage>` directly.
- **Tool trace opt-in**: v1.1.0 only materializes `ToolTrace` when `GenerationOptions::record_tool_trace = true`. Handled in the "Tool Trace Opt-In" section above — set in `configure_tools()` when tools are registered.
