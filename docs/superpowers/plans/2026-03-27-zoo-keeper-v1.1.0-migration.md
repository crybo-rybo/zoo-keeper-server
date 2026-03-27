# Zoo-Keeper v1.1.0 Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate zoo-keeper-server from zoo-keeper v1.0.1 to v1.1.0, preserving backward-compatible config format and adding per-request sampling overrides.

**Architecture:** The zoo-keeper submodule's single `zoo::Config` splits into `ModelConfig`, `AgentConfig`, and `GenerationOptions`. The server's flat `"zoo"` JSON block is preserved — a custom parser distributes fields to the three types. Per-request sampling overrides flow through a new `merge_request_overrides()` adapter. `RequestHandle<TextResponse>` replaces the old untyped handle. `build_tool_system_prompt()` is removed (native template-driven tool calling).

**Tech Stack:** C++23, CMake, Google Test, nlohmann/json, Drogon, zoo-keeper v1.1.0

**Spec:** `docs/superpowers/specs/2026-03-27-zoo-keeper-v1.1.0-migration-design.md`

**Verified assumptions from v1.1.0 headers:**
- All `validate()` methods return `zoo::Expected<void>` with `zoo::Error` (has `.to_string()`)
- All `from_json` overloads default-initialize missing fields (defaults preserved)
- `Agent::complete()` accepts empty `AsyncTextCallback` via default `= {}`
- `ModelConfig`, `AgentConfig`, `GenerationOptions` all exported from `zoo/core/types.hpp`
- `using Message = OwnedMessage` transitional alias preserved

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `extern/zoo-keeper` | Bump submodule | Pin to v1.1.0 tag |
| `src/server/config.hpp` | Modify | Replace `zoo::Config zoo_config` with 3 config types + `system_prompt` |
| `src/server/config.cpp` | Modify | Custom `from_json` for flat zoo block; update `validate()` |
| `src/server/runtime.cpp:23-26` | Modify | `zoo_config.request_queue_capacity` → `agent_config.request_queue_capacity` |
| `src/server/api_types.hpp:154-159` | Modify | Add sampling fields to `ChatCompletionRequest` |
| `src/server/zoo_adapter.hpp` | Modify | `zoo::Response` → `zoo::TextResponse`; add `merge_request_overrides()` |
| `src/server/zoo_adapter.cpp` | Modify | Implement updated adapter + merge function |
| `src/server/chat_service.hpp:67-118` | Modify | Add `default_generation_` member; update constructor |
| `src/server/chat_service.cpp` | Modify | Agent creation, complete() calls, remove adapt_zoo_future, tool trace opt-in |
| `src/server/api_json.cpp:163-224` | Modify | Parse new sampling fields; add to allowed keys |
| `src/server/api_json.cpp:315-358` | Modify | Map new error codes |
| `src/link_smoke.cpp` | Modify | `zoo::Config{}` → `zoo::ModelConfig{}` |
| `src/live_smoke.cpp` | Modify | Split config, `complete()` + `await_result()` |
| `tests/config_test.cpp` | Modify | Update assertions, add split config + unknown zoo key tests |
| `tests/api_test.cpp` | Modify | Update `UnknownRequestFieldRejected`, add sampling tests, add adapter tests |
| `tests/runtime_test.cpp:32` | Modify | `zoo_config.model_path` → `model_config.model_path` |
| `tests/auth_test.cpp:12` | Modify | `zoo_config.model_path` → `model_config.model_path` |
| `tests/http_integration_test.cpp:64` | Modify | `zoo_config.model_path` → `model_config.model_path` |
| `docs/openapi.yaml` | Modify | Add sampling parameters, error codes |
| `docs/error-reference.md` | Modify | Add new error codes |

---

### Task 1: Bump Submodule to v1.1.0

**Files:**
- Modify: `extern/zoo-keeper` (submodule pointer)

- [ ] **Step 1: Update submodule to v1.1.0 tag**

