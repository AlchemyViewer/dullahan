###############################################################################
# Alchemy Dullahan CMake build configuration

include(CEFPlugin)

# add source files to library
if(WINDOWS)
    set(PLATFORM_IMPL_SRC_FILE src/dullahan_impl_windows.cpp)
    set(KEYBOARD_IMPL_SRC_FILE src/dullahan_impl_keyboard_win.cpp)
elseif(DARWIN)
    # dullahan_impl.cpp and dullahan_runtime.cpp both use Cocoa (NSBundle) in
    # their macOS paths, so compile them as Objective-C++.
    set_source_files_properties(src/dullahan_impl.cpp src/dullahan_runtime.cpp PROPERTIES LANGUAGE OBJCXX)
    set(PLATFORM_IMPL_SRC_FILE src/dullahan_impl_mac.mm)
    set(KEYBOARD_IMPL_SRC_FILE src/dullahan_impl_keyboard_mac.mm)
elseif(LINUX)
    set(PLATFORM_IMPL_SRC_FILE src/dullahan_impl_linux.cpp)
    set(KEYBOARD_IMPL_SRC_FILE)
endif()

add_library(
    dullahan
    STATIC
    src/dullahan.cpp
    src/dullahan.h
    src/dullahan_browser_client.cpp
    src/dullahan_browser_client.h
    src/dullahan_callback_manager.cpp
    src/dullahan_callback_manager.h
    src/dullahan_debug.h
    src/dullahan_impl.cpp
    src/dullahan_impl.h
    src/dullahan_runtime.cpp
    src/dullahan_runtime.h
    src/dullahan_version.h
    src/dullahan_version.h.in
    ${PLATFORM_IMPL_SRC_FILE}
    ${KEYBOARD_IMPL_SRC_FILE}
    src/dullahan_impl_keyboard_sdl.cpp
    src/dullahan_impl_mouse.cpp
    src/dullahan_render_handler.cpp
    src/dullahan_render_handler.h
)

target_link_libraries(dullahan PUBLIC ll::cef)
target_include_directories(dullahan PUBLIC src)

###############################################################################
# Parse CEF version header and process into the Dullahan header file.
# Only do this if the version input template changed to try to keep
# version/build number the same for the same set of source files)
if("${CMAKE_CURRENT_SOURCE_DIR}/src/dullahan_version.h.in" IS_NEWER_THAN "${CMAKE_CURRENT_SOURCE_DIR}/src/dullahan_version.h")

    # Extract CEF/Chrome version info from CEF header
    file(STRINGS ${CEF_INCLUDE_DIR}/cef_version.h CEF_VERSION_STR REGEX "\#define CEF_VERSION ")
    file(STRINGS ${CEF_INCLUDE_DIR}/cef_version.h CHROME_VERSION_MAJOR_STR REGEX "\#define CHROME_VERSION_MAJOR ")
    file(STRINGS ${CEF_INCLUDE_DIR}/cef_version.h CHROME_VERSION_MINOR_STR REGEX "\#define CHROME_VERSION_MINOR ")
    file(STRINGS ${CEF_INCLUDE_DIR}/cef_version.h CHROME_VERSION_BUILD_STR REGEX "\#define CHROME_VERSION_BUILD ")
    file(STRINGS ${CEF_INCLUDE_DIR}/cef_version.h CHROME_VERSION_PATCH_STR REGEX "\#define CHROME_VERSION_PATCH ")

    # Create a build number based on the actual build date/time
    string(TIMESTAMP BUILDNUMBER "#define DULLAHAN_VERSION_BUILD %Y%m%d%H%M")

    # Parse the version input file into the Dullahan version header
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dullahan_version.h.in"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dullahan_version.h"
    )
endif()

###############################################################################
# Dullahan host executable

# add source files to the application
if(WINDOWS)
    add_executable(
        dullahan_host
        WIN32
        src/host/dullahan_host.cpp
        src/compatibility.manifest
    )

    set_target_properties(dullahan_host PROPERTIES LINK_FLAGS "/ignore:4099")

    target_link_libraries(
        dullahan_host
        ll::cef
    )
