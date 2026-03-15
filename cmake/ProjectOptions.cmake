include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZKS_DROGON_VERSION "v1.9.12" CACHE STRING
    "Pinned Drogon version for the embedded HTTP server build")
option(ZKS_ENABLE_TEST_UI "Enable the test-only browser UI at /_test" OFF)
set(ZKS_LIVE_SMOKE_MODEL "" CACHE FILEPATH
    "Path to a GGUF model used by the optional live smoke test")
set(ZKS_LIVE_SMOKE_PROMPT "Reply with the single word: zoo." CACHE STRING
    "Prompt used by the optional live smoke test")
