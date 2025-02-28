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

# remove_cxxstd
source "$(dirname "$AUTOBUILD_VARIABLES_FILE")/functions"

# Use msbuild.exe instead of devenv.com
build_sln() {
    local solution=$1
    local config=$2
    local proj="${3:-}"
    local toolset="${AUTOBUILD_WIN_VSTOOLSET:-v143}"

    # e.g. config = "Release|$AUTOBUILD_WIN_VSPLATFORM" per devenv.com convention
    local -a confparts
    IFS="|" read -ra confparts <<< "$config"

    msbuild.exe \
        "$(cygpath -w "$solution")" \
        ${proj:+-t:"$proj"} \
        -p:Configuration="${confparts[0]}" \
        -p:Platform="${confparts[1]}" \
        -p:PlatformToolset=$toolset
}

build=${AUTOBUILD_BUILD_ID:=0}

case "$AUTOBUILD_PLATFORM" in
    windows*)
        load_vsvars

        opts="$(replace_switch /Zi /Z7 $LL_BUILD_RELEASE)"

        # build the CEF c->C++ wrapper "libcef_dll_wrapper"
        cd "$cef_no_wrapper_dir"
        rm -rf "$cef_no_wrapper_build_dir"
        mkdir -p "$cef_no_wrapper_build_dir"
        cd "$cef_no_wrapper_build_dir"
        cmake .. -G "Ninja Multi-Config" \
                    -DCEF_RUNTIME_LIBRARY_FLAG="-MD" -DUSE_SANDBOX=Off
        cmake --build . --config Release --target libcef_dll_wrapper

        # generate the project files for Dullahan
        cd "$stage"
        cmake .. \
            -G "$AUTOBUILD_WIN_CMAKE_GEN" -A "$AUTOBUILD_WIN_VSPLATFORM" \
            -DCEF_WRAPPER_DIR="$(cygpath -w "$cef_no_wrapper_dir")" \
            -DCEF_WRAPPER_BUILD_DIR="$(cygpath -w "$cef_no_wrapper_build_dir")" \
            -DCMAKE_CXX_FLAGS="$opts" \
            $(cmake_cxx_standard $opts)

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
        cp "$cef_no_wrapper_dir/Release/chrome_elf.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/d3dcompiler_47.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libcef.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libEGL.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/libGLESv2.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/snapshot_blob.bin" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/v8_context_snapshot.bin" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vk_swiftshader.dll" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vk_swiftshader_icd.json" "$stage/bin/release/"
        cp "$cef_no_wrapper_dir/Release/vulkan-1.dll" "$stage/bin/release/"

        # CEF resources
        cp -R "$cef_no_wrapper_dir/Resources/"* "$stage/resources/"

        # licenses
        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"

        # populate version_file (after CMake runs)
        cl /EHsc \
            /Fo"$(cygpath -w "$stage/version.obj")" \
            /Fe"$(cygpath -w "$stage/version.exe")" \
            /I "$(cygpath -w "$cef_no_wrapper_dir/include/")"  \
            /I "$(cygpath -w "$top/src")"  \
            "$(cygpath -w "$top/tools/autobuild_version.cpp")"
        "$stage/version.exe" > "$stage/version.txt"
        rm "$stage"/version.{obj,exe}
    ;;
    darwin*)
        export MACOSX_DEPLOYMENT_TARGET="$LL_BUILD_DARWIN_DEPLOY_TARGET"

        # build the CEF c->C++ wrapper "libcef_dll_wrapper"
        pushd "$cef_no_wrapper_dir"
            rm -rf "$cef_no_wrapper_build_dir"
            mkdir -p "$cef_no_wrapper_build_dir"
            pushd "$cef_no_wrapper_build_dir"
                for arch in x86_64 arm64 ; do
                    ARCH_ARGS="-arch $arch"
                    cxx_opts="${TARGET_OPTS:-$ARCH_ARGS $LL_BUILD_RELEASE}"
                    cc_opts="$(remove_cxxstd $cxx_opts)"

                    mkdir -p "$arch"
                    pushd "$arch"
                        cmake "$cef_no_wrapper_dir/$arch" -G Xcode \
                            -DPROJECT_ARCH="$arch" \
                            -DCMAKE_C_FLAGS="$cc_opts" \
                            -DCMAKE_CXX_FLAGS="$cxx_opts" \
                            -DCMAKE_OSX_ARCHITECTURES="$arch" \
                            -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                            $(cmake_cxx_standard $opts)

                        cmake --build . --config Release --target libcef_dll_wrapper --parallel $AUTOBUILD_CPU_COUNT
                    popd
                done
            popd
        popd

        pushd "$stage"
            for arch in x86_64 arm64 ; do
                ARCH_ARGS="-arch $arch"
                cxx_opts="${TARGET_OPTS:-$ARCH_ARGS $LL_BUILD_RELEASE}"
                cc_opts="$(remove_cxxstd $cxx_opts)"

                mkdir -p "build_$arch"
                pushd "build_$arch"
                    cmake $top -G Xcode \
                        -DCMAKE_OSX_ARCHITECTURES="$arch" \
                        -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
                        -DCEF_WRAPPER_DIR="$cef_no_wrapper_dir/$arch" \
                        -DCEF_WRAPPER_BUILD_DIR="$cef_no_wrapper_build_dir/$arch" \
                        -DCMAKE_C_FLAGS:STRING="$cc_opts" \
                        -DCMAKE_CXX_FLAGS:STRING="$cxx_opts" \
                        $(cmake_cxx_standard $opts)

                    cmake --build . --config Release --parallel $AUTOBUILD_CPU_COUNT
                popd
            done
        popd

        # copy files to staging ready to be packaged
        mkdir -p "$stage/include/cef"
        mkdir -p "$stage/lib/release"
        mkdir -p "$stage/LICENSES"

        lipo -create -output $stage/lib/release/libdullahan.a "$stage/build_x86_64/Release/libdullahan.a" "$stage/build_arm64/Release/libdullahan.a"
        cp "$dullahan_source_dir/dullahan.h" "$stage/include/cef/"
        cp "$dullahan_source_dir/dullahan_version.h" "$stage/include/cef/"
        cp -R "$stage/build_x86_64/Release/DullahanHelper.app" "$stage/lib/release"

        cp -R "$stage/build_x86_64/Release/DullahanHelper.app" "$stage/lib/release/DullahanHelper (GPU).app"
        mv "$stage/lib/release/DullahanHelper (GPU).app/Contents/MacOS/DullahanHelper" "$stage/lib/release/DullahanHelper (GPU).app/Contents/MacOS/DullahanHelper (GPU)"

        cp -R "$stage/build_x86_64/Release/DullahanHelper.app" "$stage/lib/release/DullahanHelper (Renderer).app"
        mv "$stage/lib/release/DullahanHelper (Renderer).app/Contents/MacOS/DullahanHelper" "$stage/lib/release/DullahanHelper (Renderer).app/Contents/MacOS/DullahanHelper (Renderer)"

        cp -R "$stage/build_x86_64/Release/DullahanHelper.app" "$stage/lib/release/DullahanHelper (Plugin).app"
        mv "$stage/lib/release/DullahanHelper (Plugin).app/Contents/MacOS/DullahanHelper" "$stage/lib/release/DullahanHelper (Plugin).app/Contents/MacOS/DullahanHelper (Plugin)"

        lipo -create "$stage/build_x86_64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" "$stage/build_arm64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" -output "$stage/lib/release/DullahanHelper.app/Contents/MacOS/DullahanHelper"
        lipo -create "$stage/build_x86_64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" "$stage/build_arm64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" -output "$stage/lib/release/DullahanHelper (GPU).app/Contents/MacOS/DullahanHelper (GPU)"
        lipo -create "$stage/build_x86_64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" "$stage/build_arm64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" -output "$stage/lib/release/DullahanHelper (Plugin).app/Contents/MacOS/DullahanHelper (Plugin)"
        lipo -create "$stage/build_x86_64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" "$stage/build_arm64/Release/DullahanHelper.app/Contents/MacOS/DullahanHelper" -output "$stage/lib/release/DullahanHelper (Renderer).app/Contents/MacOS/DullahanHelper (Renderer)"

        lipo -create -output "$stage/lib/release/libcef_dll_wrapper.a" "$cef_no_wrapper_build_dir/x86_64/libcef_dll_wrapper/Release/libcef_dll_wrapper.a" "$cef_no_wrapper_build_dir/arm64/libcef_dll_wrapper/Release/libcef_dll_wrapper.a"

        cp -R "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework" "$stage/lib/release"

        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework" -output "$stage/lib/release/Chromium Embedded Framework.framework/Chromium Embedded Framework"

        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib" -output "$stage/lib/release/Chromium Embedded Framework.framework/Libraries/libEGL.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib" -output "$stage/lib/release/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib"
        lipo -create "$cef_no_wrapper_dir/x86_64/Release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib" -output "$stage/lib/release/Chromium Embedded Framework.framework/Libraries/libvk_swiftshader.dylib"

        # Set up snapshot blobs for universal
        rm "$stage/lib/release/Chromium Embedded Framework.framework/Resources/snapshot_blob.bin"
        cp -R "$cef_no_wrapper_dir/arm64/Release/Chromium Embedded Framework.framework/Resources/v8_context_snapshot.arm64.bin" "$stage/lib/release/Chromium Embedded Framework.framework/Resources/"

        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"

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

        cmake -S . -B stage/build  -DCMAKE_BUILD_TYPE=Release -G Ninja -DCMAKE_INSTALL_PREFIX=stage -DENABLE_CXX11_ABI=ON \
        -DUSE_SPOTIFY_CEF=TRUE -DSPOTIFY_CEF_URL=https://cef-builds.spotifycdn.com/cef_binary_118.4.1%2Bg3dd6078%2Bchromium-118.0.5993.54_linux64_beta_minimal.tar.bz2
		cmake --build stage/build
		cmake --install stage/build

        strip stage/lib/release/libcef.so
	rm stage/bin/release/*.so*
	rm stage/bin/release/*.json

        g++ -std=c++17  -I "${cef_no_wrapper_dir}/include"  -I "${dullahan_source_dir}" -o "$stage/version"  "$top/tools/autobuild_version.cpp"

        "$stage/version" > "$stage/VERSION.txt"
        rm "$stage/version"

        mkdir -p "$stage/LICENSES"
        mkdir -p "$stage/include/cef"

        cp ${dullahan_source_dir}/dullahan.h ${stage}/include/cef/
        cp ${dullahan_source_dir}/dullahan_version.h ${stage}/include/cef/
        cp "$top/CEF_LICENSE.txt" "$stage/LICENSES"
        cp "$top/LICENSE.txt" "$stage/LICENSES"
        ;;
esac