elseif(DARWIN)
    # Below based on official CEF example code

    # CEF Helper app suffixes.
    # Format is "<name suffix>:<target suffix>:<plist suffix>".
    set(DULLAHAN_HELPER_APP_SUFFIXES
        "::"
        " (Alerts):_alerts:.alerts"
        " (GPU):_gpu:.gpu"
        " (Plugin):_plugin:.plugin"
        " (Renderer):_renderer:.renderer"
        )

    # Create the multiple Helper app bundle targets.
    foreach(_suffix_list ${DULLAHAN_HELPER_APP_SUFFIXES})
        # Convert to a list and extract the suffix values.
        string(REPLACE ":" ";" _suffix_list ${_suffix_list})
        list(GET _suffix_list 0 _name_suffix)
        list(GET _suffix_list 1 _target_suffix)
        list(GET _suffix_list 2 _plist_suffix)

        # Define Helper target and output names.
        set(_helper_target "dullahan_host${_target_suffix}")
        set(_helper_output_name "DullahanHelper${_name_suffix}")

        # Create Helper-specific variants of the helper-Info.plist file. Do this
        # manually because the configure_file command (which is executed as part of
        # MACOSX_BUNDLE_INFO_PLIST) uses global env variables and would insert the
        # wrong values with multiple targets.
        set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/host/Info.plist.in" _plist_contents)
        string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
        file(WRITE ${_helper_info_plist} ${_plist_contents})

        # Create Helper executable target.
        add_executable(${_helper_target} MACOSX_BUNDLE src/host/dullahan_host.cpp)

        target_link_libraries(${_helper_target} PRIVATE ll::cef)
        set_target_properties(${_helper_target} PROPERTIES
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.achemyviewer.dullahanhost${_plist_suffix}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.achemyviewer.dullahanhost${_plist_suffix}"
            MACOSX_BUNDLE_EXECUTABLE_NAME "${_helper_output_name}"
            MACOSX_BUNDLE_BUNDLE_NAME "${_helper_output_name}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${DULLAHAN_VERSION_MAJOR}.${DULLAHAN_VERSION_MINOR}.${DULLAHAN_VERSION_PATCH}"
            MACOSX_BUNDLE_INFO_STRING "Dullahan Helper${_name_suffix}"
            MACOSX_BUNDLE_INFO_PLIST "${_helper_info_plist}"
            OUTPUT_NAME "${_helper_output_name}"
        )
    endforeach()
elseif(LINUX)
    add_executable(
        dullahan_host
        src/host/dullahan_host.cpp
    )

    target_link_libraries(
        dullahan_host
        ll::cef
    )

    # dullahan_host is staged to llplugin/ by the viewer; libcef.so is in
    # the adjacent lib/ directory. Set rpath so the dynamic linker finds it
    # when CEF spawns dullahan_host as a subprocess.
    target_link_options(dullahan_host PRIVATE
        "LINKER:-rpath,$ORIGIN/../lib"
    )
endif()

# Windows commands to copy CEF binaries and 'resources' folders to
# executable dir since they're needed at runtime

if(WINDOWS)
    # Copy plugin files to packaging directory
    set(CEF_BIN_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/cef-bin")
    add_custom_command(
        TARGET dullahan_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_BIN_DIR}/Release" "$<TARGET_FILE_DIR:dullahan_host>"
    )

    set(CEF_RESOURCE_FILES
        "${CEF_BIN_DIR}/Resources/chrome_100_percent.pak"
        "${CEF_BIN_DIR}/Resources/chrome_200_percent.pak"
        "${CEF_BIN_DIR}/Resources/resources.pak"
        "${CEF_BIN_DIR}/Resources/icudtl.dat"
    )
    add_custom_command(
        TARGET dullahan_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CEF_RESOURCE_FILES} "$<TARGET_FILE_DIR:dullahan_host>"
        COMMAND_EXPAND_LISTS
    )

    add_custom_command(
        TARGET dullahan_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_BIN_DIR}/Resources/locales" "$<TARGET_FILE_DIR:dullahan_host>/locales"
    )
endif()

