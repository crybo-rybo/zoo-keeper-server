  # zoo-keeper-server v1

  ## Summary

  - Create zoo-keeper-server as a separate companion repo using CMake, Drogon, nlohmann/
    json, CTest, and zoo-keeper as extern/zoo-keeper.
  - Expose a small OpenAI-like HTTP API over JSON, with SSE when stream=true.
  - Keep v1 intentionally narrow: single shared model/runtime, stateless requests, built-
    in in-process tools only, no auth, no persistence.
  - The zoo-keeper project this is based around can be found at the following location: https://github.com/crybo-rybo/zoo-keeper.git
  - The zoo-keeper library should be included as a git submodule within this project. Include it in an extern/ folder.
  - Any changes to the zoo-keeper code should not be directly made within this project, instead, github "issues" should be written against the zoo-keeper project.

  ## Key Changes

  - Upstream dependency:
      - Add one minimal public zoo-keeper request-scoped API that accepts a full
        std::vector<zoo::Message> plus an optional token callback and executes without
        mutating agent-global history.
      - Keep existing zoo::Agent::chat(Message) behavior unchanged; the new server depends
        on the new request-scoped entrypoint instead of reimplementing the tool loop.
  - Server runtime:
      - Bootstrap one shared zoo::Agent at process start from server config; fail fast if
        model load or built-in tool registration fails.
      - Register built-in tools in C++ during startup with Agent::register_tool(...).
      - Propagate client disconnect on streaming requests to Agent::cancel(request_id) for
        best-effort cancellation.
  - HTTP surface:
      - GET /healthz: readiness once the shared agent is live.
      - GET /v1/models: return the single configured model id and minimal metadata.
      - GET /v1/tools: return server-owned tool metadata and schemas for discovery.
      - POST /v1/chat/completions: accept model, messages, and stream; map request
        messages to zoo::Message, execute one request-scoped completion, and return
        OpenAI-like JSON.
      - stream=true uses SSE token deltas plus a terminal [DONE]; WebSocket is deferred.
      - Reject unsupported request features with 400: remote tool registration, client-
        supplied executable tools, server sessions, per-request runtime mutation, and
        multi-model routing beyond the single configured model id.
  - Internal modules:
      - Config/bootstrap module for bind address, port, advertised model id, and embedded
        zoo::Config.
      - API adapter module for OpenAI-like JSON to/from zoo types and stable error
        envelopes.
      - Chat service module that owns the shared agent and hides request execution details
        from controllers.
      - Streaming module for SSE framing and cancellation propagation.

  ## Test Plan

  - Unit-test JSON adapters: role mapping, response shaping, error mapping, and rejection
    of unsupported fields.
  - Unit-test SSE framing: token chunks, terminal event, and [DONE].
  - Integration-test startup and readiness with a configured model path.
  - Integration-test non-stream chat, streamed chat, invalid model id, unsupported remote
    tools, queue-full handling, and cancel-on-disconnect behavior.
  - Add an optional live smoke suite that runs only when a GGUF model path is supplied,
    matching the zoo-keeper integration pattern.

  ## Assumptions

  - zoo-keeper-server remains a separate repo and consumes zoo-keeper through a submodule
    first.
  - v1 is stateless: each chat request carries the full messages[]; the server stores no
    conversations.
  - v1 uses one shared agent/model for the whole process.
  - Tools are server-owned and executable only when registered in-process at startup.
  - v1 defers auth, persistence, embeddings, remote tool callbacks, per-request sampling
    overrides, multi-model routing, and WebSocket transport.
