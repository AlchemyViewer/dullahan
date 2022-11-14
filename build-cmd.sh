#!/usr/bin/env bash

cd "$(dirname "$0")"

# turn on verbose debugging output for parabuild logs.
exec 4>&1; export BASH_XTRACEFD=4; set -x

# make errors fatal
set -e

# bleat on references to undefined shell variables
set -u

# Check autobuild is around or fail
if [ -z "$AUTOBUILD" ] ; then
    exit 1
fi

if [ "$OSTYPE" = "cygwin" ] ; then
    autobuild="$(cygpath -u $AUTOBUILD)"
else
    autobuild="$AUTOBUILD"
fi

top="$(pwd)"
stage="$(pwd)/stage"
dullahan_source_dir=$top/src

# The name of the directory that `autobuild install` drops the
# CEF package into - no way to share this name between CEF and
# Dullahan so they have to be kept in lock step manually
cef_no_wrapper_dir="$stage/packages/cef"

# The name of the directory that the libcef_dll_wrapper gets
# built in - we also need to refer to it to get the build result
# libraries for Dullahan so we must specify it exactly once here.
cef_no_wrapper_build_dir="$cef_no_wrapper_dir/build"

# load autobuild provided shell functions and variables
source_environment_tempfile="$stage/source_environment.sh"
"$autobuild" source_environment > "$source_environment_tempfile"
. "$source_environment_tempfile"

build=${AUTOBUILD_BUILD_ID:=0}

case "$AUTOBUILD_PLATFORM" in
    windows*)
        load_vsvars

        # We've observed some weird failures in which the PATH is too big to be
        # passed to a child process! When that gets munged, we start seeing errors
        # like failing to find the 'mt.exe' command. Thing is, by this point
        # in the script we've acquired a shocking number of duplicate entries.
        # Dedup the PATH using Python's OrderedDict, which preserves the order in
        # which you insert keys.
        # We find that some of the Visual Studio PATH entries appear both with and
        # without a trailing slash, which is pointless. Strip those off and dedup
        # what's left.
        # Pass the existing PATH as an explicit argument rather than reading it
        # from the environment to bypass the fact that cygwin implicitly converts
        # PATH to Windows form when running a native executable. Since we're
        # setting bash's PATH, leave everything in cygwin form. That means
        # splitting and rejoining on ':' rather than on os.pathsep, which on
        # Windows is ';'.
        # Use python -u, else the resulting PATH will end with a spurious '\r'.
        export PATH="$(python -u -c "import sys
