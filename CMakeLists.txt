###############################################################################
# Dullahan CMake file - Callum Prentice - 2020-05-03

###############################################################################
cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

###############################################################################
# Dullahan main project/solution
project(dullahan)

option(USE_SANDBOX "Enable Sandbox Support" OFF)

###############################################################################
#  Determine platform
if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(IS_WINDOWS TRUE)

    set(DULLAHAN_HELPER_TARGET "dullahan_host")
    set(DULLAHAN_HELPER_OUTPUT_NAME "DullahanHost")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(IS_MACOS TRUE)

    # Standard libraries.
    set(DULLAHAN_STANDARD_LIBS
        -lpthread
        "-framework Cocoa"
        "-framework AppKit"
        )
    
    # CEF Helper app suffixes.
    # Format is "<name suffix>:<target suffix>:<plist suffix>".
    set(DULLAHAN_HELPER_APP_SUFFIXES
        "::"
        " (GPU):_gpu:.gpu"
        " (Plugin):_plugin:.plugin"
        " (Renderer):_renderer:.renderer"
        )

    set(DULLAHAN_HELPER_TARGET "dullahan_host")
    set(DULLAHAN_HELPER_OUTPUT_NAME "DullahanHost")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(IS_LINUX TRUE)

    set(CMAKE_SKIP_RPATH TRUE)

    set(DULLAHAN_HELPER_TARGET "dullahan_host")
    set(DULLAHAN_HELPER_OUTPUT_NAME "DullahanHost")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

###############################################################################
# functions
function(check_exists variable)
    if(NOT ${variable})
        message(FATAL_ERROR ${variable} " is not set")
    else()
        message(STATUS "--> ${variable} is ${${variable}}")
    endif()
endfunction()

###############################################################################
# The user must pass in CEF_WRAPPER_DIR and CEF_WRAPPER_BUILD_DIR then we
# derrive all the other ones we need based on those
set(CEF_INCLUDE_DIR ${CEF_WRAPPER_DIR}/include)
set(CEF_DEBUG_LIB_DIR ${CEF_WRAPPER_DIR}/Debug)
set(CEF_DEBUG_DLL_LIB_DIR ${CEF_WRAPPER_BUILD_DIR}/libcef_dll_wrapper/Debug)
set(CEF_RELEASE_LIB_DIR ${CEF_WRAPPER_DIR}/Release)
set(CEF_RELEASE_DLL_LIB_DIR ${CEF_WRAPPER_BUILD_DIR}/libcef_dll_wrapper/Release)
set(CEF_DEBUG_BIN_DIR ${CEF_WRAPPER_DIR}/Debug)
set(CEF_RELEASE_BIN_DIR ${CEF_WRAPPER_DIR}/Release)
set(CEF_RESOURCES_DIR ${CEF_WRAPPER_DIR}/Resources)

# Check if all our variables exist and bail with
# a fatal error if any of them are missing
check_exists(CEF_WRAPPER_DIR)
check_exists(CEF_WRAPPER_BUILD_DIR)
check_exists(CEF_INCLUDE_DIR)
check_exists(CEF_DEBUG_LIB_DIR)
check_exists(CEF_DEBUG_DLL_LIB_DIR)
check_exists(CEF_RELEASE_LIB_DIR)
check_exists(CEF_RELEASE_DLL_LIB_DIR)
check_exists(CEF_DEBUG_BIN_DIR)
check_exists(CEF_RELEASE_BIN_DIR)
check_exists(CEF_RESOURCES_DIR)

###############################################################################
# location of CEF libraries we link against
if(IS_WINDOWS)
    # Find both link libraries we need for Debug/Release
    find_library(
        CEF_LIBRARY_DEBUG
        NAMES libcef.lib
        PATHS ${CEF_DEBUG_LIB_DIR}
        NO_DEFAULT_PATH
    )

    find_library(
        CEF_DLL_LIBRARY_DEBUG
        NAMES libcef_dll_wrapper.lib
        PATHS ${CEF_DEBUG_DLL_LIB_DIR}
        NO_DEFAULT_PATH
    )

    find_library(
        CEF_LIBRARY_RELEASE
        NAMES libcef.lib
        PATHS ${CEF_RELEASE_LIB_DIR}
        NO_DEFAULT_PATH
    )

    find_library(
        CEF_DLL_LIBRARY_RELEASE
        NAMES libcef_dll_wrapper.lib
        PATHS ${CEF_RELEASE_DLL_LIB_DIR}
        NO_DEFAULT_PATH
    )

    # Confirm that we were able to find our link libs
    check_exists(CEF_LIBRARY_DEBUG)
    check_exists(CEF_DLL_LIBRARY_DEBUG)
    check_exists(CEF_LIBRARY_RELEASE)
    check_exists(CEF_DLL_LIBRARY_RELEASE)
