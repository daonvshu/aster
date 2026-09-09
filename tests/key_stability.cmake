if(WIN32 AND DEFINED QT_RUNTIME_DIR)
    set(ENV{PATH} "${QT_RUNTIME_DIR};$ENV{PATH}")
endif()

execute_process(
    COMMAND "${PROGRAM}" --key-digest
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first
    ERROR_VARIABLE first_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND "${PROGRAM}" --key-digest
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second
    ERROR_VARIABLE second_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT "${first_result}" STREQUAL "0" OR NOT "${second_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Key probe failed: first=${first_result} (${first_error}), "
        "second=${second_result} (${second_error})"
    )
endif()

if(NOT "${first}" STREQUAL "${second}" OR "${first}" STREQUAL "")
    message(FATAL_ERROR "Key digests differ between processes: '${first}' / '${second}'")
endif()

message(STATUS "Stable cross-process key: ${first}")
