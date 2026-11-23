#!/usr/bin/env bash
# Builds and packages Kyujo2 (US English). Normally run by build-linux.sh, build-windows.sh, build-android.sh,
# build-switch.sh or build-all.sh, which first install what is needed and prepare the sources (tools/prepare-sources.sh us).
#
#   tools/build.sh [--label NAME] [--jobs N] linux|windows|android|switch|all ...
#
# Output: dist/<label>/{Linux,Windows,Android,Switch}. The default label comes from CMakeLists.txt (e.g. English-r26).
# The settings are the ones the tested releases were built with: desktop and Android builds use the Debug runtime with
# the generated game code at -O2; the Switch build is a full Release build with the GPU renderer.
#
# Environment (all optional):
#   KYUJO2_SYSROOT     staged Debian packages (default .work/sysroot when present, else the system's /usr)
#   ANDROID_HOME       Android SDK (default .work/android-sdk)
#   DEVKITPRO          devkitPro (default /opt/devkitpro)
#   KYUJO_REFERENCE_ROM  Pokémon Stadium (USA) cartridge for the menu artwork (default other_projects/Pokemon Stadium (USA).z64)
#   KYUJO2_ANDROID_KEYSTORE / _KEYSTORE_PASS / _KEY_ALIAS / _KEY_PASS  signing key for the APK (see tools/package-android.sh)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

label= jobs=${JOBS:-$(nproc)} targets=()
while (($#)); do
    case "$1" in
        --label) label=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        linux|windows|android|switch) targets+=("$1"); shift ;;
        all) targets+=(linux windows android switch); shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