elseif(IS_MACOS)
    find_library(
        CEF_DLL_LIBRARY_DEBUG
        NAMES libcef_dll_wrapper.a
        PATHS ${CEF_DEBUG_DLL_LIB_DIR}
    )

    find_library(
        CEF_DLL_LIBRARY_RELEASE
        NAMES libcef_dll_wrapper.a
        PATHS ${CEF_RELEASE_DLL_LIB_DIR}
    )

    find_library(
        CEF_FRAMEWORK_DEBUG
        NAMES "Chromium Embedded Framework"
        PATHS ${CEF_DEBUG_BIN_DIR}
    )

    find_library(
        CEF_FRAMEWORK_RELEASE
        NAMES "Chromium Embedded Framework"
        PATHS ${CEF_RELEASE_BIN_DIR}
    )

    set(CEF_FRAMEWORK
        debug ${CEF_FRAMEWORK_DEBUG}
        optimized ${CEF_FRAMEWORK_RELEASE}
    )

    find_library(
        CEF_SANDBOX_DEBUG
        NAMES cef_sandbox.a
        PATHS ${CEF_DEBUG_BIN_DIR}
    )

    find_library(
        CEF_SANDBOX_RELEASE
        NAMES cef_sandbox.a
        PATHS ${CEF_RELEASE_BIN_DIR}
    )

    set(CEF_SANDBOX
        debug ${CEF_SANDBOX_DEBUG}
        optimized ${CEF_SANDBOX_RELEASE}
    )

    find_library(OPENGL_FRAMEWORK OpenGL)
    find_library(COCOA_FRAMEWORK Cocoa)

    # Check that we were able to find our build components
    check_exists(CEF_DLL_LIBRARY_DEBUG)
    check_exists(CEF_DLL_LIBRARY_RELEASE)
    check_exists(CEF_FRAMEWORK_DEBUG)
    check_exists(CEF_FRAMEWORK_RELEASE)
    check_exists(CEF_FRAMEWORK)
    check_exists(CEF_SANDBOX_DEBUG)
    check_exists(CEF_SANDBOX_RELEASE)
    check_exists(CEF_SANDBOX)
    check_exists(OPENGL_FRAMEWORK)
    check_exists(COCOA_FRAMEWORK)
elseif(IS_LINUX)
    set(CMAKE_CXX_LINK_FLAGS "-Wl,--no-keep-memory -Wl,--build-id -Wl,--exclude-libs,ALL")
    find_library(
        CEF_LIBRARY_RELEASE
        NAMES libcef.so
        PATHS ${CEF_RELEASE_LIB_DIR}
        PATH_SUFFIXES release
    )
    find_library(
        CEF_DLL_LIBRARY_RELEASE
        NAMES libcef_dll_wrapper.a
        PATHS ${CEF_WRAPPER_BUILD_DIR}/libcef_dll_wrapper/
        PATH_SUFFIXES release
    )
endif()

###############################################################################
# Final layer of finding the right libs for each combination
# of name, platform, configuration, type etc.
if(IS_LINUX)
  set(CEF_LIBRARY ${CEF_LIBRARY_RELEASE} )
  set(CEF_DLL_LIBRARY ${CEF_DLL_LIBRARY_RELEASE} )
else()
  set(CEF_LIBRARY
      debug ${CEF_LIBRARY_DEBUG}
      optimized ${CEF_LIBRARY_RELEASE}
  )
  set(CEF_DLL_LIBRARY
      debug ${CEF_DLL_LIBRARY_DEBUG}
      optimized ${CEF_DLL_LIBRARY_RELEASE}
  )
endif()

check_exists(CEF_LIBRARY)
check_exists(CEF_DLL_LIBRARY)

