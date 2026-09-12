cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sol)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/lizardbyte-common/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
if(NOT TARGET gtest)
    add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
option(SOL_LLVM_COVERAGE "Use LLVM source-based coverage" OFF)
if(SOL_LLVM_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -ggdb -O0)
    add_link_options(-fprofile-instr-generate)
else()
    set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
    set(CMAKE_C_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
endif()

# Link the libgcov that belongs to the selected compiler. Asking GCC directly
# avoids selecting another installed GCC version through platform-specific
# library directory names (for example, the GCC required by CUDA on Arch Linux).
if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgcov.a
        OUTPUT_VARIABLE LIBGCOV_LIBRARY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE LIBGCOV_RESULT
    )

    if(LIBGCOV_RESULT EQUAL 0 AND EXISTS "${LIBGCOV_LIBRARY}")
        message(STATUS "Found libgcov: ${LIBGCOV_LIBRARY}")
        set(GCOV_LINK_LIBRARY "${LIBGCOV_LIBRARY}")
    else()
        message(WARNING "Could not find libgcov for ${CMAKE_C_COMPILER}")
        unset(GCOV_LINK_LIBRARY)
    endif()
endif()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

# modify SOL_DEFINITIONS
if (WIN32)
    list(APPEND
            SOL_DEFINITIONS SOL_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND SOL_DEFINITIONS SOL_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed

# this indicates we're building tests in case sol needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS SOL_TESTS)
list(APPEND TEST_DEFINITIONS SOL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
list(APPEND TEST_DEFINITIONS SOL_TEST_BIN_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# Override SOL_ASSETS_DIR to use a writable temp directory for tests
# Remove the existing definition from SOL_DEFINITIONS to avoid redefinition error
list(FILTER SOL_DEFINITIONS EXCLUDE REGEX "^SOL_ASSETS_DIR=")
list(APPEND TEST_DEFINITIONS SOL_ASSETS_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets")

if(NOT WIN32)
    find_package(Udev 255)  # we need 255+ for udevadm verify
    message(STATUS "UDEV_FOUND: ${UDEV_FOUND}")
    if(UDEV_FOUND)
        list(APPEND TEST_DEFINITIONS UDEVADM_EXECUTABLE="${UDEVADM_EXECUTABLE}")
    endif()
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)
if(SOL_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(SOL_TRAY_TEST_ICON_SOURCE_DIR
            "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/images")
    set(SOL_TRAY_TEST_ICON_DESTINATION_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/test_assets/web/images")
    file(MAKE_DIRECTORY "${SOL_TRAY_TEST_ICON_DESTINATION_DIR}")

    configure_file(
            "${CMAKE_SOURCE_DIR}/sol.svg"
            "${SOL_TRAY_TEST_ICON_DESTINATION_DIR}/logo-sol.svg"
            COPYONLY)

    if(WIN32)
        configure_file(
                "${CMAKE_SOURCE_DIR}/third-party/libvirtualhid/libvirtualhid.svg"
                "${SOL_TRAY_TEST_ICON_DESTINATION_DIR}/logo-libvirtualhid.svg"
                COPYONLY)
    endif()

    set(SOL_TRAY_TEST_ICONS
            sol-locked.svg
            sol-playing.svg
            sol-pausing.svg)

    foreach(icon IN LISTS SOL_TRAY_TEST_ICONS)
        configure_file(
                "${SOL_TRAY_TEST_ICON_SOURCE_DIR}/${icon}"
                "${SOL_TRAY_TEST_ICON_DESTINATION_DIR}/${icon}"
                COPYONLY)
    endforeach()

    add_library(sol_tray_test_helpers STATIC
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/notification_utils.cpp
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/screenshot_utils.cpp)
    set_target_properties(sol_tray_test_helpers PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
    target_include_directories(sol_tray_test_helpers PRIVATE
            ${CMAKE_SOURCE_DIR}/third-party/tray)
    target_link_libraries(sol_tray_test_helpers PRIVATE
            lizardbyte::common)

    if(WIN32)
        target_link_libraries(sol_tray_test_helpers PRIVATE
                gdi32
                gdiplus)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui)
        set(SOL_TRAY_TEST_QT_GUI_TARGET Qt${QT_VERSION_MAJOR}::Gui)

        target_link_libraries(sol_tray_test_helpers PRIVATE
                ${SOL_TRAY_TEST_QT_GUI_TARGET})
        if(APPLE)
            target_link_libraries(sol_tray_test_helpers PRIVATE
                    "-framework Cocoa")
        endif()
    endif()
endif()
# Add macOS-specific test files only when building tests for macOS
if (APPLE)
    file(GLOB_RECURSE MACOS_TEST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/*.mm)
    list(APPEND TEST_SOURCES ${MACOS_TEST_SOURCES})
endif ()

set(SOL_SOURCES
        ${SOL_TARGET_FILES})

# remove main.cpp from the list of sources
list(REMOVE_ITEM SOL_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
        ${SOL_SOURCES})

# Copy files needed for config consistency tests to build directory
# This ensures both CLI and CLion can access the same files relative to the test executable
# Using configure_file ensures files are copied when they change between builds
set(INTEGRATION_TEST_FILES
    "src/config.cpp"
    "src_assets/common/assets/web/config.html"
    "docs/configuration.md"
    "src_assets/common/assets/web/public/assets/locale/en.json"
    "src_assets/common/assets/web/src/routes/config/tabs/GeneralTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/InputTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/AudioVideoTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/NetworkTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/FilesTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/AdvancedTab.tsx"
    "src_assets/common/assets/web/src/routes/config/tabs/EncoderTabs.tsx"
    "src_assets/linux/misc/60-sol.rules"
)

foreach(file ${INTEGRATION_TEST_FILES})
    configure_file(
        "${CMAKE_SOURCE_DIR}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/${file}"
        COPYONLY
    )
endforeach()

# Copy all locale files for locale consistency tests
# Use a custom command to properly handle both adding and removing files
set(LOCALE_SRC_DIR "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/assets/locale")
set(LOCALE_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/src_assets/common/assets/web/public/assets/locale")
add_custom_target(sync_locale_files ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOCALE_SRC_DIR}" "${LOCALE_DST_DIR}"
    COMMENT "Synchronizing locale files for tests"
    VERBATIM
)

foreach(dep ${SOL_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sol
endforeach()

# Ensure locale files are synchronized before building the test executable
add_dependencies(${PROJECT_NAME} sync_locale_files)

# Build the list of libraries to link
set(TEST_LINK_LIBRARIES
    ${SOL_EXTERNAL_LIBRARIES}
    lizardbyte::test_support
    ${PLATFORM_LIBRARIES}
)

if(SOL_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    list(APPEND TEST_LINK_LIBRARIES
            sol_tray_test_helpers)
    if(SOL_TRAY_TEST_QT_GUI_TARGET)
        list(APPEND TEST_LINK_LIBRARIES
                ${SOL_TRAY_TEST_QT_GUI_TARGET})
    endif()
endif()

# Add the specific libgcov library if found
if(GCOV_LINK_LIBRARY)
    list(APPEND TEST_LINK_LIBRARIES ${GCOV_LINK_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME} ${TEST_LINK_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC ${SOL_DEFINITIONS} ${TEST_DEFINITIONS})
set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        PROPERTIES COMPILE_DEFINITIONS "${SOL_VERSION_DEFINITIONS}")
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SOL_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SOL_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE ${SOL_LINK_OPTIONS})

if (WIN32)
    # prefer static libraries since we're linking statically
    # this fixes libcurl linking errors when using non MSYS2 version of CMake
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)

    # Copy minhook-detours DLL to test binary directory for ARM64
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_MINHOOK_DLL}"
                "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Copying minhook-detours DLL to test binary directory"
            VERBATIM
        )
    endif()
endif ()