((${#targets[@]})) || { echo "Name a platform: linux, windows, android, switch or all" >&2; exit 2; }
revision=$(sed -n 's/^set(KYUJO2_REVISION "\(.*\)" CACHE.*/\1/p' CMakeLists.txt)   # e.g. "English r26"
[[ -n "$label" ]] || label=${revision// /-}
[[ "$label" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "The label may contain letters, digits, - and _ only" >&2; exit 2; }
revision=${label//-/ }

[[ -f build/recompiled/us/funcs_0.c ]] || { echo "Generated game sources are missing: run tools/prepare-sources.sh us first" >&2; exit 1; }
reference=${KYUJO_REFERENCE_ROM:-$root/other_projects/Pokemon Stadium (USA).z64}
[[ -f "$reference" ]] || { echo "Missing the Pokémon Stadium (USA) cartridge for the menu artwork: $reference" >&2; exit 1; }

sysroot=${KYUJO2_SYSROOT:-}
if [[ -z "$sysroot" && -d .work/sysroot/usr ]]; then sysroot="$root/.work/sysroot"; fi
if [[ -n "$sysroot" ]]; then
    export PATH="$sysroot/usr/bin:$PATH"
    export LD_LIBRARY_PATH="$sysroot/usr/lib/x86_64-linux-gnu:$sysroot/usr/lib/x86_64-linux-gnu/pulseaudio${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    sdl_prefix="$sysroot/usr"
else
    sdl_prefix=/usr
fi
out="$root/dist/$label"
common=(-GNinja -DBUILD_RUNTIME_TRACE=ON -DKYUJO2_REVISION="$revision" -DKYUJO_REFERENCE_ROM="$reference")

build_linux() {
    CC=clang CXX=clang++ cmake -S . -B build/native-linux "${common[@]}" -DCMAKE_BUILD_TYPE=Debug -DKYUJO2_GPU=OFF \
        -DSDL_ROOT="$sdl_prefix" -DSDL_HEADERS="$sdl_prefix/include/SDL2" -DSDL_LIB="$sdl_prefix/lib/x86_64-linux-gnu/libSDL2.so"
    cmake --build build/native-linux --target Kyujo2 -j"$jobs"
    KYUJO2_SYSROOT="${sysroot:-}" tools/package-linux.sh "$label"
    mkdir -p "$out/Linux"
    cp "build/packages/$label/Kyujo2-US-$label.AppImage" "build/packages/$label/Kyujo2-US-$label.flatpak" "$out/Linux/"
}

build_windows() {
    local sdl="$root/.work/sdl-windows/SDL2-2.32.10/x86_64-w64-mingw32"
    [[ -d "$sdl" ]] || { echo "Missing $sdl: run ./build-windows.sh" >&2; return 1; }
    # The runtime includes <Windows.h>; MinGW's header is windows.h and Linux file names are case-sensitive.
    mkdir -p .work/mingw-compat
    printf '#include <windows.h>\n' > .work/mingw-compat/Windows.h
    cmake -S . -B build/native-windows "${common[@]}" -DCMAKE_BUILD_TYPE=Debug -DKYUJO2_GPU=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$root/tools/mingw-toolchain.cmake" -DCMAKE_CXX_FLAGS="-I$root/.work/mingw-compat" \
        -DSDL_ROOT="$sdl" -DSDL_HEADERS="$sdl/include/SDL2" -DSDL_LIB="$sdl/lib/libSDL2.dll.a"
    cmake --build build/native-windows --target Kyujo2 -j"$jobs"
    mkdir -p "$out/Windows"
    x86_64-w64-mingw32-strip -o "$out/Windows/Kyujo2.exe" build/native-windows/Kyujo2.exe
    cp "$sdl/bin/SDL2.dll" "$out/Windows/"
}

build_android() {
    local sdk=${ANDROID_HOME:-$root/.work/android-sdk}
    local ndk=${ANDROID_NDK_HOME:-$sdk/ndk/27.3.13750724}
    local toolchain="$ndk/build/cmake/android.toolchain.cmake"
    [[ -f "$toolchain" ]] || { echo "Missing the Android NDK at $ndk: run ./build-android.sh" >&2; return 1; }
    [[ -d .work/SDL2-2.32.10 ]] || { echo "Missing .work/SDL2-2.32.10: run ./build-android.sh" >&2; return 1; }
    if [[ ! -f .work/sdl-android/libSDL2.so ]]; then
        cmake -GNinja -S .work/SDL2-2.32.10 -B .work/sdl-android -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
            -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 -DSDL_STATIC=OFF -DSDL_TEST=OFF -DCMAKE_BUILD_TYPE=Release
        cmake --build .work/sdl-android -j"$jobs"
    fi
    cmake -S . -B build/game-android "${common[@]}" -DCMAKE_BUILD_TYPE=Debug -DKYUJO2_GPU=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 -DANDROID_STL=c++_static \
        -DSDL_HEADERS="$root/.work/SDL2-2.32.10/include" -DSDL_LIB="$root/.work/sdl-android/libSDL2.so"
    cmake --build build/game-android --target Kyujo2 -j"$jobs"
    ANDROID_HOME="$sdk" ANDROID_NDK_HOME="$ndk" tools/package-android.sh "$label"
    mkdir -p "$out/Android"
    cp "build/packages/$label/Kyujo2-US-$label.apk" "$out/Android/"
}

build_switch() {
    export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
    export DEVKITA64=${DEVKITA64:-$DEVKITPRO/devkitA64}
    export PATH="$DEVKITPRO/tools/bin:$PATH"
    [[ -f "$DEVKITPRO/cmake/Switch.cmake" ]] || { echo "devkitPro is not installed at $DEVKITPRO (see BUILDING.md)" >&2; return 1; }
    cmake -S . -B build/game-switch "${common[@]}" -DCMAKE_BUILD_TYPE=Release -DKYUJO2_GPU=ON \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
        -DSDL_HEADERS="$DEVKITPRO/portlibs/switch/include/SDL2" -DSDL_LIB="$DEVKITPRO/portlibs/switch/lib/libSDL2.a"
    cmake --build build/game-switch -j"$jobs"
    mkdir -p "$out/Switch"
    cp build/game-switch/Kyujo2.nro "$out/Switch/Kyujo2-US-$label.nro"
    cp build/game-switch/Kyujo2.elf "$out/Switch/Kyujo2-US-$label.elf"
}

for t in "${targets[@]}"; do
    printf '\n==== %s (%s)\n' "$t" "$label"
    "build_$t"
done
printf '\nBuilt into %s\n' "$out"
find "$out" -type f -printf '  %10s  %P\n' | sort -k2