###############################################################################
# set C and C++ flags
# Warnings at level 4 (-W4 generates too much spew) but disable:
#    4100 "unreferenced parameter" -  too much spew for cef code
#    4127 "conditional is constant" - I use an explicity var to turn code on and off which triggers this
#    4505 "unreferenced local function has been removed" - supress meaningless freeglut warning
if(IS_WINDOWS)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -W4 -wd4100 -wd4127 -wd4505")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -W4 -wd4100 -wd4127 -wd4505")
elseif(IS_MACOS)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -xobjective-c++")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -xobjective-c++")
elseif(IS_LINUX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()

###############################################################################
# Dullahan libary

# add source files to library
if(IS_WINDOWS)
    set(KEYBOARD_IMPL_SRC_FILE src/dullahan_impl_keyboard_win.cpp)
elseif(IS_MACOS)
    set(KEYBOARD_IMPL_SRC_FILE src/dullahan_impl_keyboard_mac.mm)
elseif(IS_LINUX)
    set(KEYBOARD_IMPL_SRC_FILE src/dullahan_impl_keyboard_linux.cpp)
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
    src/dullahan_version.h
    src/dullahan_version.h.in
    ${KEYBOARD_IMPL_SRC_FILE}
    src/dullahan_impl_mouse.cpp
    src/dullahan_render_handler.cpp
    src/dullahan_render_handler.h
)
set_target_properties(dullahan PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# define which include directories to pull in
target_include_directories(
    dullahan
    PUBLIC
    ${CEF_INCLUDE_DIR}
    ${CEF_INCLUDE_DIR}/..
)

# turn off spurious linker warnings
if(IS_WINDOWS)
    set_target_properties(dullahan PROPERTIES LINK_FLAGS "/ignore:4099")
endif()

if(USE_SANDBOX)
    target_compile_definitions(dullahan PUBLIC USE_SANDBOX=1)
endif()

###############################################################################
# Parse CEF version header and process into the Dullahan header file.
# Only do this if the version input template changed to try to keep
# version/build number the same for the same set of source files)
if("${PROJECT_SOURCE_DIR}/src/dullahan_version.h.in" IS_NEWER_THAN "${PROJECT_SOURCE_DIR}/src/dullahan_version.h")

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
        "${PROJECT_SOURCE_DIR}/src/dullahan_version.h.in"
        "${PROJECT_SOURCE_DIR}/src/dullahan_version.h"
    )
endif()

###############################################################################
# Dullahan host executable

# add source files to the application
if(IS_WINDOWS)
    add_executable(
        ${DULLAHAN_HELPER_TARGET}
        WIN32
        src/host/dullahan_host.cpp
        src/win/compatibility.manifest
    )

    # define which include directories to pull in
    target_include_directories(
        ${DULLAHAN_HELPER_TARGET}
        PUBLIC
        ${CEF_INCLUDE_DIR}
        ${CEF_INCLUDE_DIR}/..
    )

    target_link_libraries(
        ${DULLAHAN_HELPER_TARGET}
        ${CEF_LIBRARY}
        ${CEF_DLL_LIBRARY}
    )
