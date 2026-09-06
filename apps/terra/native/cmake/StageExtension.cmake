# Do not stage while Terra is running. The lock prevents overlapping stage operations.
if(NOT EXISTS "${ECLIPSE_STAGE_SOURCE}/eclipse-core" AND
   NOT EXISTS "${ECLIPSE_STAGE_SOURCE}/eclipse-core.exe")
  message(FATAL_ERROR "Extension output missing: ${ECLIPSE_STAGE_SOURCE}. Build first.")
endif()
if(NOT DEFINED ECLIPSE_STAGE_DESTINATION OR ECLIPSE_STAGE_DESTINATION STREQUAL "")
  message(FATAL_ERROR "ECLIPSE_STAGE_DESTINATION is required")
endif()
file(MAKE_DIRECTORY "${ECLIPSE_STAGE_DESTINATION}")
file(LOCK "${ECLIPSE_STAGE_DESTINATION}/../.native-stage.lock" GUARD PROCESS TIMEOUT 0)
# Remove only generated native runtime files, including dependencies from old builds.
file(GLOB previous_runtime
  "${ECLIPSE_STAGE_DESTINATION}/*.dll"
  "${ECLIPSE_STAGE_DESTINATION}/eclipse-core"
  "${ECLIPSE_STAGE_DESTINATION}/eclipse-core.exe"
  "${ECLIPSE_STAGE_DESTINATION}/eclipse-core.pdb"
  "${ECLIPSE_STAGE_DESTINATION}/eclipse-wayland-overlay"
  "${ECLIPSE_STAGE_DESTINATION}/libeclipse-wayland-parent.so"
  "${ECLIPSE_STAGE_DESTINATION}/gamecontrollerdb.txt")
if(previous_runtime)
  file(REMOVE ${previous_runtime})
endif()
file(COPY "${ECLIPSE_STAGE_SOURCE}/" DESTINATION "${ECLIPSE_STAGE_DESTINATION}")
message(STATUS "Staged Terra extension from ${ECLIPSE_STAGE_SOURCE}")
