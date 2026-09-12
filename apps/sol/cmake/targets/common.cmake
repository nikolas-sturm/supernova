# common target definitions
# this file will also load platform specific macros

if(APPLE AND NOT SOL_BUILD_HOMEBREW)
    add_executable(sol MACOSX_BUNDLE ${SOL_TARGET_FILES})
else()
    add_executable(sol ${SOL_TARGET_FILES})
endif()
foreach(dep ${SOL_TARGET_DEPENDENCIES})
    add_dependencies(sol ${dep})  # compile these before sol
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(sol ${SOL_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(sol PUBLIC ${SOL_DEFINITIONS})
# Git-derived version changes should rebuild only sources that expose the version.
set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        PROPERTIES COMPILE_DEFINITIONS "${SOL_VERSION_DEFINITIONS}")

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SOL_COMPILE_OPTIONS)
        list(APPEND SOL_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sol PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SOL_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SOL_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sol PRIVATE ${SOL_LINK_OPTIONS})

# Homebrew build fails the vite build if we set these environment variables
if(${SOL_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${SOL_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

# Web UI is optional for native-only orchestration. Never invoke Nx here.
if(SOL_BUILD_WEB_UI)
find_program(NPM npm REQUIRED)

if(WIN32)
    get_filename_component(NPM_DIRECTORY "${NPM}" DIRECTORY)
    find_program(NPM_NODE_EXECUTABLE NAMES node node.exe HINTS "${NPM_DIRECTORY}" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    string(CONCAT NPM_NODE_GNU_BINDING_CHECK
            "process.arch === 'x64' && "
            "(process.config.variables.shlib_suffix === 'dll.a' || "
            "process.config.variables.node_target_type === 'shared_library')")
    execute_process(
            COMMAND "${NPM_NODE_EXECUTABLE}" -p "${NPM_NODE_GNU_BINDING_CHECK}"
            OUTPUT_VARIABLE NPM_NODE_USES_GNU_BINDING
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NPM_NODE_USES_GNU_BINDING STREQUAL "true")
        message(FATAL_ERROR
                "The MSYS2 Node.js package is incompatible with Rolldown. "
                "Set NPM explicitly to native Windows Node.js npm.cmd, or disable SOL_BUILD_WEB_UI.")
    endif()

    set(NPM_COMMAND cmd /C)
    if(DEFINED ENV{MSYSTEM})
        # MSYS2 shells export a POSIX-style PATH that Windows cmd cannot parse.
        # Build a minimal native PATH from the npm directory and system roots.
        set(NPM_PATH "PATH=${NPM_DIRECTORY};$ENV{SystemRoot}/System32;$ENV{SystemRoot}/System32/Wbem;$ENV{SystemRoot}")
    else()
        set(NPM_PATH "PATH=${NPM_DIRECTORY};$ENV{PATH}")
    endif()
else()
    set(NPM_COMMAND)
    set(NPM_PATH "PATH=$ENV{PATH}")
endif()

get_filename_component(SUPERNOVA_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
add_custom_target(web-ui ALL
        WORKING_DIRECTORY "${SUPERNOVA_ROOT}"
        COMMENT "Building the Web UI (root npm install is a precondition)"
        COMMAND "${CMAKE_COMMAND}" -E env "${NPM_PATH}" "SOL_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}" "SOL_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}" "SOL_ASSETS_DIR=${NPM_ASSETS_DIR}" ${NPM_COMMAND} "${NPM}" run build --workspace apps/sol  # cmake-lint: disable=C0301
        VERBATIM)
endif()

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