```bash
cd extern/zoo-keeper
git fetch --all --tags
git checkout v1.1.0
cd ../..
```

- [ ] **Step 2: Verify the tag**

Run: `git -C extern/zoo-keeper describe --tags --exact-match`
Expected: `v1.1.0`

- [ ] **Step 3: Commit submodule bump**

```bash
git add extern/zoo-keeper
git commit -m "chore: bump zoo-keeper submodule to v1.1.0"
```

**Note:** The build will NOT compile after this task. That is expected — the remaining tasks fix all break points. Tasks 2 and 3 must be completed together as one atomic unit before the build compiles again.

---

### Task 2: Core Migration — Config + Chat Service + Runtime + Adapter + Smoke Tests

This is the atomic core migration task. All files that reference `zoo::Config`, `zoo::Response`, `zoo::RequestHandle`, or `build_tool_system_prompt` are updated together so the build stays green after this task completes.

**Files:**
- Modify: `src/server/config.hpp`
- Modify: `src/server/config.cpp`
- Modify: `src/server/runtime.cpp:23-26`
- Modify: `src/server/zoo_adapter.hpp`
- Modify: `src/server/zoo_adapter.cpp`
- Modify: `src/server/chat_service.hpp`
- Modify: `src/server/chat_service.cpp`
- Modify: `src/link_smoke.cpp`
- Modify: `src/live_smoke.cpp`
- Modify: `tests/config_test.cpp`
- Modify: `tests/runtime_test.cpp`
- Modify: `tests/auth_test.cpp`
- Modify: `tests/http_integration_test.cpp`

#### Config Layer

- [ ] **Step 1: Update `config.hpp` — replace `zoo_config` field**

In `src/server/config.hpp`, replace line 59 (`zoo::Config zoo_config;`) with:

```cpp
zoo::ModelConfig model_config;
zoo::AgentConfig agent_config;
zoo::GenerationOptions default_generation;
std::string system_prompt;
```

- [ ] **Step 2: Update `config.cpp` — custom `from_json` for zoo block**

In `src/server/config.cpp`, replace line 253 (`config.zoo_config = j.at("zoo").get<zoo::Config>();`) with:

```cpp
{
    const auto& zoo = j.at("zoo");
    static constexpr std::array<std::string_view, 12> kZooAllowed = {
        "model_path",             "context_size",
        "n_gpu_layers",           "use_mmap",
        "use_mlock",              "max_history_messages",
        "request_queue_capacity", "max_tokens",
        "system_prompt",          "sampling",
        "max_tool_iterations",    "max_tool_retries"};
    check_unknown_keys(zoo, "zoo config", kZooAllowed);

    // Build ModelConfig sub-object and delegate to zoo's from_json
    nlohmann::json model_json = nlohmann::json::object();
    for (const auto& key :
         {"model_path", "context_size", "n_gpu_layers", "use_mmap", "use_mlock"}) {
        if (zoo.contains(key)) {
            model_json[key] = zoo.at(key);
        }
    }
    if (model_json.contains("model_path")) {
        config.model_config = model_json.get<zoo::ModelConfig>();
    }

    // Build AgentConfig sub-object
    nlohmann::json agent_json = nlohmann::json::object();
    for (const auto& key :
         {"max_history_messages", "request_queue_capacity", "max_tool_iterations",
          "max_tool_retries"}) {
        if (zoo.contains(key)) {
            agent_json[key] = zoo.at(key);
        }
    }
    if (!agent_json.empty()) {
        config.agent_config = agent_json.get<zoo::AgentConfig>();
    }

    // Build GenerationOptions sub-object
    nlohmann::json gen_json = nlohmann::json::object();
    for (const auto& key : {"max_tokens", "sampling"}) {
        if (zoo.contains(key)) {
            gen_json[key] = zoo.at(key);
        }
    }
    if (!gen_json.empty()) {
        config.default_generation = gen_json.get<zoo::GenerationOptions>();
    }

    // system_prompt is server-owned (not in any zoo config type)
    if (auto it = zoo.find("system_prompt"); it != zoo.end()) {
        if (!it->is_string()) {
            throw nlohmann::json::type_error::create(
                302, "zoo.system_prompt must be a string", nullptr);
        }
        config.system_prompt = it->get<std::string>();
    }
}
```

