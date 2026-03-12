cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
endif()

project(zoo-keeper-server LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZKS_SUBMODULE_HINT "Run: git submodule update --init --recursive")
set(ZKS_ZOO_KEEPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/zoo-keeper")
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

add_subdirectory(extern/zoo-keeper)

add_executable(zoo_keeper_server
    src/main.cpp
)
target_compile_features(zoo_keeper_server PRIVATE cxx_std_23)
target_link_libraries(zoo_keeper_server PRIVATE ZooKeeper::zoo)

if(BUILD_TESTING)
    add_test(NAME baseline_link_smoke COMMAND zoo_keeper_server)
    set_tests_properties(baseline_link_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "Observed expected InvalidModelPath validation failure."
    )
endif()
