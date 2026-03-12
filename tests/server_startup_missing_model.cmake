if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

if(NOT DEFINED TEST_CONFIG)
    message(FATAL_ERROR "TEST_CONFIG is required")
endif()

execute_process(
    COMMAND "${TEST_EXECUTABLE}" "${TEST_CONFIG}"
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_stdout
    ERROR_VARIABLE test_stderr
)

set(test_output "${test_stdout}${test_stderr}")

if(test_result EQUAL 0)
    message(FATAL_ERROR
        "Expected zoo_keeper_server to fail startup for an invalid config.\nOutput:\n${test_output}"
    )
endif()

if(NOT test_output MATCHES "Model path cannot be empty")
    message(FATAL_ERROR
        "Expected startup failure output to mention the missing model path.\nOutput:\n${test_output}"
    )
endif()