- [ ] **Step 3: Update `ServerConfig::validate()` in `config.cpp`**

Replace lines 80-82:
```cpp
// Replace:
//   if (auto validation = zoo_config.validate(); !validation) {
//       return std::unexpected(validation.error().to_string());
//   }
// With:
if (auto v = model_config.validate(); !v) {
    return std::unexpected(v.error().to_string());
}
if (auto v = agent_config.validate(); !v) {
    return std::unexpected(v.error().to_string());
}
if (auto v = default_generation.validate(); !v) {
    return std::unexpected(v.error().to_string());
}
```

#### Runtime

- [ ] **Step 4: Update `runtime.cpp`**

In `src/server/runtime.cpp`, line 25, replace:
```cpp
// config.zoo_config.request_queue_capacity
// With:
config.agent_config.request_queue_capacity
```

#### Zoo Adapter

- [ ] **Step 5: Update `zoo_adapter.hpp`**

Replace `zoo::Response` with `zoo::TextResponse` and add `merge_request_overrides`:

```cpp
#pragma once

#include "server/api_types.hpp"

#include <vector>

#include <zoo/agent.hpp>
#include <zoo/tools/types.hpp>

namespace zks::server {

[[nodiscard]] CompletionResult from_zoo_response(const zoo::TextResponse& response);

[[nodiscard]] ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata);
[[nodiscard]] std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata);

[[nodiscard]] zoo::GenerationOptions
merge_request_overrides(const zoo::GenerationOptions& defaults,
                        const ChatCompletionRequest& request);

} // namespace zks::server
```

- [ ] **Step 6: Update `zoo_adapter.cpp`**

Replace `from_zoo_response` and add `merge_request_overrides`:

```cpp
#include "server/zoo_adapter.hpp"

namespace zks::server {

ToolDefinition from_zoo_tool_metadata(const zoo::tools::ToolMetadata& metadata) {
    return ToolDefinition{metadata.name, metadata.description, metadata.parameters_schema};
}

std::vector<ToolDefinition>
from_zoo_tool_metadata(const std::vector<zoo::tools::ToolMetadata>& metadata) {
    std::vector<ToolDefinition> definitions;
    definitions.reserve(metadata.size());
    for (const auto& entry : metadata) {
        definitions.push_back(from_zoo_tool_metadata(entry));
    }
    return definitions;
}

CompletionResult from_zoo_response(const zoo::TextResponse& response) {
    CompletionResult converted;
    converted.text = response.text;
    converted.usage.prompt_tokens = response.usage.prompt_tokens;
    converted.usage.completion_tokens = response.usage.completion_tokens;
    converted.usage.total_tokens = response.usage.total_tokens;
    converted.metrics.latency_ms = response.metrics.latency_ms;
    converted.metrics.time_to_first_token_ms = response.metrics.time_to_first_token_ms;
    converted.metrics.tokens_per_second = response.metrics.tokens_per_second;
    if (response.tool_trace) {
        converted.tool_invocations = response.tool_trace->invocations;
    }
    return converted;
}

zoo::GenerationOptions merge_request_overrides(const zoo::GenerationOptions& defaults,
                                               const ChatCompletionRequest& request) {
    auto opts = defaults;
    if (request.temperature) opts.sampling.temperature = *request.temperature;
    if (request.top_p) opts.sampling.top_p = *request.top_p;
    if (request.top_k) opts.sampling.top_k = *request.top_k;
    if (request.repeat_penalty) opts.sampling.repeat_penalty = *request.repeat_penalty;
    if (request.max_tokens) opts.max_tokens = *request.max_tokens;
    if (request.seed) opts.sampling.seed = *request.seed;
    if (request.stop) opts.stop_sequences = *request.stop;
    return opts;
}

} // namespace zks::server
```

