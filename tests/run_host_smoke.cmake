# Check both process exit and success marker. CTest's PASS_REGULAR_EXPRESSION
# alone would ignore ordinary non-zero process exit codes.
execute_process(
    COMMAND "${PIXEL_GODOT}" --headless --fixed-fps 60 --path "${PIXEL_GAME}" -- --smoke-test
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 120)
message("${output}${errors}")
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Godot smoke process failed: ${result}")
endif()
if(NOT output MATCHES "SMOKE PASS" OR "${output}${errors}" MATCHES "ERROR:|SMOKE FAIL")
    message(FATAL_ERROR "Godot smoke output is missing success or contains an engine error.")
endif()
