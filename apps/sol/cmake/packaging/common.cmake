# common packaging

# common cpack options
set(CPACK_PACKAGE_NAME ${CMAKE_PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "LizardByte")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cpack_artifacts)
set(CPACK_PACKAGE_CONTACT "https://app.lizardbyte.dev")
set(CPACK_PACKAGE_DESCRIPTION ${CMAKE_PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_HOMEPAGE_URL ${CMAKE_PROJECT_HOMEPAGE_URL})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_ICON ${PROJECT_SOURCE_DIR}/sol.png)
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_STRIP_FILES YES)

# install common assets
install(DIRECTORY "${SOL_SOURCE_ASSETS_DIR}/common/assets/"
        DESTINATION "${SOL_ASSETS_DIR}"
        PATTERN "web" EXCLUDE)
# copy assets to build directory, for running without install
file(GLOB_RECURSE ALL_ASSETS
        RELATIVE "${SOL_SOURCE_ASSETS_DIR}/common/assets/" "${SOL_SOURCE_ASSETS_DIR}/common/assets/*")
list(FILTER ALL_ASSETS EXCLUDE REGEX "^web/.*$")  # Filter out the web directory
foreach(asset ${ALL_ASSETS})  # Copy assets to build directory, excluding the web directory
    file(COPY "${SOL_SOURCE_ASSETS_DIR}/common/assets/${asset}"
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/assets")
endforeach()

# Copy the primary application icon into the built web assets for the system tray.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets/web/images")
configure_file(
        "${CMAKE_SOURCE_DIR}/sol.svg"
        "${CMAKE_CURRENT_BINARY_DIR}/assets/web/images/logo-sol.svg"
        COPYONLY)

# Copy the Virtual HID Driver icon for Windows tray notifications.
if(WIN32)
    configure_file(
            "${CMAKE_SOURCE_DIR}/third-party/libvirtualhid/libvirtualhid.svg"
            "${CMAKE_CURRENT_BINARY_DIR}/assets/web/images/logo-libvirtualhid.svg"
            COPYONLY)
endif()

# Packaging must consume a completed web build, not just the tray icons above.
install(CODE "
    if(NOT EXISTS \"${SOL_WEB_ASSETS_DIR}/index.html\")
        message(FATAL_ERROR \"Web assets missing: ${SOL_WEB_ASSETS_DIR}/index.html. Build and stage the web UI before packaging.\")
    endif()
")
install(DIRECTORY "${SOL_WEB_ASSETS_DIR}/"
        DESTINATION "${SOL_ASSETS_DIR}/web")
# Tray icons are native assets and may not be present in an external web stage.
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets/web/images"
        DESTINATION "${SOL_ASSETS_DIR}/web")

# platform specific packaging
if(WIN32)
    include(${CMAKE_MODULE_PATH}/packaging/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/packaging/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/packaging/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/packaging/linux.cmake)
    endif()
endif()

include(CPack)