from collections import OrderedDict
print(':'.join(OrderedDict((dir.rstrip('/'), 1) for dir in sys.argv[1].split(':'))))" "$PATH")"

        # build the CEF c->C++ wrapper "libcef_dll_wrapper"
        cd "$cef_no_wrapper_dir"
        rm -rf "$cef_no_wrapper_build_dir"
        mkdir -p "$cef_no_wrapper_build_dir"
        cd "$cef_no_wrapper_build_dir"
        cmake -G "$AUTOBUILD_WIN_CMAKE_GEN" -A "$AUTOBUILD_WIN_VSPLATFORM" -DCEF_RUNTIME_LIBRARY_FLAG=/MD -DUSE_SANDBOX=OFF ..
        cmake --build . --config Debug --target libcef_dll_wrapper
        cmake --build . --config Release --target libcef_dll_wrapper

        # generate the project files for Dullahan
        cd "$stage"
        cmake .. \
            -G "$AUTOBUILD_WIN_CMAKE_GEN" -A "$AUTOBUILD_WIN_VSPLATFORM" \
            -DCEF_WRAPPER_DIR="$(cygpath -w "$cef_no_wrapper_dir")" \
            -DCEF_WRAPPER_BUILD_DIR="$(cygpath -w "$cef_no_wrapper_build_dir")" 

        # build individual dullahan libraries but not examples
        cmake --build . --config Release --target dullahan
        cmake --build . --config Release --target dullahan_host

        # prepare the staging dirs
        cd ..
        mkdir -p "$stage/include/cef"
        mkdir -p "$stage/lib/release"
        mkdir -p "$stage/bin/release"
        mkdir -p "$stage/bin/release/swiftshader"
        mkdir -p "$stage/resources"
        mkdir -p "$stage/LICENSES"

        # Dullahan files
        cp "$dullahan_source_dir/dullahan.h" "$stage/include/cef/"
        cp "$dullahan_source_dir/dullahan_version.h" "$stage/include/cef/"
        cp "$stage/Release/dullahan.lib" "$stage/lib/release/"
        cp "$stage/Release/dullahan_host.exe" "$stage/bin/release/"

        # CEF libraries
        cp "$cef_no_wrapper_dir/Release/libcef.lib" "$stage/lib/release"
        cp "$cef_no_wrapper_build_dir/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib" "$stage/lib/release"

        # CEF run time binaries (copy individually except SwiftShader so it's
        # obvious when a file is removed and this part of the script fails)
        cp -R "$cef_no_wrapper_dir/Release/swiftshader/"* "$stage/bin/release/swiftshader/"
        cp "$cef_no_wrapper_dir/Release/chrome_elf.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/d3dcompiler_47.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libcef.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libEGL.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libGLESv2.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vk_swiftshader.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vulkan-1.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/snapshot_blob.bin" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/v8_context_snapshot.bin" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vk_swiftshader_icd.json" "$stage/bin/release/"

        # CEF resources
        cp -R "$cef_no_wrapper_dir/Resources/"* "$stage/resources/"

        # licenses
        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"

        # populate version_file (after CMake runs)
        cl \
            /Fo"$(cygpath -w "$stage/version.obj")" \
            /Fe"$(cygpath -w "$stage/version.exe")" \
            /I "$(cygpath -w "$cef_no_wrapper_dir/include/")"  \
            /I "$(cygpath -w "$top/src")"  \
            "$(cygpath -w "$top/tools/autobuild_version.cpp")"
        "$stage/version.exe" > "$stage/version.txt"
        rm "$stage"/version.{obj,exe}
    ;;
    darwin*)
        # Setup osx sdk platform
        SDKNAME="macosx"
        export SDKROOT=$(xcodebuild -version -sdk ${SDKNAME} Path)

        # Deploy Targets
        X86_DEPLOY=10.15
        ARM64_DEPLOY=11.0

        # Setup build flags
        ARCH_FLAGS_X86="-arch x86_64 -mmacosx-version-min=${X86_DEPLOY} -isysroot ${SDKROOT} -msse4.2"
        ARCH_FLAGS_ARM64="-arch arm64 -mmacosx-version-min=${ARM64_DEPLOY} -isysroot ${SDKROOT}"
        DEBUG_COMMON_FLAGS="-O0 -g -fPIC -DPIC"
        RELEASE_COMMON_FLAGS="-O3 -g -fPIC -DPIC -fstack-protector-strong"
        DEBUG_CFLAGS="$DEBUG_COMMON_FLAGS"
        RELEASE_CFLAGS="$RELEASE_COMMON_FLAGS"
        DEBUG_CXXFLAGS="$DEBUG_COMMON_FLAGS -std=c++17"
        RELEASE_CXXFLAGS="$RELEASE_COMMON_FLAGS -std=c++17"
        DEBUG_CPPFLAGS="-DPIC"
        RELEASE_CPPFLAGS="-DPIC"
        DEBUG_LDFLAGS="-Wl,-headerpad_max_install_names"
        RELEASE_LDFLAGS="-Wl,-headerpad_max_install_names"

        export MACOSX_DEPLOYMENT_TARGET=${X86_DEPLOY}

        # build the CEF c->C++ wrapper "libcef_dll_wrapper"
        mkdir -p "$cef_no_wrapper_dir/build_x86_64"
        pushd "$cef_no_wrapper_dir/build_x86_64"
            cmake ../x86_64/ -G Xcode -DPROJECT_ARCH="x86_64" \
                -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                -DCMAKE_OSX_SYSROOT=${SDKROOT} \

            cmake --build . --config Debug --target libcef_dll_wrapper
            cmake --build . --config Release --target libcef_dll_wrapper
        popd

        export MACOSX_DEPLOYMENT_TARGET=${ARM64_DEPLOY}

        mkdir -p "$cef_no_wrapper_dir/build_arm64"
        pushd "$cef_no_wrapper_dir/build_arm64"
            cmake ../arm64/ -G Xcode -DPROJECT_ARCH="arm64" \
                -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                -DCMAKE_OSX_SYSROOT=${SDKROOT} \

            cmake --build . --config Debug --target libcef_dll_wrapper
            cmake --build . --config Release --target libcef_dll_wrapper
        popd

        export MACOSX_DEPLOYMENT_TARGET=${X86_DEPLOY}

        # build Dullahan
        mkdir -p "$stage/build_x86_64"
        pushd "$stage/build_x86_64"
            cmake $top -G Xcode \
                -DCEF_WRAPPER_DIR="$cef_no_wrapper_dir/x86_64" \
                -DCEF_WRAPPER_BUILD_DIR="$cef_no_wrapper_dir/build_x86_64" \
                -DUSE_SANDBOX=OFF \
                -DCMAKE_C_FLAGS="$ARCH_FLAGS_X86 $RELEASE_CFLAGS" \
                -DCMAKE_CXX_FLAGS="$ARCH_FLAGS_X86 $RELEASE_CXXFLAGS" \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL="3" \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_FAST_MATH=NO \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS=YES \
                -DCMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT=dwarf-with-dsym \
                -DCMAKE_XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING=YES \
                -DCMAKE_XCODE_ATTRIBUTE_CLANG_X86_VECTOR_INSTRUCTIONS=sse4.2 \
                -DCMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD="c++17" \
                -DCMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY="libc++" \
                -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="" \
                -DCMAKE_OSX_ARCHITECTURES:STRING=x86_64 \
                -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                -DCMAKE_OSX_SYSROOT=${SDKROOT} \
                -DCMAKE_MACOSX_RPATH=YES \
                -DCMAKE_INSTALL_PREFIX=$stage

            cmake --build . --config Debug
            cmake --build . --config Release
        popd

        export MACOSX_DEPLOYMENT_TARGET=${ARM64_DEPLOY}

        mkdir -p "$stage/build_arm64"
        pushd "$stage/build_arm64"
            cmake $top -G Xcode \
                -DCEF_WRAPPER_DIR="$cef_no_wrapper_dir/arm64" \
                -DCEF_WRAPPER_BUILD_DIR="$cef_no_wrapper_dir/build_arm64" \
                -DUSE_SANDBOX=OFF \
                -DCMAKE_C_FLAGS="$ARCH_FLAGS_ARM64 $RELEASE_CFLAGS" \
                -DCMAKE_CXX_FLAGS="$ARCH_FLAGS_ARM64 $RELEASE_CXXFLAGS" \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL="3" \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_FAST_MATH=NO \
                -DCMAKE_XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS=YES \
                -DCMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT=dwarf-with-dsym \
                -DCMAKE_XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING=YES \
                -DCMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD="c++17" \
                -DCMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY="libc++" \
                -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="" \
                -DCMAKE_OSX_ARCHITECTURES:STRING=arm64 \
                -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                -DCMAKE_OSX_SYSROOT=${SDKROOT} \
                -DCMAKE_MACOSX_RPATH=YES \
                -DCMAKE_INSTALL_PREFIX=$stage

            cmake --build . --config Debug
            cmake --build . --config Release
        popd

        # copy files to staging ready to be packaged
        mkdir -p "$stage/include/cef"
        mkdir -p "$stage/bin/debug"
        mkdir -p "$stage/lib/debug"
        mkdir -p "$stage/bin/release"
        mkdir -p "$stage/lib/release"
        mkdir -p "$stage/LICENSES"

        lipo -create ${stage}/build_x86_64/Debug/libdullahan.a ${stage}/build_arm64/Debug/libdullahan.a -output ${stage}/lib/debug/libdullahan.a
        lipo -create ${stage}/build_x86_64/Release/libdullahan.a ${stage}/build_arm64/Release/libdullahan.a -output ${stage}/lib/release/libdullahan.a

        cp "$dullahan_source_dir/dullahan.h" "$stage/include/cef/"
        cp "$dullahan_source_dir/dullahan_version.h" "$stage/include/cef/"

        # Debug Copy
        cp -R $stage/build_x86_64/Debug/DullahanHost*.app "$stage/bin/debug"
        lipo -create "$stage/build_x86_64/Debug/DullahanHost.app/Contents/MacOS/DullahanHost" "$stage/build_arm64/Debug/DullahanHost.app/Contents/MacOS/DullahanHost" -output "$stage/bin/debug/DullahanHost.app/Contents/MacOS/DullahanHost"
        lipo -create "$stage/build_x86_64/Debug/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)" "$stage/build_arm64/Debug/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)" -output "$stage/bin/debug/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)"
        lipo -create "$stage/build_x86_64/Debug/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)" "$stage/build_arm64/Debug/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)" -output "$stage/bin/debug/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)"
        lipo -create "$stage/build_x86_64/Debug/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)" "$stage/build_arm64/Debug/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)" -output "$stage/bin/debug/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)"

        dsymutil -o "$stage/bin/debug/DullahanHost.dSYM" "$stage/bin/debug/DullahanHost.app/Contents/MacOS/DullahanHost"
        dsymutil -o "$stage/bin/debug/DullahanHost\ \(GPU\).dSYM" "$stage/bin/debug/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)"
        dsymutil -o "$stage/bin/debug/DullahanHost\ \(Plugin\).dSYM" "$stage/bin/debug/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)"
        dsymutil -o "$stage/bin/debug/DullahanHost\ \(Renderer\).dSYM" "$stage/bin/debug/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)"
    
        lipo -create $cef_no_wrapper_dir/build_x86_64/libcef_dll_wrapper/Debug/libcef_dll_wrapper.a $cef_no_wrapper_dir/build_arm64/libcef_dll_wrapper/Debug/libcef_dll_wrapper.a -output ${stage}/lib/debug/libcef_dll_wrapper.a

        cp -R "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework" "$stage/bin/debug"
        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Chromium Embedded Framework" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Chromium Embedded Framework" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Chromium Embedded Framework"

        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Libraries/libEGL.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Debug/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" -output "$stage/bin/debug/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib"

        rm "$stage/bin/debug/Chromium Embedded Framework.framework/Resources/snapshot_blob.bin"
        cp -R "$cef_no_wrapper_dir/arm64/Debug/Chromium Embedded Framework.framework/Resources/v8_context_snapshot.arm64.bin" "$stage/bin/debug/Chromium Embedded Framework.framework/Resources/"

        # Release Copy
        cp -R $stage/build_x86_64/Release/DullahanHost*.app "$stage/bin/release"

        lipo -create "$stage/build_x86_64/Release/DullahanHost.app/Contents/MacOS/DullahanHost" "$stage/build_arm64/Release/DullahanHost.app/Contents/MacOS/DullahanHost" -output "$stage/bin/release/DullahanHost.app/Contents/MacOS/DullahanHost"
        lipo -create "$stage/build_x86_64/Release/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)" "$stage/build_arm64/Release/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)" -output "$stage/bin/release/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)"
        lipo -create "$stage/build_x86_64/Release/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)" "$stage/build_arm64/Release/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)" -output "$stage/bin/release/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)"
        lipo -create "$stage/build_x86_64/Release/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)" "$stage/build_arm64/Release/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)" -output "$stage/bin/release/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)"

        dsymutil -o "$stage/bin/release/DullahanHost.dSYM" "$stage/bin/release/DullahanHost.app/Contents/MacOS/DullahanHost"
        dsymutil -o "$stage/bin/release/DullahanHost\ \(GPU\).dSYM" "$stage/bin/release/DullahanHost (GPU).app/Contents/MacOS/DullahanHost (GPU)"
        dsymutil -o "$stage/bin/release/DullahanHost\ \(Plugin\).dSYM" "$stage/bin/release/DullahanHost (Plugin).app/Contents/MacOS/DullahanHost (Plugin)"
        dsymutil -o "$stage/bin/release/DullahanHost\ \(Renderer\).dSYM" "$stage/bin/release/DullahanHost (Renderer).app/Contents/MacOS/DullahanHost (Renderer)"
    
        lipo -create $cef_no_wrapper_dir/build_x86_64/libcef_dll_wrapper/Release/libcef_dll_wrapper.a $cef_no_wrapper_dir/build_arm64/libcef_dll_wrapper/Release/libcef_dll_wrapper.a -output ${stage}/lib/release/libcef_dll_wrapper.a

        cp -R "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework" "$stage/bin/release"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework" -output "$stage/bin/release/Chromium Embedded Framework.framework/Chromium Embedded Framework"

        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" -output "$stage/bin/release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" -output "$stage/bin/release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib" -output "$stage/bin/release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libEGL.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib" -output "$stage/bin/release/Chromium Embedded Framework.framework/Libraries/libswiftshader_libGLESv2.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" -output "$stage/bin/release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib"

        rm "$stage/bin/release/Chromium Embedded Framework.framework/Resources/snapshot_blob.bin"
        cp -R "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Resources/v8_context_snapshot.arm64.bin" "$stage/bin/release/Chromium Embedded Framework.framework/Resources/"

        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"

        # # sign the binaries (both CEF and DullahanHelper)
        # CONFIG_FILE="$build_secrets_checkout/code-signing-osx/config.sh"
        # if [ -f "$CONFIG_FILE" ]; then
        #     source $CONFIG_FILE

        #     pushd "$stage/lib/release/Chromium Embedded Framework.framework/Libraries"
        #     for dylib in lib*.dylib;
        #     do
        #         if [ -f "$dylib" ]; then
        #             codesign --force --timestamp --options runtime --entitlements "$dullahan_source_dir/dullahan.entitlements" --sign "$APPLE_SIGNATURE" "$dylib"
        #         fi
        #     done
        #     codesign --force --timestamp --options runtime --entitlements "$dullahan_source_dir/dullahan.entitlements" --sign "$APPLE_SIGNATURE" "../Chromium Embedded Framework"
        #     popd

        #     pushd "$stage/lib/release/"
        #     for app in DullahanHelper*.app;
        #     do
        #         if [ -d "$app" ]; then
        #             sed -i "" "s/DullahanHelper/${app%.*}/" "$app/Contents/Info.plist"
        #             codesign --force --timestamp --options runtime --entitlements "$dullahan_source_dir/dullahan.entitlements" --sign "$APPLE_SIGNATURE" "$app"
        #         fi
        #     done
        #     popd
        # else
        #     echo "No config file found; skipping codesign."
        # fi

        # populate version_file (after CMake runs)
        g++ \
            -I "$cef_no_wrapper_dir/include/" \
            -I "$top/src" \
            -o "$stage/version" \
            "$top/tools/autobuild_version.cpp"
        "$stage/version" > "$stage/version.txt"
        rm "$stage/version"
    ;;
    linux64)
		#Force version regeneration.
		rm -f src/dullahan_version.h

        # build the CEF c->C++ wrapper "libcef_dll_wrapper"
		pushd "${cef_no_wrapper_dir}"
            rm -rf "${cef_no_wrapper_build_dir}"
            mkdir -p "${cef_no_wrapper_build_dir}"
            pushd "${cef_no_wrapper_build_dir}"
                cmake -G  Ninja .. -DCMAKE_BUILD_TYPE="Release"
		        ninja libcef_dll_wrapper
            popd
        popd
		
        pushd "$stage"
        cmake .. -G  Ninja -DCMAKE_BUILD_TYPE="Release" -DCEF_WRAPPER_DIR="${cef_no_wrapper_dir}" \
            -DCEF_WRAPPER_BUILD_DIR="${cef_no_wrapper_build_dir}" \
			  -DCMAKE_C_FLAGS:STRING="-m${AUTOBUILD_ADDRSIZE}" \
			  -DCMAKE_CXX_FLAGS:STRING="-m${AUTOBUILD_ADDRSIZE}"

		ninja

		g++ -std=c++17 	-I "${cef_no_wrapper_dir}/include" 	-I "${dullahan_source_dir}" -o "$stage/version"  "$top/tools/autobuild_version.cpp"

		"$stage/version" > "$stage/VERSION.txt"
		rm "$stage/version"
		
		mkdir -p "$stage/LICENSES"
		mkdir -p "$stage/bin/release/"
		mkdir -p "$stage/include"
		mkdir -p "$stage/include/cef"
		mkdir -p "$stage/lib/release/swiftshader"
		mkdir -p "$stage/resources"
		 
		cp libdullahan.a ${stage}/lib/release/
		cp ${cef_no_wrapper_build_dir}/libcef_dll_wrapper/libcef_dll_wrapper.a $stage/lib/release

		cp -a ${cef_no_wrapper_dir}/Release/*.so* ${stage}/lib/release/
		cp -a ${cef_no_wrapper_dir}/Release/swiftshader/* ${stage}/lib/release/swiftshader/

		cp dullahan_host ${stage}/bin/release/

        cp -a ${cef_no_wrapper_dir}/Release/*.json ${stage}/bin/release/
		cp -a ${cef_no_wrapper_dir}/Release/*.bin ${stage}/bin/release/
		cp -a ${cef_no_wrapper_dir}/Release/chrome-sandbox ${stage}/bin/release/

		cp -R ${cef_no_wrapper_dir}/Resources/* ${stage}/resources/
		cp ${dullahan_source_dir}/dullahan.h ${stage}/include/cef/
		cp ${dullahan_source_dir}/dullahan_version.h ${stage}/include/cef/
        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"
        popd
		;;
esac
