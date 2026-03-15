include_guard(GLOBAL)

function(zks_force_cache_bool variable value description)
    set(${variable} ${value} CACHE BOOL "${description}" FORCE)
endfunction()

function(zks_require_submodule directory error_message)
    if(NOT EXISTS "${directory}/CMakeLists.txt")
        message(FATAL_ERROR "${error_message}. ${ZKS_SUBMODULE_HINT}")
    endif()
endfunction()

set(ZKS_SUBMODULE_HINT "Run: git submodule update --init --recursive")
set(ZKS_ZOO_KEEPER_DIR "${PROJECT_SOURCE_DIR}/extern/zoo-keeper")
set(ZKS_LLAMA_DIR "${ZKS_ZOO_KEEPER_DIR}/extern/llama.cpp")

zks_require_submodule(
    "${ZKS_ZOO_KEEPER_DIR}"
    "Missing zoo-keeper submodule at extern/zoo-keeper"
)
zks_require_submodule("${ZKS_LLAMA_DIR}"
    "Missing nested llama.cpp submodule under extern/zoo-keeper")

zks_force_cache_bool(ZOO_BUILD_TESTS OFF "Disable upstream zoo-keeper tests in this repo")
zks_force_cache_bool(ZOO_BUILD_INTEGRATION_TESTS OFF
    "Disable upstream zoo-keeper integration tests in this repo")
zks_force_cache_bool(ZOO_BUILD_EXAMPLES OFF "Disable upstream zoo-keeper examples in this repo")
zks_force_cache_bool(ZOO_BUILD_DOCS OFF "Disable upstream zoo-keeper docs in this repo")
zks_force_cache_bool(ZOO_ENABLE_COVERAGE OFF "Disable upstream zoo-keeper coverage in this repo")
zks_force_cache_bool(ZOO_ENABLE_SANITIZERS OFF
    "Disable upstream zoo-keeper sanitizers in this repo")
zks_force_cache_bool(ZOO_ENABLE_LOGGING OFF "Disable upstream zoo-keeper logging in this repo")

if(NOT DEFINED ZOO_ENABLE_METAL)
    if(APPLE)
        zks_force_cache_bool(ZOO_ENABLE_METAL ON
            "Enable Metal acceleration for zoo-keeper on Apple hosts")
    else()
        zks_force_cache_bool(ZOO_ENABLE_METAL OFF
            "Enable Metal acceleration for zoo-keeper on Apple hosts")
    endif()
endif()

if(NOT DEFINED ZOO_ENABLE_CUDA)
    zks_force_cache_bool(ZOO_ENABLE_CUDA OFF "Enable CUDA acceleration for zoo-keeper")
endif()

zks_force_cache_bool(BUILD_CTL OFF "Disable Drogon CLI tooling in this repo")
zks_force_cache_bool(BUILD_EXAMPLES OFF "Disable Drogon examples in this repo")
zks_force_cache_bool(BUILD_ORM OFF "Disable Drogon ORM in this repo")
zks_force_cache_bool(BUILD_DOC OFF "Disable Drogon docs in this repo")
zks_force_cache_bool(BUILD_BROTLI OFF "Disable Brotli support in this repo")
zks_force_cache_bool(BUILD_YAML_CONFIG OFF "Disable YAML config support in this repo")

set(_zks_top_level_build_testing "${BUILD_TESTING}")
zks_force_cache_bool(BUILD_TESTING OFF "Disable subproject tests in this repo")

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
