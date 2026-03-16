cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
endif()

project(zoo-keeper-server VERSION 0.0.4 LANGUAGES CXX)

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

include(CTest)
include(FetchContent)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Dependencies)

add_subdirectory(extern/zoo-keeper)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zoo-keeper-server/config
    FILES_MATCHING PATTERN "*.json")