#### Chat Service

- [ ] **Step 7: Update `chat_service.hpp`**

Add `default_generation_` member and update constructor. In the private section after `agent_`:
```cpp
zoo::GenerationOptions default_generation_;
```

Update constructor signature:
```cpp
ZooChatService(std::string model_id, std::string request_system_prompt,
               std::vector<ToolDefinition> tool_metadata,
               std::shared_ptr<zoo::Agent> shared_agent,
               std::unique_ptr<SessionStore> session_store,
               zoo::GenerationOptions default_generation);
```

- [ ] **Step 8: Rewrite `chat_service.cpp` — remove `adapt_zoo_future`, rewrite `create_configured_agent`, `configure_tools`, `wrap_zoo_handle`**

**Remove `adapt_zoo_future`** (lines 21-36) entirely.

**Rewrite `create_configured_agent`** (lines 69-90):
```cpp
Result<ConfiguredAgent> create_configured_agent(const zoo::ModelConfig& model_config,
                                                const zoo::AgentConfig& agent_config,
                                                const zoo::GenerationOptions& gen_options,
                                                const std::string& system_prompt,
                                                const ToolProvider& tools) {
    auto agent_result = zoo::Agent::create(model_config, agent_config, gen_options);
    if (!agent_result) {
        return std::unexpected("Failed to create zoo::Agent: " + agent_result.error().to_string());
    }

    auto agent = std::move(*agent_result);
    if (!system_prompt.empty()) {
        agent->set_system_prompt(system_prompt);
    }

    ConfiguredAgent configured{
        .agent = std::move(agent),
        .request_system_prompt = system_prompt,
        .tool_definitions = {},
    };

    return configure_tools(std::move(configured), tools);
}
```

**In `configure_tools`**, remove lines 60-64 (the `build_tool_system_prompt` block):
```cpp
// DELETE these lines:
//   if (!configured.tool_definitions.empty()) {
//       configured.request_system_prompt =
//           configured.agent->build_tool_system_prompt(configured.request_system_prompt);
//       configured.agent->set_system_prompt(configured.request_system_prompt);
//   }
```

**Rewrite `wrap_zoo_handle`** (lines 92-95):
```cpp
CompletionHandle wrap_zoo_handle(zoo::RequestHandle<zoo::TextResponse> handle) {
    const auto request_id = static_cast<std::uint64_t>(handle.id());
    auto future =
        std::async(std::launch::async, [h = std::move(handle)]() mutable {
            auto result = h.await_result();
            if (!result) {
                return RuntimeResult<CompletionResult>{std::unexpected(result.error())};
            }
            return RuntimeResult<CompletionResult>{from_zoo_response(*result)};
        });
    return make_completion_handle(request_id, std::move(future));
}
```

- [ ] **Step 9: Update `ZooChatService::create` factory methods**

Update the 2-arg `create` (lines 103-119):
```cpp
Result<std::shared_ptr<ZooChatService>> ZooChatService::create(const ServerConfig& config,
                                                               ToolProvider tools) {
    auto gen_options = config.default_generation;
    if (!tools.tools.empty()) {
        gen_options.record_tool_trace = true;
    }

    auto shared_result = create_configured_agent(config.model_config, config.agent_config,
                                                 gen_options, config.system_prompt, tools);
    if (!shared_result) {
        return std::unexpected(shared_result.error());
    }

    auto shared_agent = std::move(shared_result->agent);
    auto session_store = std::make_unique<SessionStore>(
        config.model_id, config.sessions, shared_result->request_system_prompt,
        config.agent_config.max_history_messages);

    return std::make_shared<ZooChatService>(
        config.model_id, std::move(shared_result->request_system_prompt),
        std::move(shared_result->tool_definitions), std::move(shared_agent),
        std::move(session_store), gen_options);
}
```

