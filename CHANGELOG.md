# Changelog

## v0.0.4 - 2026-05-08

- Updated the vendored `zoo-keeper` submodule and adapted the server build to
  zoo-keeper's FetchContent-based llama.cpp layout.
- Added optional GGUF auto-configuration through `zoo.auto_configure_model` when
  `ZKS_ENABLE_ZOO_HUB` is enabled.
- Expanded API compatibility for newer zoo-keeper behavior, including
  mid-conversation system messages, assistant `tool_calls`, retained-agent
  history responses with tool calls, and explicit hub error mapping.
- Documented the current API surface, runtime snapshot fields, config templates,
  and release validation commands.
- Kept the v0.0.4 release gate green with the default CTest suite and
  formatting checks.
