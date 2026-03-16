include_guard(GLOBAL)

set(ZKS_SUBMODULE_HINT "Run: git submodule update --init --recursive")
set(ZKS_ZOO_KEEPER_DIR "${PROJECT_SOURCE_DIR}/extern/zoo-keeper")
set(ZKS_LLAMA_DIR "${ZKS_ZOO_KEEPER_DIR}/extern/llama.cpp")

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

set(_zks_top_level_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "Disable subproject tests in this repo" FORCE)

FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG ${ZKS_DROGON_VERSION}
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(drogon)

set(BUILD_TESTING "${_zks_top_level_build_testing}" CACHE BOOL "Enable top-level tests" FORCE)
unset(_zks_top_level_build_testing)

if(TARGET drogon AND NOT TARGET Drogon::Drogon)
    add_library(Drogon::Drogon ALIAS drogon)
endif()