The 1-arg `create` overload (line 99-101) delegates to the 2-arg version and needs no changes.

- [ ] **Step 10: Update constructor**

```cpp
ZooChatService::ZooChatService(std::string model_id, std::string request_system_prompt,
                               std::vector<ToolDefinition> tool_metadata,
                               std::shared_ptr<zoo::Agent> shared_agent,
                               std::unique_ptr<SessionStore> session_store,
                               zoo::GenerationOptions default_generation)
    : model_id_(std::move(model_id)), request_system_prompt_(std::move(request_system_prompt)),
      tool_metadata_(std::move(tool_metadata)), agent_(std::move(shared_agent)),
      session_store_(std::move(session_store)),
      default_generation_(std::move(default_generation)) {}
```

- [ ] **Step 11: Update `start_completion` — wire per-request overrides**

Replace line 163 in `start_completion`:
```cpp
// Replace:
//   auto handle = wrap_zoo_handle(agent_->complete(prepare_messages(request), std::move(callback)));
// With:
auto gen_options = merge_request_overrides(default_generation_, request);
auto messages = prepare_messages(request);
zoo::AsyncTextCallback zoo_callback;
if (callback) {
    zoo_callback = [cb = std::move(*callback)](std::string_view token) { cb(token); };
}
auto handle = wrap_zoo_handle(
    agent_->complete(zoo::ConversationView(std::span<const ChatMessage>(messages)),
                     gen_options, std::move(zoo_callback)));
```

- [ ] **Step 12: Update `start_session_completion` — wire per-request overrides**

Replace lines 200-201 in `start_session_completion`:
```cpp
// Replace:
//   auto handle = wrap_zoo_handle(agent_->complete(std::move(begin->messages), std::move(callback)));
// With:
auto gen_options = merge_request_overrides(default_generation_, request);
zoo::AsyncTextCallback zoo_callback;
if (callback) {
    zoo_callback = [cb = std::move(*callback)](std::string_view token) { cb(token); };
}
auto handle = wrap_zoo_handle(
    agent_->complete(zoo::ConversationView(std::span<const ChatMessage>(begin->messages)),
                     gen_options, std::move(zoo_callback)));
```

#### Smoke Tests

- [ ] **Step 13: Update `link_smoke.cpp`**

Replace `zoo::Agent::create(zoo::Config{})` with `zoo::Agent::create(zoo::ModelConfig{})`.

- [ ] **Step 14: Update `live_smoke.cpp`**

Replace config setup and agent call with split config types, `Agent::create(model, agent, gen)`, `set_system_prompt()`, `complete()` with `ConversationView`, and `handle.await_result()`. See spec for full replacement code.

#### Test File Fixups

- [ ] **Step 15: Update test references to `zoo_config`**

In each file, replace `zoo_config.model_path` with `model_config.model_path`:
- `tests/config_test.cpp` lines 65, 66, 300, 312, 322:
  - `config->zoo_config.model_path` → `config->model_config.model_path`
  - `config->zoo_config.max_tokens` → `config->default_generation.max_tokens`
  - `config.zoo_config.model_path` → `config.model_config.model_path`
- `tests/runtime_test.cpp:32`
- `tests/auth_test.cpp:12`
- `tests/http_integration_test.cpp:64`

#### Verify

- [ ] **Step 16: Build**

Run: `scripts/build`
Expected: Clean compilation

- [ ] **Step 17: Run all tests**

Run: `scripts/test`
Expected: All existing tests PASS

- [ ] **Step 18: Commit**

```bash
git add src/server/config.hpp src/server/config.cpp src/server/runtime.cpp \
  src/server/zoo_adapter.hpp src/server/zoo_adapter.cpp \
  src/server/chat_service.hpp src/server/chat_service.cpp \
  src/link_smoke.cpp src/live_smoke.cpp \
  tests/config_test.cpp tests/runtime_test.cpp tests/auth_test.cpp tests/http_integration_test.cpp
git commit -m "feat: migrate to zoo-keeper v1.1.0 split config, typed handles, and TextResponse"
```

