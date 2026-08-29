cmake_minimum_required(VERSION 3.24)

project(eclipse-client LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ECLIPSE_LINK_MOONLIGHT_COMMON "Build and link pinned moonlight-common-c" ON)
option(ECLIPSE_ENABLE_WINDOWS_VIDEO "Build FFmpeg D3D11VA video output on Windows" ON)

set(ECLIPSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(MOONLIGHT_QT_ROOT "${ECLIPSE_ROOT}/refs/moonlight-qt")
set(MOONLIGHT_COMMON_ROOT "${MOONLIGHT_QT_ROOT}/moonlight-common-c/moonlight-common-c")
set(ECLIPSE_EXTENSION_OUTPUT "${ECLIPSE_ROOT}/extensions/eclipse-core/bin")

if(NOT EXISTS "${MOONLIGHT_QT_ROOT}/.git")
  message(FATAL_ERROR "Moonlight submodule missing. Run: git submodule update --init --recursive")
endif()

find_package(Git QUIET)

function(eclipse_git_revision directory output_variable)
  if(Git_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${directory}"
      OUTPUT_VARIABLE revision
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()

  if(NOT revision)
    set(revision "unknown")
  endif()

  set(${output_variable} "${revision}" PARENT_SCOPE)
endfunction()

eclipse_git_revision("${MOONLIGHT_QT_ROOT}" ECLIPSE_MOONLIGHT_QT_REVISION)
eclipse_git_revision("${MOONLIGHT_COMMON_ROOT}" ECLIPSE_MOONLIGHT_COMMON_REVISION)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(USE_TLS ON CACHE BOOL "IXWebSocket TLS support" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "IXWebSocket OpenSSL backend" FORCE)
set(USE_ZLIB OFF CACHE BOOL "IXWebSocket compression support" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Install IXWebSocket" FORCE)

FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v12.0.1
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)
FetchContent_Declare(
  pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(ixwebsocket nlohmann_json pugixml)

if(WIN32 AND ECLIPSE_ENABLE_WINDOWS_VIDEO)
  FetchContent_Declare(
    ffmpeg_prebuilt
    URL https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-full_build-shared.7z
    URL_HASH SHA256=cba748035c21ce1431d0823c7a3a711f38616f89f87a265dceddf9b7f6749d2d
  )
  FetchContent_MakeAvailable(ffmpeg_prebuilt)

  file(GLOB ffmpeg_roots LIST_DIRECTORIES TRUE "${ffmpeg_prebuilt_SOURCE_DIR}/*")
  list(APPEND ffmpeg_roots "${ffmpeg_prebuilt_SOURCE_DIR}")
  find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS ${ffmpeg_roots} PATH_SUFFIXES include NO_DEFAULT_PATH REQUIRED)
  find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec PATHS ${ffmpeg_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH REQUIRED)
  find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil PATHS ${ffmpeg_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH REQUIRED)
  find_library(FFMPEG_SWRESAMPLE_LIBRARY NAMES swresample PATHS ${ffmpeg_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH REQUIRED)
  find_file(FFMPEG_AVCODEC_DLL NAMES avcodec-62.dll PATHS ${ffmpeg_roots} PATH_SUFFIXES bin NO_DEFAULT_PATH REQUIRED)
  find_file(FFMPEG_AVUTIL_DLL NAMES avutil-60.dll PATHS ${ffmpeg_roots} PATH_SUFFIXES bin NO_DEFAULT_PATH REQUIRED)
  find_file(FFMPEG_SWRESAMPLE_DLL NAMES swresample-6.dll PATHS ${ffmpeg_roots} PATH_SUFFIXES bin NO_DEFAULT_PATH REQUIRED)
  get_filename_component(FFMPEG_BIN_DIR "${FFMPEG_AVCODEC_DLL}" DIRECTORY)

  add_library(ffmpeg-avcodec SHARED IMPORTED)
  set_target_properties(ffmpeg-avcodec PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_AVCODEC_LIBRARY}"
    IMPORTED_LOCATION "${FFMPEG_AVCODEC_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
  )
  add_library(ffmpeg-avutil SHARED IMPORTED)
  set_target_properties(ffmpeg-avutil PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_AVUTIL_LIBRARY}"
    IMPORTED_LOCATION "${FFMPEG_AVUTIL_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
  )
  add_library(ffmpeg-swresample SHARED IMPORTED)
  set_target_properties(ffmpeg-swresample PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_SWRESAMPLE_LIBRARY}"
    IMPORTED_LOCATION "${FFMPEG_SWRESAMPLE_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
  )
endif()

if(ECLIPSE_LINK_MOONLIGHT_COMMON)
  if(NOT EXISTS "${MOONLIGHT_COMMON_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "moonlight-common-c is missing. Initialize recursive submodules.")
  endif()

  add_subdirectory("${MOONLIGHT_COMMON_ROOT}" "${CMAKE_BINARY_DIR}/moonlight-common-c" EXCLUDE_FROM_ALL)
endif()

find_package(OpenSSL REQUIRED)

add_executable(
  eclipse-core
  src/control_plane.cpp
  src/audio_renderer.cpp
  src/gamestream_client.cpp
  src/identity.cpp
  src/input_forwarder.cpp
  src/main.cpp
  src/stream_session.cpp
  src/video_renderer.cpp
)

target_link_libraries(
  eclipse-core
  PRIVATE
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    pugixml::pugixml
)

if(ECLIPSE_LINK_MOONLIGHT_COMMON)
  target_link_libraries(eclipse-core PRIVATE moonlight-common-c)
  target_compile_definitions(eclipse-core PRIVATE ECLIPSE_HAS_MOONLIGHT_COMMON=1)
endif()

if(WIN32)
  target_link_libraries(eclipse-core PRIVATE Crypt32)
  if(ECLIPSE_ENABLE_WINDOWS_VIDEO)
    target_link_libraries(eclipse-core PRIVATE ffmpeg-avcodec ffmpeg-avutil ffmpeg-swresample d3d11 dxgi ole32 uuid)
    target_compile_definitions(eclipse-core PRIVATE ECLIPSE_HAS_WINDOWS_VIDEO=1)
  endif()
endif()

target_compile_definitions(
  eclipse-core
  PRIVATE
    ECLIPSE_MOONLIGHT_QT_REVISION="${ECLIPSE_MOONLIGHT_QT_REVISION}"
    ECLIPSE_MOONLIGHT_COMMON_REVISION="${ECLIPSE_MOONLIGHT_COMMON_REVISION}"
)

if(MSVC)
  target_compile_options(eclipse-core PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(eclipse-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(
  eclipse-core
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ECLIPSE_EXTENSION_OUTPUT}"
)

if(WIN32)
  get_filename_component(ECLIPSE_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)

  add_custom_command(
    TARGET eclipse-core
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}"
      "-DECLIPSE_EXECUTABLE=$<TARGET_FILE:eclipse-core>"
      "-DECLIPSE_OUTPUT_DIR=$<TARGET_FILE_DIR:eclipse-core>"
      "-DECLIPSE_SEARCH_DIRECTORIES=${ECLIPSE_COMPILER_DIRECTORY};${FFMPEG_BIN_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDependencies.cmake"
    VERBATIM
  )
endif()

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${configuration}" configuration_upper)
  set_target_properties(
    eclipse-core
    PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${ECLIPSE_EXTENSION_OUTPUT}"
  )
endforeach()
