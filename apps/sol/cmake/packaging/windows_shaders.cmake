# Repair only the build-tree link; never recursively remove shader directories.
function(sol_prepare_shader_directory source destination)
    if(NOT IS_DIRECTORY "${source}")
        message(FATAL_ERROR "Shader source directory missing: ${source}")
    endif()

    cmake_path(CONVERT "${source}" TO_NATIVE_PATH_LIST source_native)
    cmake_path(CONVERT "${destination}" TO_NATIVE_PATH_LIST destination_native)
    if(IS_SYMLINK "${destination}")
        file(REAL_PATH "${source}" expected_target)
        file(REAL_PATH "${destination}" actual_target)
        if(NOT actual_target STREQUAL expected_target)
            # rmdir without /S removes a junction, not its target or target files.
            execute_process(COMMAND cmd.exe /d /c rmdir "${destination_native}"
                    COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endif()

    if(IS_DIRECTORY "${destination}")
        return()
    endif()
    if(EXISTS "${destination}")
        message(FATAL_ERROR "Shader staging path is not a directory: ${destination}")
    endif()

    execute_process(COMMAND cmd.exe /d /c mklink /J "${destination_native}" "${source_native}"
            RESULT_VARIABLE link_result OUTPUT_VARIABLE link_output ERROR_VARIABLE link_error)
    if(NOT link_result EQUAL 0 OR NOT IS_DIRECTORY "${destination}")
        message(FATAL_ERROR "Cannot create shader junction at ${destination}: ${link_output}${link_error}")
    endif()
endfunction()