---

### Task 3: Add Config Split Tests + Unknown Zoo Key Regression

**Files:**
- Modify: `tests/config_test.cpp`

- [ ] **Step 1: Write test — flat zoo block populates split config types**

Add to `tests/config_test.cpp`:
```cpp
TEST(ConfigTest, ZooBlockPopulatesSplitConfigTypes) {
    TempDir tmp;
    auto file = tmp.path / "split.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": {
    "model_path": "/tmp/demo.gguf",
    "context_size": 4096,
    "n_gpu_layers": 8,
    "use_mmap": false,
    "max_history_messages": 32,
    "request_queue_capacity": 16,
    "max_tokens": 64,
    "system_prompt": "Be helpful.",
    "sampling": { "temperature": 0.5 }
  }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->model_config.model_path, "/tmp/demo.gguf");
    EXPECT_EQ(config->model_config.context_size, 4096);
    EXPECT_EQ(config->model_config.n_gpu_layers, 8);
    EXPECT_FALSE(config->model_config.use_mmap);
    EXPECT_EQ(config->agent_config.max_history_messages, 32u);
    EXPECT_EQ(config->agent_config.request_queue_capacity, 16u);
    EXPECT_EQ(config->default_generation.max_tokens, 64);
    EXPECT_FLOAT_EQ(config->default_generation.sampling.temperature, 0.5f);
    EXPECT_EQ(config->system_prompt, "Be helpful.");
}
```

- [ ] **Step 2: Write test — unknown zoo key rejected (regression)**

```cpp
TEST(ConfigTest, UnknownZooKeyRejected) {
    TempDir tmp;
    auto file = tmp.path / "bad_zoo.json";
    ASSERT_TRUE(write_text_file(file,
                                R"json({
  "model_id": "demo-model",
  "zoo": {
    "model_path": "/tmp/demo.gguf",
    "bogus_key": true
  }
})json"));

    auto config = zks::server::load_config(file);
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().find("Unknown"), std::string::npos);
    EXPECT_NE(config.error().find("bogus_key"), std::string::npos);
}
```

- [ ] **Step 3: Run tests**

Run: `scripts/test -R config`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add tests/config_test.cpp
git commit -m "test: add split config parsing and unknown zoo key regression tests"
```

---

### Task 4: Per-Request Sampling — Request Type + Parsing + Adapter Tests

**Files:**
- Modify: `src/server/api_types.hpp:154-159`
- Modify: `src/server/api_json.cpp:163-224, 315-358`
- Modify: `tests/api_test.cpp`

- [ ] **Step 1: Write failing tests**

Add to `tests/api_test.cpp`:
```cpp
#include "server/zoo_adapter.hpp"

TEST(ApiTest, ParseRequestWithSamplingOverrides) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"m","messages":[{"role":"user","content":"Hi"}],"temperature":0.3,"top_p":0.8,"max_tokens":100,"seed":42,"stop":["END"]})json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->temperature, std::optional<float>{0.3f});
    EXPECT_EQ(parsed->top_p, std::optional<float>{0.8f});
    EXPECT_EQ(parsed->max_tokens, std::optional<int>{100});
    EXPECT_EQ(parsed->seed, std::optional<int>{42});
    ASSERT_TRUE(parsed->stop.has_value());
    EXPECT_EQ(parsed->stop->size(), 1u);
    EXPECT_EQ(parsed->stop->at(0), "END");
}

TEST(ApiTest, ParseRequestWithoutSamplingOverrides) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"m","messages":[{"role":"user","content":"Hi"}]})json");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->temperature.has_value());
    EXPECT_FALSE(parsed->top_p.has_value());
    EXPECT_FALSE(parsed->max_tokens.has_value());
    EXPECT_FALSE(parsed->seed.has_value());
    EXPECT_FALSE(parsed->stop.has_value());
}

