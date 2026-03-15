if(NOT DEFINED XXD OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedXxd.cmake requires XXD, INPUT, OUTPUT, and SYMBOL.")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${XXD}" -n "${SYMBOL}" -i "${INPUT}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE xxd_result
    ERROR_VARIABLE xxd_error
)

if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "xxd failed for ${INPUT}: ${xxd_error}")
endif()
