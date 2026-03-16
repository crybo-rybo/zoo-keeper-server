set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

function(zks_add_test test_name)
    cmake_parse_arguments(ARG "OWN_MAIN" "" "SOURCES;LIBS;ARGS" ${ARGN})
    add_executable(zks_${test_name} ${ARG_SOURCES})
    if(ARG_OWN_MAIN)
        target_link_libraries(zks_${test_name} PRIVATE
            zks_server_core GTest::gtest ${ARG_LIBS})
    else()
        target_link_libraries(zks_${test_name} PRIVATE
            zks_server_core GTest::gtest_main ${ARG_LIBS})
    endif()
    add_test(NAME ${test_name} COMMAND zks_${test_name} ${ARG_ARGS})
endfunction()

add_executable(zks_tool_test_helper tool_test_helper.cpp)
target_link_libraries(zks_tool_test_helper PRIVATE zks_server_core)

zks_add_test(config_parse_test SOURCES config_test.cpp)
zks_add_test(health_response_test SOURCES health_test.cpp)
zks_add_test(api_response_test SOURCES api_test.cpp)
zks_add_test(runtime_stop_test SOURCES runtime_test.cpp)
zks_add_test(session_manager_test SOURCES session_manager_test.cpp)
zks_add_test(auth_test SOURCES auth_test.cpp)
zks_add_test(metrics_test SOURCES metrics_test.cpp)
zks_add_test(http_integration_test SOURCES http_integration_test.cpp)
set_tests_properties(http_integration_test PROPERTIES TIMEOUT 30)
zks_add_test(streaming_test SOURCES streaming_test.cpp)
zks_add_test(executor_test SOURCES executor_test.cpp)
zks_add_test(zoo_adapter_test SOURCES zoo_adapter_test.cpp)
zks_add_test(command_tools_test
    OWN_MAIN
    SOURCES command_tools_test.cpp
    ARGS $<TARGET_FILE:zks_tool_test_helper>
)

if(ZKS_ENABLE_TEST_UI)
    zks_add_test(test_ui_response_test SOURCES test_ui_test.cpp)
endif()

add_test(NAME baseline_link_smoke COMMAND zoo_keeper_link_smoke)
set_tests_properties(baseline_link_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Observed expected InvalidModelPath validation failure."
)

add_test(
    NAME server_startup_missing_model
    COMMAND ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:zoo_keeper_server>
        -DTEST_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/server.invalid.missing_model.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/server_startup_missing_model.cmake
)

if(ZKS_LIVE_SMOKE_MODEL)
    if(NOT EXISTS "${ZKS_LIVE_SMOKE_MODEL}")
        message(FATAL_ERROR "ZKS_LIVE_SMOKE_MODEL does not exist: ${ZKS_LIVE_SMOKE_MODEL}")
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
