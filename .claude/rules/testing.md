---
description: Rules for writing and registering tests
globs: ["tests/**/*.cpp", "tests/CMakeLists.txt"]
---

# Testing

## Test registration
Use the `zks_add_test()` CMake function in `tests/CMakeLists.txt`:
```cmake
zks_add_test(my_feature_test SOURCES my_feature_test.cpp)
```
For tests needing their own `main()`, add `OWN_MAIN`. For extra link libraries, add `LIBS`.

## File naming
Test files: `tests/<name>_test.cpp`. The name should match the module being tested.

## Framework
Google Test v1.15.2 (fetched by CMake). Use `#include <gtest/gtest.h>`.

## FakeChatService
`tests/fake_chat_service.hpp` is the canonical test double for `ChatService`. Usage:
1. Construct a `FakeChatService`
2. Call `set_mode(FakeCompletionMode::Success)` (or other modes: `ServerError`, `QueueFull`, `Cancelled`, `StreamingSuccess`)
3. Call `set_response(...)` to set the completion result
4. Pass to the component under test

Available modes:
- `Success` — immediate successful response
- `ServerError` — returns error from `start_completion()`
- `QueueFull` — returns 503 queue-full error
- `Cancelled` — future resolves to `RequestCancelled`
- `StreamingSuccess` — streams tokens then blocks on `finish_latch()`

## Test fixtures
Static test data lives in `tests/fixtures/` (e.g., config JSON files). Never use real model files in tests.

## Running tests
```bash
scripts/test              # build + run all
scripts/test -R config    # build + run matching tests
```
