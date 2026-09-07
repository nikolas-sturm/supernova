# Publisher Metadata
set(SOL_PUBLISHER_NAME "Third Party Publisher"
        CACHE STRING "The name of the publisher (not developer) of the application.")
set(SOL_PUBLISHER_WEBSITE ""
        CACHE STRING "The URL of the publisher's website.")
set(SOL_PUBLISHER_ISSUE_URL "https://app.lizardbyte.dev/support"
        CACHE STRING "The URL of the publisher's support site or issue tracker.
        If you provide a modified version of Sunshine, we kindly request that you use your own url.")

option(BUILD_DOCS "Build documentation" ON)
option(BUILD_TESTS "Build tests" ON)
option(SOL_BUILD_WEB_UI "Build the web UI with the native build (requires root npm install)" ON)
set(SOL_WEB_ASSETS_DIR "${CMAKE_BINARY_DIR}/assets/web"
        CACHE PATH "Staged web assets consumed by installation and packaging")

option(BUILD_WERROR "Enable -Werror flag." OFF)

# if this option is set, the build will exit after configuring special package configuration files
option(SOL_CONFIGURE_ONLY "Configure special files only, then exit." OFF)

option(SOL_ENABLE_TRAY "Enable system tray icon." ON)

if(WIN32)
    option(SOL_USE_STATIC_QT
            "Require static Qt libraries and their static third-party dependencies." ON)
endif()

option(SOL_SYSTEM_VULKAN_HEADERS "Use system installation of vulkan-headers rather than the submodule." OFF)
option(SOL_SYSTEM_WAYLAND_PROTOCOLS "Use system installation of wayland-protocols rather than the submodule." OFF)

if(APPLE)
    option(BOOST_USE_STATIC "Use static boost libraries." OFF)
else()
    option(BOOST_USE_STATIC "Use static boost libraries." ON)
endif()

option(CUDA_FAIL_ON_MISSING "Fail the build if CUDA is not found." ON)
option(CUDA_INHERIT_COMPILE_OPTIONS
        "When building CUDA code, inherit compile options from the the main project. You may want to disable this if
        your IDE throws errors about unknown flags after running cmake." ON)

if(UNIX)
    option(SOL_BUILD_HOMEBREW
            "Enable a Homebrew build." OFF)
    option(SOL_CONFIGURE_HOMEBREW
            "Configure Homebrew formula. Recommended to use with SOL_CONFIGURE_ONLY" OFF)
endif()

if(APPLE)
    option(SOL_CONFIGURE_PORTFILE
            "Configure macOS Portfile. Recommended to use with SOL_CONFIGURE_ONLY" OFF)
elseif(UNIX)  # Linux
    option(SOL_BUILD_APPIMAGE
            "Enable an AppImage build." OFF)
    option(SOL_BUILD_FLATPAK
            "Enable a Flatpak build." OFF)
    option(SOL_CONFIGURE_PKGBUILD
            "Configure files required for AUR. Recommended to use with SOL_CONFIGURE_ONLY" OFF)
    option(SOL_CONFIGURE_FLATPAK_MAN
            "Configure manifest file required for Flatpak build. Recommended to use with SOL_CONFIGURE_ONLY" OFF)

    # Linux capture methods
    option(SOL_ENABLE_CUDA
            "Enable cuda specific code." ON)
    option(SOL_ENABLE_DRM
            "Enable KMS grab if available." ON)
    option(SOL_ENABLE_VAAPI
            "Enable building vaapi specific code." ON)
    option(SOL_ENABLE_VULKAN
            "Enable Vulkan video encoding." ON)
    option(SOL_ENABLE_WAYLAND
            "Enable building wayland specific code." ON)
    option(SOL_ENABLE_X11
            "Enable X11 grab if available." ON)
    option(SOL_ENABLE_KWIN
            "Enable KWin ScreenCast grab if available" ON)
    option(SOL_ENABLE_PORTAL
            "Enable XDG portal grab if available" ON)
endif()
