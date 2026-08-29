if(NOT WIN32)
  return()
endif()

file(GLOB previous_dependencies "${ECLIPSE_OUTPUT_DIR}/*.dll")
file(REMOVE ${previous_dependencies})

file(
  GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${ECLIPSE_EXECUTABLE}"
  DIRECTORIES ${ECLIPSE_SEARCH_DIRECTORIES}
  RESOLVED_DEPENDENCIES_VAR resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
  PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
  POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32.*"
)

message(STATUS "Staging Eclipse runtime dependencies: ${resolved_dependencies}")

if(unresolved_dependencies)
  list(JOIN unresolved_dependencies ", " unresolved_message)
  message(FATAL_ERROR "Unresolved Eclipse runtime dependencies: ${unresolved_message}")
endif()

foreach(dependency IN LISTS resolved_dependencies)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dependency}" "${ECLIPSE_OUTPUT_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
  )
endforeach()
