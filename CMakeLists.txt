cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
endif()

project(zoo-keeper-server VERSION 0.0.1 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZKS_SUBMODULE_HINT "Run: git submodule update --init --recursive")
set(ZKS_ZOO_KEEPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/zoo-keeper")
set(ZKS_LLAMA_DIR "${ZKS_ZOO_KEEPER_DIR}/extern/llama.cpp")
set(ZKS_DROGON_VERSION "v1.9.12" CACHE STRING
    "Pinned Drogon version for the embedded HTTP server build")
option(ZKS_ENABLE_TEST_UI "Enable the test-only browser UI at /_test" OFF)
set(ZKS_LIVE_SMOKE_MODEL "" CACHE FILEPATH
    "Path to a GGUF model used by the optional live smoke test")
set(ZKS_LIVE_SMOKE_PROMPT "Reply with the single word: zoo." CACHE STRING
    "Prompt used by the optional live smoke test")

if(NOT EXISTS "${ZKS_ZOO_KEEPER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing zoo-keeper submodule at extern/zoo-keeper. ${ZKS_SUBMODULE_HINT}"
    )
endif()

if(NOT EXISTS "${ZKS_LLAMA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing nested llama.cpp submodule under extern/zoo-keeper. ${ZKS_SUBMODULE_HINT}"
    )
endif()

set(ZOO_BUILD_TESTS OFF CACHE BOOL "Disable upstream zoo-keeper tests in this repo" FORCE)
set(ZOO_BUILD_INTEGRATION_TESTS OFF CACHE BOOL
    "Disable upstream zoo-keeper integration tests in this repo" FORCE)
set(ZOO_BUILD_EXAMPLES OFF CACHE BOOL "Disable upstream zoo-keeper examples in this repo" FORCE)
set(ZOO_BUILD_DOCS OFF CACHE BOOL "Disable upstream zoo-keeper docs in this repo" FORCE)
set(ZOO_ENABLE_COVERAGE OFF CACHE BOOL "Disable upstream zoo-keeper coverage in this repo" FORCE)
set(ZOO_ENABLE_SANITIZERS OFF CACHE BOOL
    "Disable upstream zoo-keeper sanitizers in this repo" FORCE)
set(ZOO_ENABLE_LOGGING OFF CACHE BOOL "Disable upstream zoo-keeper logging in this repo" FORCE)

if(NOT DEFINED ZOO_ENABLE_METAL)
    if(APPLE)
        set(ZOO_ENABLE_METAL ON CACHE BOOL
            "Enable Metal acceleration for zoo-keeper on Apple hosts" FORCE)
    else()
        set(ZOO_ENABLE_METAL OFF CACHE BOOL
            "Enable Metal acceleration for zoo-keeper on Apple hosts" FORCE)
    endif()
endif()

if(NOT DEFINED ZOO_ENABLE_CUDA)
    set(ZOO_ENABLE_CUDA OFF CACHE BOOL "Enable CUDA acceleration for zoo-keeper" FORCE)
endif()

set(BUILD_CTL OFF CACHE BOOL "Disable Drogon CLI tooling in this repo" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable Drogon examples in this repo" FORCE)
set(BUILD_ORM OFF CACHE BOOL "Disable Drogon ORM in this repo" FORCE)
set(BUILD_DOC OFF CACHE BOOL "Disable Drogon docs in this repo" FORCE)
set(BUILD_BROTLI OFF CACHE BOOL "Disable Brotli support in this repo" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "Disable YAML config support in this repo" FORCE)
set(ZKS_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF CACHE BOOL "Disable subproject tests in this repo" FORCE)
FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG ${ZKS_DROGON_VERSION}
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(drogon)
set(BUILD_TESTING ${ZKS_BUILD_TESTING} CACHE BOOL "Enable top-level tests" FORCE)
if(TARGET drogon AND NOT TARGET Drogon::Drogon)
    add_library(Drogon::Drogon ALIAS drogon)
endif()

add_subdirectory(extern/zoo-keeper)

add_library(zks_server_core
    src/server/api_json.cpp
    src/server/api_routes.cpp
    src/server/chat_service.cpp
    src/server/config.cpp
    src/server/runtime.cpp
    src/server/session_manager.cpp
    src/server/streaming.cpp
    src/server/health.cpp
)
if(ZKS_ENABLE_TEST_UI)
    target_sources(zks_server_core PRIVATE
        src/server/test_ui.cpp
    )
    target_compile_definitions(zks_server_core PUBLIC ZKS_ENABLE_TEST_UI=1)
endif()
target_compile_features(zks_server_core PUBLIC cxx_std_23)
target_include_directories(zks_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(zks_server_core
    PUBLIC
        Drogon::Drogon
        ZooKeeper::zoo
)

add_executable(zoo_keeper_server
    src/main.cpp
)
target_compile_features(zoo_keeper_server PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_server PRIVATE zks_server_core)

add_executable(zoo_keeper_link_smoke
    src/link_smoke.cpp
)
target_compile_features(zoo_keeper_link_smoke PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_link_smoke PRIVATE ZooKeeper::zoo)

add_executable(zoo_keeper_live_smoke
    src/live_smoke.cpp
)
target_compile_features(zoo_keeper_live_smoke PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_live_smoke PRIVATE ZooKeeper::zoo)

if(BUILD_TESTING)
    add_executable(zks_config_test
        tests/config_test.cpp
    )
    target_compile_features(zks_config_test PRIVATE cxx_std_23)
    target_link_libraries(zks_config_test PRIVATE zks_server_core)

    add_executable(zks_health_test
        tests/health_test.cpp
    )
    target_compile_features(zks_health_test PRIVATE cxx_std_23)
    target_link_libraries(zks_health_test PRIVATE zks_server_core)

    add_executable(zks_api_test
        tests/api_test.cpp
    )
    target_compile_features(zks_api_test PRIVATE cxx_std_23)
    target_link_libraries(zks_api_test PRIVATE zks_server_core)

    add_executable(zks_runtime_test
        tests/runtime_test.cpp
    )
    target_compile_features(zks_runtime_test PRIVATE cxx_std_23)
    target_link_libraries(zks_runtime_test PRIVATE zks_server_core)

    add_executable(zks_session_manager_test
        tests/session_manager_test.cpp
    )
    target_compile_features(zks_session_manager_test PRIVATE cxx_std_23)
    target_link_libraries(zks_session_manager_test PRIVATE zks_server_core)

    add_executable(zks_auth_test
        tests/auth_test.cpp
    )
    target_compile_features(zks_auth_test PRIVATE cxx_std_23)
    target_link_libraries(zks_auth_test PRIVATE zks_server_core)

    add_executable(zks_metrics_test
        tests/metrics_test.cpp
    )
    target_compile_features(zks_metrics_test PRIVATE cxx_std_23)
    target_link_libraries(zks_metrics_test PRIVATE zks_server_core)

    add_executable(zks_http_integration_test
        tests/http_integration_test.cpp
    )
    target_compile_features(zks_http_integration_test PRIVATE cxx_std_23)
    target_link_libraries(zks_http_integration_test PRIVATE zks_server_core)

    if(ZKS_ENABLE_TEST_UI)
        add_executable(zks_test_ui_test
            tests/test_ui_test.cpp
        )
        target_compile_features(zks_test_ui_test PRIVATE cxx_std_23)
        target_link_libraries(zks_test_ui_test PRIVATE zks_server_core)
    endif()

    add_test(NAME baseline_link_smoke COMMAND zoo_keeper_link_smoke)
    set_tests_properties(baseline_link_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "Observed expected InvalidModelPath validation failure."
    )

    add_test(NAME config_parse_test COMMAND zks_config_test)
    add_test(NAME health_response_test COMMAND zks_health_test)
    add_test(NAME api_response_test COMMAND zks_api_test)
    add_test(NAME runtime_stop_test COMMAND zks_runtime_test)
    add_test(NAME session_manager_test COMMAND zks_session_manager_test)
    add_test(NAME auth_test COMMAND zks_auth_test)
    add_test(NAME metrics_test COMMAND zks_metrics_test)
    add_test(NAME http_integration_test COMMAND zks_http_integration_test)
    set_tests_properties(http_integration_test PROPERTIES TIMEOUT 30)
    if(ZKS_ENABLE_TEST_UI)
        add_test(NAME test_ui_response_test COMMAND zks_test_ui_test)
    endif()
    add_test(
        NAME server_startup_missing_model
        COMMAND ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:zoo_keeper_server>
            -DTEST_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/server.invalid.missing_model.json
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_startup_missing_model.cmake
    )

    if(ZKS_LIVE_SMOKE_MODEL)
        if(NOT EXISTS "${ZKS_LIVE_SMOKE_MODEL}")
            message(FATAL_ERROR
                "ZKS_LIVE_SMOKE_MODEL does not exist: ${ZKS_LIVE_SMOKE_MODEL}"
            )
        endif()

        add_test(
            NAME live_model_smoke
            COMMAND zoo_keeper_live_smoke
                "${ZKS_LIVE_SMOKE_MODEL}"
                "${ZKS_LIVE_SMOKE_PROMPT}"
        )
        set_tests_properties(live_model_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "Live smoke succeeded."
            TIMEOUT 300
        )
    endif()
endif()