###############################################################################
# Examples
if (BUILD_DULLAHAN_EXAMPLE)
    # Dear ImGui (with the SDL3 + OpenGL2 backends) is provided by vcpkg.
    find_package(imgui CONFIG REQUIRED)

    # SDL3 provides cross-platform windowing, input and GL context creation.
    # ll::SDL3 is defined by the parent build (indra/cmake/SDL3.cmake) before this
    # submodule is added; fall back to the vcpkg package for standalone builds.
    if(NOT TARGET ll::SDL3)
        find_package(SDL3 CONFIG REQUIRED)
        add_library(ll::SDL3 INTERFACE IMPORTED)
        target_link_libraries(ll::SDL3 INTERFACE SDL3::SDL3)
    endif()

    ###############################################################################
    # OpenGL example application

    set(OPENGL_EXAMPLE_PLATFORM_SRC "")
    if(DARWIN)
        # CEF/NSApplication compatibility shim (CefAppProtocol selectors)
        set(OPENGL_EXAMPLE_PLATFORM_SRC examples/opengl-example/src/opengl-example-mac.mm)
    endif()

    # On Windows the example is a DLL (RunWinMain) loaded by CEF's bootstrap
    # executable for the sandbox; elsewhere it is a plain executable.
    if(WINDOWS)
        add_library(
            opengl-example
            MODULE
            examples/opengl-example/src/opengl-example.cpp
            examples/opengl-example/src/opengl-example.h
            examples/opengl-example/src/opengl-example-accelpaint.cpp
            examples/opengl-example/src/opengl-example-accelpaint.h
            ${OPENGL_EXAMPLE_PLATFORM_SRC}
            examples/opengl-example/README.md
        )
    else()
        add_executable(
            opengl-example
            MACOSX_BUNDLE
            examples/opengl-example/src/opengl-example.cpp
            examples/opengl-example/src/opengl-example.h
            examples/opengl-example/src/opengl-example-accelpaint.cpp
            examples/opengl-example/src/opengl-example-accelpaint.h
            ${OPENGL_EXAMPLE_PLATFORM_SRC}
            examples/opengl-example/README.md
            src/compatibility.manifest
        )
    endif()

    target_include_directories(
        opengl-example
        PUBLIC
        src
        ${THIRD-PARTY-PREFIX}/${GLAD_FOLDER}/include
    )

    if(WINDOWS)
        find_package(glad CONFIG REQUIRED)
        target_link_libraries (
            opengl-example
            dullahan
            opengl32
            ll::SDL3
            imgui::imgui
            glad::glad
            d3d11      # zero-copy paint: D3D11 + WGL_NV_DX_interop2
            dxgi
        )
        SET_PROPERTY(DIRECTORY PROPERTY VS_STARTUP_PROJECT "opengl-example")
    elseif(DARWIN)
        find_library (OPENGL_FRAMEWORK OpenGL)
        # zero-copy paint: IOSurface (CGLTexImageIOSurface2D) lives in the OpenGL
        # framework; the IOSurfaceRef type comes from the IOSurface framework.
        find_library (IOSURFACE_FRAMEWORK IOSurface)
        target_link_libraries (
            opengl-example
            dullahan
            ll::SDL3
            imgui::imgui
            ${OPENGL_FRAMEWORK}
            ${IOSURFACE_FRAMEWORK}
        )
        set(EXAMPLE_FRAMEWORKS
            "${CEF_FRAMEWORK_DIR}"
            dullahan_host
            dullahan_host_alerts
            dullahan_host_gpu
            dullahan_host_renderer
            dullahan_host_plugin
        )
        set_target_properties(opengl-example
                            PROPERTIES
                            XCODE_EMBED_FRAMEWORKS "${EXAMPLE_FRAMEWORKS}")
    elseif(LINUX)
        find_package(OpenGL REQUIRED)
        find_package(glad CONFIG REQUIRED)
        target_link_libraries (
            opengl-example
            dullahan
            ll::SDL3
            imgui::imgui
            glad::glad
            OpenGL::GL
            ${CMAKE_DL_LIBS}
        )

        # Stage the CEF runtime next to the example so it runs standalone: CEF
        # finds its resources (.pak / icudtl.dat / locales) and GL libs
        # (libEGL/GLESv2/swiftshader) beside the executable. Mirrors the Windows
        # staging above. The accelerated (dma-buf) paint path additionally needs an
        # EGL context, so run under Wayland or with SDL_VIDEO_FORCE_EGL=1.
        set(CEF_PKG_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        add_custom_command(TARGET opengl-example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_PKG_DIR}/lib/libcef.so" "$<TARGET_FILE_DIR:opengl-example>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_PKG_DIR}/share/cef-bin/Release" "$<TARGET_FILE_DIR:opengl-example>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_PKG_DIR}/share/cef-bin/Resources" "$<TARGET_FILE_DIR:opengl-example>"
        )

        # The viewer build strips rpath (CMAKE_SKIP_RPATH), so generate a tiny
        # wrapper that puts libcef.so + SDL3 on the library path and runs the
        # example: `./run-opengl-example.sh` (pass DULLAHAN_* env vars as usual).
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:opengl-example>/run-opengl-example.sh"
            CONTENT "#!/bin/sh
here=\"$(cd \"$(dirname \"$0\")\" && pwd)\"
export LD_LIBRARY_PATH=\"$here:${CEF_PKG_DIR}/lib:$LD_LIBRARY_PATH\"
exec \"$here/opengl-example\" \"$@\"
"
            FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endif()

    add_dependencies(opengl-example dullahan)
    if(WINDOWS OR LINUX)
        add_dependencies(opengl-example dullahan_host)
    elseif(DARWIN)
        add_dependencies(opengl-example dullahan_host dullahan_host_alerts dullahan_host_gpu dullahan_host_renderer dullahan_host_plugin)
    endif()

    if(WINDOWS)
        # Ship CEF's bootstrap.exe renamed to opengl-example.exe; matching base
        # name makes it load opengl-example.dll and call our RunWinMain with the
        # Windows sandbox info.
        add_custom_command(TARGET opengl-example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_BIN_DIR}/Release/bootstrap.exe" "$<TARGET_FILE_DIR:opengl-example>/opengl-example.exe"
        )
    endif()
endif()