elseif(IS_MACOS)
    # Create the multiple Helper app bundle targets.
    foreach(_suffix_list ${DULLAHAN_HELPER_APP_SUFFIXES})
        # Convert to a list and extract the suffix values.
        string(REPLACE ":" ";" _suffix_list ${_suffix_list})
        list(GET _suffix_list 0 _name_suffix)
        list(GET _suffix_list 1 _target_suffix)
        list(GET _suffix_list 2 _plist_suffix)

        # Define Helper target and output names.
        set(_helper_target "${DULLAHAN_HELPER_TARGET}${_target_suffix}")
        set(_helper_output_name "${DULLAHAN_HELPER_OUTPUT_NAME}${_name_suffix}")

        # Create Helper-specific variants of the helper-Info.plist file. Do this
        # manually because the configure_file command (which is executed as part of
        # MACOSX_BUNDLE_INFO_PLIST) uses global env variables and would insert the
        # wrong values with multiple targets.
        set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
        file(READ "${PROJECT_SOURCE_DIR}/src/host/Info.plist.in" _plist_contents)
        string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
        file(WRITE ${_helper_info_plist} ${_plist_contents})

        # Create Helper executable target.
        add_executable(${_helper_target} MACOSX_BUNDLE src/host/dullahan_host.cpp)

        # define which include directories to pull in
        target_include_directories(
            ${_helper_target}
            PUBLIC
            ${CEF_INCLUDE_DIR}
            ${CEF_INCLUDE_DIR}/..
        )
        target_link_libraries(${_helper_target} ${CEF_DLL_LIBRARY} ${DULLAHAN_STANDARD_LIBS})
        set_target_properties(${_helper_target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${_helper_info_plist}
        OUTPUT_NAME ${_helper_output_name}
        )

        if(USE_SANDBOX)
            target_compile_definitions(${_helper_target} PUBLIC USE_SANDBOX=1)
            target_link_libraries(${_helper_target} ${CEF_SANDBOX})
        endif()
    endforeach()
elseif(IS_LINUX)
    add_executable(
        ${DULLAHAN_HELPER_TARGET}
        src/host/dullahan_host.cpp
    )
    target_include_directories(
        ${DULLAHAN_HELPER_TARGET}
        PUBLIC
        ${CEF_INCLUDE_DIR}
        ${CEF_INCLUDE_DIR}/..
    )
    target_link_libraries(
        ${DULLAHAN_HELPER_TARGET}
        ${CEF_DLL_LIBRARY}
        ${CEF_LIBRARY}
    )
endif()

# Windows commands to copy CEF binaries and 'resources' folders to 
# executable dir since they're needed at runtime
if(IS_WINDOWS)
    add_custom_command(
        TARGET ${DULLAHAN_HELPER_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<$<CONFIG:debug>:${CEF_DEBUG_BIN_DIR}>$<$<CONFIG:release>:${CEF_RELEASE_BIN_DIR}>"
                "$<TARGET_FILE_DIR:${DULLAHAN_HELPER_TARGET}>"
        COMMENT "Copying runtime files to executable directory")

    add_custom_command(
        TARGET ${DULLAHAN_HELPER_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CEF_RESOURCES_DIR}"
                "$<TARGET_FILE_DIR:${DULLAHAN_HELPER_TARGET}>"
        COMMENT "Copying resource files to executable directory")
endif()

###############################################################################
# examples
if(IS_WINDOWS)
    ###############################################################################
    # webcube example application

    # add source file to application
    add_executable(
        webcube
        WIN32
        examples/webcube/webcube.cpp
        examples/webcube/webcube.h
        examples/webcube/webcube.rc
        examples/webcube/resource.h
    )

    # define which include directories to pull in
    target_include_directories(
        webcube
        PUBLIC
        src
    )

    # define which libs to link against
    target_link_libraries(
        webcube
        dullahan
        ${CEF_LIBRARY}
        ${CEF_DLL_LIBRARY}
        opengl32
        glu32
        comctl32
        winmm
    )

    # Supress Pointless Warnings
    set_target_properties(webcube PROPERTIES LINK_FLAGS "/ignore:4099")

    # webcube example dependes on main library and host executable
    add_dependencies(webcube ${DULLAHAN_HELPER_TARGET})

    # set the web cube example as the default startup project in Visual Studio
    if("${CMAKE_VERSION}" VERSION_GREATER 3.6.2)
        if(IS_WINDOWS)
            SET_PROPERTY(DIRECTORY PROPERTY VS_STARTUP_PROJECT "webcube")
        endif()
    endif()

    add_custom_command(
        TARGET webcube POST_BUILD
        COMMAND "mt.exe" -manifest \"$(ProjectDir)..\\src\\win\\compatibility.manifest\" -outputresource:"$(TargetDir)$(TargetFileName)"\;\#1
        COMMENT "Adding custom manifest...")

    ###############################################################################
    # simplegl example application using FreeGLUT

    # add source file to application
    add_executable(
        simplegl
        examples/simplegl/simplegl.cpp
    )

    # define which include directories to pull in
    target_include_directories(
        simplegl
        PUBLIC
        src
        examples/simplegl/freeglut/include/GL
    )

    # determine location of FreeGLUT library to link against based on bitwidth
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(FREEGLUT_LIBRARY "${PROJECT_SOURCE_DIR}/examples/simplegl/freeglut/lib/x64/freeglut.lib")
        set(FREEGLUT_DLL "${PROJECT_SOURCE_DIR}/examples/simplegl/freeglut/bin/x64/freeglut.dll")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(FREEGLUT_LIBRARY "${PROJECT_SOURCE_DIR}/examples/simplegl/freeglut/lib/freeglut.lib")
        set(FREEGLUT_DLL "${PROJECT_SOURCE_DIR}/examples/simplegl/freeglut/bin/freeglut.dll")
    endif()

    # copy over freeglut.dll
    add_custom_command(
        TARGET simplegl POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${FREEGLUT_DLL}"
                "$<TARGET_FILE_DIR:simplegl>"
       COMMENT "Copying FreeGLUT DLL to executable directory")

    # define which libs to link against
    target_link_libraries(
        simplegl
        dullahan
        ${CEF_LIBRARY}
        ${CEF_DLL_LIBRARY}
        ${FREEGLUT_LIBRARY}
    )

    # turn off spurious linker warnings
    set_target_properties(simplegl PROPERTIES LINK_FLAGS "/ignore:4099")

    add_custom_command(
        TARGET simplegl POST_BUILD
        COMMAND "mt.exe" -manifest \"$(ProjectDir)..\\src\\win\\compatibility.manifest\" -outputresource:"$(TargetDir)$(TargetFileName)"\;\#1
        COMMENT "Adding custom manifest...")

    ###############################################################################
    # console example application

    # add source file to application
    add_executable(
        console
        examples/console/console.cpp
    )

    # define which include directories to pull in
    target_include_directories(
        console
        PUBLIC
        src
    )

    # define which libs to link against
    target_link_libraries(
        console
        dullahan
        ${CEF_LIBRARY}
        ${CEF_DLL_LIBRARY}
    )

    # turn off spurious linker warnings
    set_target_properties(console PROPERTIES LINK_FLAGS "/ignore:4099")

    add_custom_command(
        TARGET console POST_BUILD
        COMMAND "mt.exe" -manifest \"$(ProjectDir)..\\src\\win\\compatibility.manifest\" -outputresource:"$(TargetDir)$(TargetFileName)"\;\#1
        COMMENT "Adding custom manifest...")

    ###############################################################################
    # minimal CEF console example (doesn't use Dullahan)

    # add source file to the application
    add_executable(
        cef_minimal
        examples/cef_minimal/cef_minimal.cpp
    )

    # define which include directories to pull in
    target_include_directories(
        cef_minimal
        PUBLIC
        ${CEF_INCLUDE_DIR}
        ${CEF_INCLUDE_DIR}/..
    )

    # define which libs to link against
    target_link_libraries(
        cef_minimal
        ${CEF_LIBRARY}
        ${CEF_DLL_LIBRARY}
    )

    # cef_minimal example dependes on host executable
    add_dependencies(cef_minimal ${DULLAHAN_HELPER_TARGET})

    # turn off spurious linker warnings
    set_target_properties(cef_minimal PROPERTIES LINK_FLAGS "/ignore:4099")

    add_custom_command(
        TARGET cef_minimal POST_BUILD
        COMMAND "mt.exe" -manifest \"$(ProjectDir)..\\src\\win\\compatibility.manifest\" -outputresource:"$(TargetDir)$(TargetFileName)"\;\#1
        COMMENT "Adding custom manifest...")

elseif(IS_MACOS)
    ###############################################################################
    # osxgl example application

    # add source file to application
    add_executable(
        osxgl
        MACOSX_BUNDLE
        examples/osxgl/AppDelegate.h
        examples/osxgl/AppDelegate.mm
        examples/osxgl/LLOsxglView.h
        examples/osxgl/LLOsxglView.mm
        examples/osxgl/main.m
    )

    # define which include directories to pull in
    target_include_directories(
        osxgl
        PUBLIC
        src
    )

    # define which libs to link against
    target_link_libraries(
        osxgl
        dullahan
        ${CEF_DLL_LIBRARY}
        ${OPENGL_FRAMEWORK}
        ${COCOA_FRAMEWORK}
    )

    add_dependencies(osxgl dullahan)
    #add_dependencies(osxgl DullahanHelper)

    # default Inof.plist.in template in CMake doesn't contain
    # the NSPrincipalClass definition so we must add it
    set(PRINCIPAL_CLASS "NSApplication")
    set_target_properties(osxgl PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "OSXGL Test"
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/examples/osxgl/Info.plist.in")
endif()

###############################################################################
# generic commands that have to go after everything else

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
endif()
