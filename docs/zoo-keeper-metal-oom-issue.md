# Issue Draft: Handle Metal/GGML Out-of-Memory More Gracefully

Suggested repository: `crybo-rybo/zoo-keeper`

Suggested title:

`Metal OOM during inference aborts the host process instead of surfacing a recoverable error`

Suggested body:

```md
## Summary

On macOS with the Metal backend enabled, a device out-of-memory condition during inference currently aborts the entire host process instead of surfacing a `zoo::Error`.

From the `zoo-keeper` caller's perspective, this bypasses the library's normal `Expected<T>` / `ErrorCode` failure model and makes it impossible for the embedding application to degrade gracefully.

## Environment

- `zoo-keeper` commit: `344b1703e0b972d4477fe72e9f9e41d7ea399a3e`
- vendored `llama.cpp` commit: `d1b4757dedbb60a811c8d7012249a96b1b702606`
- OS: macOS 26.3 (`25D125`)
- Arch: `arm64`
- Backend: Metal

## Reproduction

In a `zoo-keeper-server` integration using `zoo::Agent` sessions:

1. Start the server with a Metal-backed model config.
2. Use a sessioned chat flow.
3. Send one streamed request that completes successfully.
4. Send a second streamed request in the same session.

The first request succeeds, but the second request can crash the process with a Metal OOM:

```text
llama_context: n_ctx_seq (2048) < n_ctx_train (131072) -- the full capacity of the model will not be utilized
llama_kv_cache_iswa: using full-size SWA cache (ref: https://github.com/ggml-org/llama.cpp/pull/13194#issuecomment-2868343055)
[session] event=created session_id=sess-1 detail="session ready"
[request] event=start completion_id=chatcmpl-1 model=local-model stream=true session_id=sess-1
[request] event=finish completion_id=chatcmpl-1 model=local-model session_id=sess-1 status=ok prompt_tokens=33 completion_tokens=453 total_tokens=486 latency_ms=30090
[request] event=start completion_id=chatcmpl-2 model=local-model stream=true session_id=sess-1
ggml_metal_synchronize: error: command buffer 0 failed with status 5
error: Insufficient Memory (00000008:kIOGPUCommandBufferCallbackErrorOutOfMemory)
/.../ggml-metal-context.m:235: fatal error
```

Backtrace excerpt:

```text
ggml_metal_synchronize
ggml_backend_sched_synchronize
llama_context::synchronize
llama_get_sampled_token_ith
llama_sampler_sample
zoo::core::Model::run_inference
zoo::core::Model::generate_from_history
zoo::internal::agent::AgentRuntime::process_request
```

## Expected behavior

The embedding application should receive a recoverable error such as:

- backend/device OOM
- configuration exceeds projected device memory
- request/session cannot be executed with current backend settings

That would let callers:

- return a structured API error instead of crashing
- suggest reducing `n_gpu_layers` / context size
- disable session creation for risky configs
- fall back to CPU or a lower-memory profile

## Actual behavior

The process aborts in vendored `llama.cpp` Metal code before `zoo-keeper` can translate the failure into a `zoo::Error`.

The fatal path appears to be:

- `zoo::Agent::create()` loads the backend model via `core::Model::load()`
- inference runs through `zoo::core::Model::run_inference()`
- Metal command-buffer failure reaches `ggml_metal_synchronize()`
- `ggml_metal_synchronize()` calls `GGML_ABORT("fatal error")`

Relevant code references:

- `src/agent.cpp#L22`
- `src/core/model_init.cpp#L15-L84`
- `src/core/model_inference.cpp#L50-L199`
- `include/zoo/core/types.hpp#L104-L139`
- `extern/llama.cpp/ggml/src/ggml-metal/ggml-metal-context.m#L212-L236`

## Why this matters at the `zoo-keeper` layer

I understand that the immediate abort originates in upstream `llama.cpp` / `ggml`, so full recovery may require upstream changes as well.

Even so, `zoo-keeper` still seems like the right place to improve mitigation because it owns:

- configuration validation
- agent/session creation policy
- error taxonomy (`ErrorCode`)
- the boundary where backend failures are translated into application-facing behavior

Right now the library has backend-oriented errors such as `ModelLoadFailed`, `ContextCreationFailed`, and `InferenceFailed`, but there is no dedicated path for device-memory exhaustion and no preflight guard against obviously risky configs.

## Suggested improvements

### 1. Add a dedicated error category for backend/device OOM

Something along the lines of:

- `BackendOutOfMemory`
- `DeviceOutOfMemory`

That would let applications distinguish "bad request / too much history / too many GPU layers" from generic inference failure.

### 2. Add preflight fitting / validation before model load or session creation

The vendored `llama.cpp` already exposes `llama_params_fit(...)` in `include/llama.h` for projecting whether model/context parameters fit device memory.

Using that during initialization (and possibly before creating additional session agents) seems like a practical way to fail early with a structured error instead of letting Metal OOM happen deep in inference.

### 3. Document that Metal OOM is currently fatal

If graceful recovery is not yet possible because the backend aborts the process, it would still help to document:

- that full GPU offload (`n_gpu_layers < 0`) can be risky on larger models
- that session-per-agent setups multiply memory pressure
- that larger retained histories increase pressure over successive turns
- recommended mitigations such as reducing `n_gpu_layers`, reducing `context_size`, or preferring CPU for constrained devices

### 4. Consider session-creation safeguards

If each session creates its own `zoo::Agent` / `llama_context`, the library could reject additional sessions earlier when the current configuration is unlikely to fit another instance safely.

## Additional notes

- This was observed after a successful first streamed turn and a failing second streamed turn in the same session, which suggests retained history / KV growth may be contributing to the pressure.
- The server integration can already translate normal `zoo::Error` values into structured API responses, but it cannot intercept a `GGML_ABORT`.

If useful, I can provide a minimal reproducer on top of `zoo-keeper-server`, but the core problem seems to be that Metal OOM currently escapes the library's normal error-handling model entirely.
```

Notes for posting:

- This is best filed against `zoo-keeper` as a mitigation/graceful-handling issue.
- A second upstream issue against `ggml-org/llama.cpp` may also be warranted because the immediate abort is in `ggml_metal_synchronize()`.
