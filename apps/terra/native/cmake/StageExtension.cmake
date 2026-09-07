# Do not stage while Terra is running. The lock prevents overlapping stage operations.
if(NOT EXISTS "${TERRA_STAGE_SOURCE}/terra-core" AND
   NOT EXISTS "${TERRA_STAGE_SOURCE}/terra-core.exe")
  message(FATAL_ERROR "Extension output missing: ${TERRA_STAGE_SOURCE}. Build first.")
endif()
if(NOT DEFINED TERRA_STAGE_DESTINATION OR TERRA_STAGE_DESTINATION STREQUAL "")
  message(FATAL_ERROR "TERRA_STAGE_DESTINATION is required")
endif()
file(MAKE_DIRECTORY "${TERRA_STAGE_DESTINATION}")
file(LOCK "${TERRA_STAGE_DESTINATION}/../.native-stage.lock" GUARD PROCESS TIMEOUT 0)
# Remove only generated native runtime files, including dependencies from old builds.
file(GLOB previous_runtime
  "${TERRA_STAGE_DESTINATION}/*.dll"
  "${TERRA_STAGE_DESTINATION}/terra-core"
  "${TERRA_STAGE_DESTINATION}/terra-core.exe"
  "${TERRA_STAGE_DESTINATION}/terra-core.pdb"
  "${TERRA_STAGE_DESTINATION}/terra-wayland-overlay"
  "${TERRA_STAGE_DESTINATION}/libterra-wayland-parent.so"
  "${TERRA_STAGE_DESTINATION}/gamecontrollerdb.txt")
if(previous_runtime)
  file(REMOVE ${previous_runtime})
endif()
file(COPY "${TERRA_STAGE_SOURCE}/" DESTINATION "${TERRA_STAGE_DESTINATION}")
message(STATUS "Staged Terra extension from ${TERRA_STAGE_SOURCE}")
