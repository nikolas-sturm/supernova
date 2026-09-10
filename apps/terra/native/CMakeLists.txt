cmake_minimum_required(VERSION 3.24)

project(terra-client LANGUAGES C CXX)

include(FetchContent)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TERRA_LINK_MOONLIGHT_COMMON "Build and link pinned moonlight-common-c" ON)
option(TERRA_ENABLE_WINDOWS_VIDEO "Build FFmpeg D3D11VA video output on Windows" ON)
option(TERRA_ENABLE_LINUX_VIDEO "Build FFmpeg SDL video output on Linux" ON)
option(TERRA_ENABLE_LINUX_AUDIO "Build FFmpeg SDL audio output on Linux" ON)
option(TERRA_ENABLE_VAAPI_X11 "Enable direct VAAPI presentation on native X11" ON)
option(TERRA_ENABLE_LIBPLACEBO "Enable Vulkan HDR and YUV 4:4:4 presentation on Linux" ON)

set(TERRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(MOONLIGHT_QT_ROOT "${TERRA_ROOT}/refs/moonlight-qt")
set(MOONLIGHT_COMMON_ROOT "${MOONLIGHT_QT_ROOT}/moonlight-common-c/moonlight-common-c")
set(TERRA_EXTENSION_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/extension/$<CONFIG>")

if(NOT EXISTS "${MOONLIGHT_QT_ROOT}/.git")
  message(FATAL_ERROR "Moonlight submodule missing. Run: git submodule update --init --recursive")
endif()

find_package(Git QUIET)

function(terra_git_revision directory output_variable)
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

terra_git_revision("${MOONLIGHT_QT_ROOT}" TERRA_MOONLIGHT_QT_REVISION)
terra_git_revision("${MOONLIGHT_COMMON_ROOT}" TERRA_MOONLIGHT_COMMON_REVISION)

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

if(WIN32 AND TERRA_ENABLE_WINDOWS_VIDEO)
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

if(TERRA_LINK_MOONLIGHT_COMMON)
  if(NOT EXISTS "${MOONLIGHT_COMMON_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "moonlight-common-c is missing. Initialize recursive submodules.")
  endif()

  add_subdirectory("${MOONLIGHT_COMMON_ROOT}" "${CMAKE_BINARY_DIR}/moonlight-common-c" EXCLUDE_FROM_ALL)
endif()

find_package(OpenSSL REQUIRED)

# Dependencies retain their upstream standards; first-party C++ uses C++23.
set(CMAKE_CXX_STANDARD 23)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (TERRA_ENABLE_LINUX_VIDEO OR TERRA_ENABLE_LINUX_AUDIO))
  find_package(PkgConfig REQUIRED)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TERRA_ENABLE_LINUX_VIDEO)
  pkg_check_modules(LINUX_VIDEO REQUIRED IMPORTED_TARGET sdl2>=2.0.16 libavcodec>=59 libavutil>=57)
  pkg_check_modules(LINUX_WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-protocols)
  pkg_check_modules(LINUX_GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)
  pkg_check_modules(LINUX_WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1>=2.40)
  if(TERRA_ENABLE_VAAPI_X11)
    pkg_check_modules(LINUX_VAAPI_X11 QUIET IMPORTED_TARGET libva>=1.8 libva-x11>=1.8 x11)
  endif()
  if(TERRA_ENABLE_LIBPLACEBO)
    pkg_check_modules(LINUX_LIBPLACEBO QUIET IMPORTED_TARGET libplacebo>=7 vulkan libavformat>=59)
  endif()
endif()

if(TARGET PkgConfig::LINUX_WAYLAND AND TARGET PkgConfig::LINUX_GTK3)
  pkg_get_variable(WAYLAND_PROTOCOLS_DATA_DIR wayland-protocols pkgdatadir)
  find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
  set(XDG_SHELL_PROTOCOL "${WAYLAND_PROTOCOLS_DATA_DIR}/stable/xdg-shell/xdg-shell.xml")
  set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
  set(XDG_SHELL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
  add_custom_command(
    OUTPUT "${XDG_SHELL_HEADER}" "${XDG_SHELL_SOURCE}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
            "${XDG_SHELL_PROTOCOL}" "${XDG_SHELL_HEADER}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
            "${XDG_SHELL_PROTOCOL}" "${XDG_SHELL_SOURCE}"
    DEPENDS "${XDG_SHELL_PROTOCOL}"
    VERBATIM
  )
  set(WLR_LAYER_SHELL_PROTOCOL
      "${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")
  set(WLR_LAYER_SHELL_HEADER
      "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h")
  set(WLR_LAYER_SHELL_SOURCE
      "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c")
  add_custom_command(
    OUTPUT "${WLR_LAYER_SHELL_HEADER}" "${WLR_LAYER_SHELL_SOURCE}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
            "${WLR_LAYER_SHELL_PROTOCOL}" "${WLR_LAYER_SHELL_HEADER}"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
            "${WLR_LAYER_SHELL_PROTOCOL}" "${WLR_LAYER_SHELL_SOURCE}"
    DEPENDS "${WLR_LAYER_SHELL_PROTOCOL}"
    VERBATIM
  )

  add_executable(terra-wayland-overlay
    src/wayland_overlay.c
    "${XDG_SHELL_SOURCE}"
    "${WLR_LAYER_SHELL_SOURCE}"
  )
  set_source_files_properties(
    "${XDG_SHELL_SOURCE}"
    "${WLR_LAYER_SHELL_SOURCE}"
    PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden"
  )
  target_include_directories(terra-wayland-overlay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(terra-wayland-overlay PRIVATE
    PkgConfig::LINUX_GTK3
    PkgConfig::LINUX_WEBKIT
    PkgConfig::LINUX_WAYLAND
  )
  target_compile_options(terra-wayland-overlay PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(
    terra-wayland-overlay
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TERRA_EXTENSION_OUTPUT}"
  )
  add_custom_command(
    TARGET terra-wayland-overlay
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${TERRA_EXTENSION_OUTPUT}/libterra-wayland-parent.so"
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TERRA_ENABLE_LINUX_AUDIO)
  pkg_check_modules(LINUX_AUDIO REQUIRED IMPORTED_TARGET sdl2>=2.0.16 libavcodec>=59 libavutil>=57 libswresample>=4)
endif()

add_executable(
  terra-core
  src/control_plane.cpp
  src/audio_renderer.cpp
  src/gamestream_client.cpp
  src/identity.cpp
  src/input_forwarder.cpp
  src/main.cpp
  src/mdns_discovery.cpp
  src/network_route.cpp
  src/stream_statistics.cpp
  src/stream_session.cpp
  src/video_renderer.cpp
  src/wake_on_lan.cpp
)

target_link_libraries(
  terra-core
  PRIVATE
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    pugixml::pugixml
)

if(TERRA_LINK_MOONLIGHT_COMMON)
  target_link_libraries(terra-core PRIVATE moonlight-common-c)
  target_compile_definitions(terra-core PRIVATE TERRA_HAS_MOONLIGHT_COMMON=1)
endif()

if(WIN32)
  target_link_libraries(terra-core PRIVATE Crypt32 Iphlpapi)
  # The renderer directly uses GetDpiForWindow (Windows 10 version 1607+).
  target_compile_definitions(terra-core PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
  if(TERRA_ENABLE_WINDOWS_VIDEO)
    target_link_libraries(terra-core PRIVATE ffmpeg-avcodec ffmpeg-avutil ffmpeg-swresample d3d11 d3dcompiler dxgi ole32 uuid)
    target_compile_definitions(terra-core PRIVATE TERRA_HAS_WINDOWS_VIDEO=1)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TERRA_ENABLE_LINUX_VIDEO)
  target_link_libraries(terra-core PRIVATE PkgConfig::LINUX_VIDEO)
  target_compile_definitions(terra-core PRIVATE TERRA_HAS_LINUX_VIDEO=1)
  if(TARGET PkgConfig::LINUX_VAAPI_X11)
    target_link_libraries(terra-core PRIVATE PkgConfig::LINUX_VAAPI_X11)
    target_compile_definitions(terra-core PRIVATE TERRA_HAS_VAAPI_X11=1)
  endif()
  if(TARGET PkgConfig::LINUX_LIBPLACEBO)
    target_sources(terra-core PRIVATE src/placebo_libav.c)
    target_link_libraries(terra-core PRIVATE PkgConfig::LINUX_LIBPLACEBO)
    target_compile_definitions(terra-core PRIVATE TERRA_HAS_LIBPLACEBO=1)
  endif()
  set(TERRA_GAMEPAD_DATABASE "${TERRA_EXTENSION_OUTPUT}/gamecontrollerdb.txt")
  add_custom_command(
    OUTPUT "${TERRA_GAMEPAD_DATABASE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TERRA_EXTENSION_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${MOONLIGHT_QT_ROOT}/app/SDL_GameControllerDB/gamecontrollerdb.txt"
      "${TERRA_GAMEPAD_DATABASE}"
    DEPENDS "${MOONLIGHT_QT_ROOT}/app/SDL_GameControllerDB/gamecontrollerdb.txt"
    VERBATIM
  )
  add_custom_target(terra-gamepad-database ALL DEPENDS "${TERRA_GAMEPAD_DATABASE}")
  add_dependencies(terra-core terra-gamepad-database)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TERRA_ENABLE_LINUX_AUDIO)
  target_link_libraries(terra-core PRIVATE PkgConfig::LINUX_AUDIO)
  target_compile_definitions(terra-core PRIVATE TERRA_HAS_LINUX_AUDIO=1)
endif()

target_compile_definitions(
  terra-core
  PRIVATE
    TERRA_MOONLIGHT_QT_REVISION="${TERRA_MOONLIGHT_QT_REVISION}"
    TERRA_MOONLIGHT_COMMON_REVISION="${TERRA_MOONLIGHT_COMMON_REVISION}"
)

if(MSVC)
  target_compile_options(terra-core PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(terra-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(
  terra-core
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TERRA_EXTENSION_OUTPUT}"
)

if(WIN32)
  get_filename_component(TERRA_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)

  add_custom_command(
    TARGET terra-core
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}"
      "-DTERRA_EXECUTABLE=$<TARGET_FILE:terra-core>"
      "-DTERRA_OUTPUT_DIR=$<TARGET_FILE_DIR:terra-core>"
      "-DTERRA_SEARCH_DIRECTORIES=${TERRA_COMPILER_DIRECTORY};${FFMPEG_BIN_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDependencies.cmake"
    VERBATIM
  )
endif()

# Shared runtime location is populated only by an explicit staging operation.
add_custom_target(stage-extension
  COMMAND "${CMAKE_COMMAND}"
    "-DTERRA_STAGE_SOURCE=${TERRA_EXTENSION_OUTPUT}"
    "-DTERRA_STAGE_DESTINATION=${TERRA_ROOT}/extensions/terra-core/bin"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageExtension.cmake"
  DEPENDS terra-core
  VERBATIM
)
if(TARGET terra-wayland-overlay)
  add_dependencies(stage-extension terra-wayland-overlay)
endif()

if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(
    terra-stream-session-test
    tests/stream_session_test.cpp
    src/network_route.cpp
    src/stream_session.cpp
    src/stream_statistics.cpp
  )
  target_include_directories(
    terra-stream-session-test
    PRIVATE
      src
      "${MOONLIGHT_COMMON_ROOT}/src"
  )
  target_link_libraries(terra-stream-session-test PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(terra-stream-session-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-stream-session-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME stream-session-lifecycle COMMAND terra-stream-session-test)

  add_executable(
    terra-network-route-test
    tests/network_route_test.cpp
    src/network_route.cpp
  )
  target_include_directories(terra-network-route-test PRIVATE src)
  if(MSVC)
    target_compile_options(terra-network-route-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-network-route-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME network-route COMMAND terra-network-route-test)

  add_executable(terra-display-mode-test tests/display_mode_test.cpp)
  target_include_directories(terra-display-mode-test PRIVATE src)
  if(MSVC)
    target_compile_options(terra-display-mode-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-display-mode-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME display-mode COMMAND terra-display-mode-test)

  add_executable(
    terra-wake-on-lan-test
    tests/wake_on_lan_test.cpp
    src/wake_on_lan.cpp
  )
  target_include_directories(terra-wake-on-lan-test PRIVATE src)
  if(MSVC)
    target_compile_options(terra-wake-on-lan-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-wake-on-lan-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME wake-on-lan COMMAND terra-wake-on-lan-test)

  add_executable(
    terra-mdns-discovery-test
    tests/mdns_discovery_test.cpp
    src/mdns_discovery.cpp
  )
  target_include_directories(terra-mdns-discovery-test PRIVATE src)
  target_link_libraries(terra-mdns-discovery-test PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(terra-mdns-discovery-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-mdns-discovery-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME mdns-discovery COMMAND terra-mdns-discovery-test)

  add_executable(
    terra-stream-statistics-test
    tests/stream_statistics_test.cpp
    src/stream_statistics.cpp
  )
  target_include_directories(terra-stream-statistics-test PRIVATE src)
  if(MSVC)
    target_compile_options(terra-stream-statistics-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-stream-statistics-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME stream-statistics COMMAND terra-stream-statistics-test)

  add_executable(terra-sse-decoder-test tests/sse_decoder_test.cpp)
  target_include_directories(terra-sse-decoder-test PRIVATE src)
  target_link_libraries(terra-sse-decoder-test PRIVATE nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(terra-sse-decoder-test PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(terra-sse-decoder-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME sse-decoder COMMAND terra-sse-decoder-test)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TERRA_ENABLE_LINUX_VIDEO)
    add_executable(
      terra-input-forwarder-test
      tests/input_forwarder_test.cpp
      src/input_forwarder.cpp
      src/stream_statistics.cpp
      src/video_renderer.cpp
    )
    target_include_directories(
      terra-input-forwarder-test
      PRIVATE
        src
        "${MOONLIGHT_COMMON_ROOT}/src"
    )
    target_link_libraries(terra-input-forwarder-test PRIVATE PkgConfig::LINUX_VIDEO)
    target_compile_definitions(terra-input-forwarder-test PRIVATE TERRA_HAS_LINUX_VIDEO=1)
    if(TARGET PkgConfig::LINUX_LIBPLACEBO)
      target_sources(terra-input-forwarder-test PRIVATE src/placebo_libav.c)
      target_link_libraries(terra-input-forwarder-test PRIVATE PkgConfig::LINUX_LIBPLACEBO)
      target_compile_definitions(terra-input-forwarder-test PRIVATE TERRA_HAS_LIBPLACEBO=1)
    endif()
    target_compile_options(terra-input-forwarder-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME input-forwarder COMMAND terra-input-forwarder-test)
    set_tests_properties(input-forwarder PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  endif()
endif()

if(WIN32)
  target_link_libraries(terra-core PRIVATE Ws2_32)
  if(TARGET terra-wake-on-lan-test)
    target_link_libraries(terra-wake-on-lan-test PRIVATE Ws2_32)
  endif()
  if(TARGET terra-mdns-discovery-test)
    target_link_libraries(terra-mdns-discovery-test PRIVATE Ws2_32 Iphlpapi)
  endif()
  if(TARGET terra-stream-session-test)
    target_link_libraries(terra-stream-session-test PRIVATE Ws2_32 Iphlpapi)
  endif()
  if(TARGET terra-network-route-test)
    target_link_libraries(terra-network-route-test PRIVATE Ws2_32 Iphlpapi)
  endif()
endif()
