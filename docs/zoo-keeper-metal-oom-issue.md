# macOS Metal OOM Issue

## Summary

On macOS with Metal acceleration enabled, a device out-of-memory condition
during inference can abort the process before `zoo-keeper` is able to surface a
recoverable runtime error to the server layer.

This is an upstream `llama.cpp` backend limitation rather than a
`zoo-keeper-server` policy choice.

## Who Is Affected

This is most relevant when all of these are true:

- the server is running on macOS
- Metal support is enabled
- the model or prompt budget is large enough to pressure GPU memory
- sessions retain enough history to make requests materially heavier over time

## What It Looks Like

- the server process terminates during or shortly after an inference request
- the request does not return a structured API error
- restarting the process is required

## Mitigations

Reduce memory pressure using one or more of these:

- lower `zoo.context_size`
- lower `zoo.n_gpu_layers`
- use a smaller GGUF model or quantization
- disable sessions by setting `sessions.max_sessions` to `0`
- run CPU-only if stability matters more than Metal acceleration for your use
  case

## Release Guidance

For `v0.0.1`, treat Metal on macOS as a performance option with a known failure
mode, not as a fully hardened production path.

If you are validating the server for heavier long-lived session workloads on
macOS, prefer conservative memory settings first.
