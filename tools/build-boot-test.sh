#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
target=${1:-linux}
out="build/boot-r2/$target"
mkdir -p "$out/include"
patch --silent -o "$out/include/recomp.h" .work/N64Recomp/include/recomp.h patches/recomp-rdram.patch
python3 tools/boot-test-units.py build/recompiled/us "$out/game.c"
src=(-O2 -g -fwrapv -fno-strict-aliasing -I"$out/include" -Ibuild/recompiled/us tests/guest_boot.c "$out/game.c")
name="$out/Kyujo2-US-BootTest-r2"
case "$target" in
linux)
    clang "${src[@]}" -lm -o "$name"
    "$name"
    ;;
windows)
    cross=${MINGW_ROOT:-"$PWD/.work/sysroot/usr"}
    clang --target=x86_64-w64-windows-gnu --sysroot="$cross/x86_64-w64-mingw32" \
        -L"$cross/lib/gcc/x86_64-w64-mingw32/14-posix" -fuse-ld=lld "${src[@]}" -o "$name.exe"
    ;;
switch)
    d=${DEVKITPRO:-/opt/devkitpro}
    "$d/devkitA64/bin/aarch64-none-elf-gcc" -D__SWITCH__ -march=armv8-a+crc+crypto \
        -mtune=cortex-a57 -mtp=soft -fPIE -I"$d/libnx/include" "${src[@]}" \
        -specs="$d/libnx/switch.specs" -L"$d/libnx/lib" -Wl,-Map="$name.map" -lnx -lm -o "$name.elf"
    "$d/tools/bin/nacptool" --create "Kyujo2 Boot Test" "Kyujo2" "0.2" "$out/test.nacp"
    "$d/tools/bin/elf2nro" "$name.elf" "$name.nro" --nacp="$out/test.nacp" --icon="$d/libnx/default_icon.jpg"
    ;;
android)
    sdk=${ANDROID_HOME:-"$PWD/.work/android-sdk"}
    ndk=${ANDROID_NDK_HOME:-"$sdk/ndk/27.3.13750724"}
    bt="$sdk/build-tools/34.0.0"
    jar="$sdk/platforms/android-34/android.jar"
    mkdir -p "$out/lib/arm64-v8a" "$out/classes"
    "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang" \
        -fPIC -shared "${src[@]}" tests/android/boot_jni.c -Wl,-z,max-page-size=16384 \
        -lm -o "$out/lib/arm64-v8a/libkyujo2boot.so"
    javac --release 8 -cp "$jar" -d "$out/classes" tests/android/BootTest.java
    "$bt/d8" --min-api 24 --lib "$jar" --output "$out" "$out/classes/org/kyujo2/boottest/BootTest.class"
    "$bt/aapt2" link -I "$jar" --manifest tests/android/AndroidManifest.xml -o "$out/manifest.apk"
    python3 - "$out" <<'PY'
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import sys
out = Path(sys.argv[1])
with ZipFile(out/'manifest.apk') as src, ZipFile(out/'unsigned.apk','w',ZIP_DEFLATED) as dest:
    for entry in src.infolist():
        dest.writestr(entry, src.read(entry))
    for asset in ['classes.dex','lib/arm64-v8a/libkyujo2boot.so']:
        dest.write(out/asset, asset)
PY
    "$bt/zipalign" -f 4 "$out/unsigned.apk" "$out/aligned.apk"
    key=.work/kyujo2-debug.jks
    if [[ ! -f "$key" ]]; then
        keytool -genkeypair -keystore "$key" -alias androiddebugkey -storepass android -keypass android \
            -keyalg RSA -keysize 2048 -validity 3650 -dname 'CN=Kyujo2 Local Test'
    fi
    "$bt/apksigner" sign --ks "$key" --ks-pass pass:android --out "$name.apk" "$out/aligned.apk"
    "$bt/apksigner" verify --verbose "$name.apk"
    ;;
*) echo "Expected linux, windows, switch or android" >&2; exit 2 ;;
esac