TEST(ZooAdapterTest, MergeRequestOverridesPassthrough) {
    zoo::GenerationOptions defaults;
    defaults.sampling.temperature = 0.7f;
    defaults.max_tokens = 100;

    zks::server::ChatCompletionRequest request;
    request.model = "m";

    auto result = zks::server::merge_request_overrides(defaults, request);
    EXPECT_FLOAT_EQ(result.sampling.temperature, 0.7f);
    EXPECT_EQ(result.max_tokens, 100);
}

TEST(ZooAdapterTest, MergeRequestOverridesPartial) {
    zoo::GenerationOptions defaults;
    defaults.sampling.temperature = 0.7f;
    defaults.sampling.top_p = 0.9f;
    defaults.max_tokens = 100;

    zks::server::ChatCompletionRequest request;
    request.model = "m";
    request.temperature = 0.2f;
    request.max_tokens = 50;

    auto result = zks::server::merge_request_overrides(defaults, request);
    EXPECT_FLOAT_EQ(result.sampling.temperature, 0.2f);
    EXPECT_FLOAT_EQ(result.sampling.top_p, 0.9f);
    EXPECT_EQ(result.max_tokens, 50);
}

TEST(ZooAdapterTest, MergeRequestOverridesStopSequences) {
    zoo::GenerationOptions defaults;

    zks::server::ChatCompletionRequest request;
    request.model = "m";
    request.stop = std::vector<std::string>{"END", "STOP"};

    auto result = zks::server::merge_request_overrides(defaults, request);
    ASSERT_EQ(result.stop_sequences.size(), 2u);
    EXPECT_EQ(result.stop_sequences[0], "END");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `scripts/test -R "api|ZooAdapter"`
Expected: Compilation failure (sampling fields don't exist on `ChatCompletionRequest` yet)

- [ ] **Step 3: Add sampling fields to `ChatCompletionRequest`**

In `src/server/api_types.hpp`, update the struct:
```cpp
struct ChatCompletionRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    bool stream = false;
    std::optional<std::string> session_id;
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int> top_k;
    std::optional<float> repeat_penalty;
    std::optional<int> max_tokens;
    std::optional<int> seed;
    std::optional<std::vector<std::string>> stop;
};
```

- [ ] **Step 4: Update `UnknownRequestFieldRejected` test**

Replace the test at line 31-37 of `tests/api_test.cpp`:
```cpp
TEST(ApiTest, UnknownRequestFieldRejected) {
    auto parsed = zks::server::parse_chat_completion_request(
        R"json({"model":"local-model","messages":[{"role":"user","content":"Hello"}],"bogus_field":true})json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().http_status, 400);
    EXPECT_EQ(parsed.error().code, std::optional<std::string>{"unknown_field"});
}
```

- [ ] **Step 5: Update allowed keys + add parsing logic in `api_json.cpp`**

In `src/server/api_json.cpp`, replace the allowed keys (lines 172-173):
```cpp
static constexpr std::array<const char*, 11> kAllowedKeys = {
    "model",          "messages", "stream",   "session_id", "temperature",
    "top_p",          "top_k",    "repeat_penalty", "max_tokens", "seed", "stop"};
```

After the `session_id` parsing block (after line 207), add:
```cpp
if (auto it = json.find("temperature"); it != json.end()) {
    if (!it->is_number()) {
        return std::unexpected(
            invalid_request_error("temperature must be a number", "temperature"));
    }
    request.temperature = it->get<float>();
}
if (auto it = json.find("top_p"); it != json.end()) {
    if (!it->is_number()) {
        return std::unexpected(invalid_request_error("top_p must be a number", "top_p"));
    }
    request.top_p = it->get<float>();
}
if (auto it = json.find("top_k"); it != json.end()) {
    if (!it->is_number_integer()) {
        return std::unexpected(invalid_request_error("top_k must be an integer", "top_k"));
    }
    request.top_k = it->get<int>();
}
if (auto it = json.find("repeat_penalty"); it != json.end()) {
    if (!it->is_number()) {
        return std::unexpected(
            invalid_request_error("repeat_penalty must be a number", "repeat_penalty"));
    }
    request.repeat_penalty = it->get<float>();
}
if (auto it = json.find("max_tokens"); it != json.end()) {
    if (!it->is_number_integer()) {
        return std::unexpected(
            invalid_request_error("max_tokens must be an integer", "max_tokens"));
    }
    request.max_tokens = it->get<int>();
}
if (auto it = json.find("seed"); it != json.end()) {
    if (!it->is_number_integer()) {
        return std::unexpected(invalid_request_error("seed must be an integer", "seed"));
    }
    request.seed = it->get<int>();
}
if (auto it = json.find("stop"); it != json.end()) {
    if (!it->is_array()) {
        return std::unexpected(invalid_request_error("stop must be an array", "stop"));
    }
    std::vector<std::string> stop_seqs;
    for (const auto& item : *it) {
        if (!item.is_string()) {
            return std::unexpected(
                invalid_request_error("stop entries must be strings", "stop"));
        }
        stop_seqs.push_back(item.get<std::string>());
    }
    request.stop = std::move(stop_seqs);
}
```

- [ ] **Step 6: Add new error codes to `map_runtime_error_to_api_error`**

In `src/server/api_json.cpp`, before the `case RuntimeErrorCode::Unknown:` line, add:
```cpp
case RuntimeErrorCode::InvalidOutputSchema:
    return invalid_request_error(error.message, std::nullopt, "invalid_output_schema");
case RuntimeErrorCode::ExtractionFailed:
    return server_error(error.message, "extraction_failed");
```

- [ ] **Step 7: Run tests**

Run: `scripts/test -R "api|ZooAdapter"`
Expected: All PASS

- [ ] **Step 8: Commit**

```bash
git add src/server/api_types.hpp src/server/api_json.cpp tests/api_test.cpp
git commit -m "feat: add per-request sampling overrides and new error code mappings"
```

---

### Task 5: Documentation Updates

**Files:**
- Modify: `docs/openapi.yaml`
- Modify: `docs/error-reference.md`

- [ ] **Step 1: Add sampling parameters to OpenAPI spec**

In `docs/openapi.yaml`, in the `ChatCompletionRequest` schema properties, add:
```yaml
temperature:
  type: number
  description: Sampling temperature override. 0.0 = greedy.
top_p:
  type: number
  description: Nucleus sampling cutoff [0.0, 1.0].
top_k:
  type: integer
  description: Top-K sampling limit.
repeat_penalty:
  type: number
  description: Repetition penalty factor.
max_tokens:
  type: integer
  description: Maximum tokens to generate.
seed:
  type: integer
  description: RNG seed for reproducibility.
stop:
  type: array
  items:
    type: string
  description: Custom stop sequences.
```

- [ ] **Step 2: Add new error codes to error-reference.md**

Add entries for `invalid_output_schema` (400) and `extraction_failed` (500).

- [ ] **Step 3: Commit**

```bash
git add docs/openapi.yaml docs/error-reference.md
git commit -m "docs: add per-request sampling parameters and new error codes"
```

---

### Task 6: Final Verification

- [ ] **Step 1: Format check**

Run: `scripts/format-check`
Expected: Clean. If violations, run `scripts/format` and commit.

- [ ] **Step 2: Full build**

Run: `scripts/build`
Expected: Clean compilation

- [ ] **Step 3: Full test suite**

Run: `scripts/test`
Expected: All tests PASS

- [ ] **Step 4: Verify backward compatibility**

Run: `./build/zoo_keeper_server config/server.example.json 2>&1 | head -5`
Expected: Either starts or fails with a model path error — not a config parse error.

- [ ] **Step 5: Commit format fixes (if any)**

```bash
git add -A
git commit -m "style: format fixes for v1.1.0 migration"
```
