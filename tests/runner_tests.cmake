execute_process(COMMAND "${TEST_EXECUTABLE}" --filter nonexistent_group
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result STREQUAL "2")
  message(FATAL_ERROR "Unmatched filter must exit 2; got ${result}: ${output}${error}")
endif()
