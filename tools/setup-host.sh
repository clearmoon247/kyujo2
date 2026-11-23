#!/usr/bin/env bash
# Prepares a Debian 13 (trixie) x86-64 host to build Kyujo2 for every platform.
#
#   tools/setup-host.sh --apt      install the Debian packages with sudo apt-get (recommended)
#   tools/setup-host.sh --local    no root: download the packages and unpack them into .work/sysroot
#   add --android                  also install the Android SDK, NDK and build tools into .work/android-sdk
#   add --no-switch-check          do not check for devkitPro (Switch builds)
#
# Every download is pinned and checked against its SHA-256 before use. Run from anywhere; files go under .work/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

mode= android=0 switch_check=1
for arg in "$@"; do
    case "$arg" in
        --apt) mode=apt ;;
        --local) mode=local ;;
        --android) android=1 ;;
        --no-switch-check) switch_check=0 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done
[[ -n "$mode" ]] || { echo "Choose --apt (sudo) or --local (no root). See --help." >&2; exit 2; }

mkdir -p .work/packages
say() { printf '\n== %s\n' "$*"; }

# ---- tools every build needs (not staged by --local: they are ordinary development packages) ------------------------
base_pkgs=(build-essential clang cmake ninja-build git unzip patch make python3 python3-venv python3-pip curl
           default-jdk-headless zstd xz-utils)
# ---- packages the build uses from the Debian archive (Debian 13 names) ---------------------------------------------
# binutils-mips-linux-gnu: rebuilding the game's ELF from the decompilation; mingw: Windows; libsdl2-dev: Linux;
# squashfs-tools, flatpak, patchelf: Linux AppImage and Flatpak packaging.
build_pkgs=(binutils-mips-linux-gnu
            binutils-mingw-w64-x86-64 gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
            gcc-mingw-w64-x86-64-posix-runtime mingw-w64-common mingw-w64-x86-64-dev
            libsdl2-dev libsdl2-2.0-0 squashfs-tools flatpak patchelf)
# --local only: the libraries libsdl2 and flatpak need that a desktop Debian may not have. The AppImage bundles the
# ones found here. This is the set the released builds were made with.
local_extra=(bubblewrap fuse3 libfuse3-4 xdg-dbus-proxy libostree-1-1 libappstream5 libxmlb2 libstemmer0d
             libmalcontent-0-0 libpolkit-agent-1-0 libpolkit-gobject-1-0 libgpgme11t64 libjson-glib-1.0-0
             libjson-glib-1.0-common libdconf1 libavahi-glib1 libgdk-pixbuf-2.0-0 libgdk-pixbuf2.0-common liblzo2-2
             libjson-c5 libpulse0 libasyncns0 libsndfile1 libflac14 libogg0 libvorbis0a libvorbisenc2 libopus0
             libmpg123-0t64 libmp3lame0 libsamplerate0 libdecor-0-0 libwayland-client0 libwayland-cursor0
             libwayland-egl1 libwayland-server0 libxkbcommon0 xkb-data libx11-xcb1 libxcursor1 libxfixes3 libxi6
             libxinerama1 libxrandr2 libxrender1 libxss1 libxtst6 libxcb-dri3-0 libxcb-present0 libxcb-randr0
             libxcb-shape0 libxcb-sync1 libxcb-xfixes0 libxshmfence1 libdrm2 libdrm-common libdrm-amdgpu1
             libdrm-intel1 libpciaccess0 libgbm1 mesa-libgallium libsensors5 libsensors-config x11-common
             libdw1t64 python3-pip-whl python3-setuptools-whl)

if [[ "$mode" == apt ]]; then
    say "Installing Debian packages (sudo)"
    sudo apt-get update
    sudo apt-get install -y "${base_pkgs[@]}" "${build_pkgs[@]}"
else
    say "Checking the base development tools"
    missing=()
    for c in gcc make clang clang++ cmake ninja git unzip patch python3 curl javac jar keytool; do
        command -v "$c" >/dev/null || missing+=("$c")
    done
    if ((${#missing[@]})); then
        echo "These commands are needed and were not found: ${missing[*]}" >&2
        echo "Install them (Debian: sudo apt-get install ${base_pkgs[*]}) or ask the administrator, then run this again." >&2
        exit 1
    fi
    say "Downloading the build packages into .work/packages and unpacking them into .work/sysroot"
    (cd .work/packages && apt-get download "${build_pkgs[@]}" "${local_extra[@]}")
    mkdir -p .work/sysroot
    for deb in .work/packages/*.deb; do dpkg-deb -x "$deb" .work/sysroot; done
    # The build calls the "-posix" MinGW compilers by those names, which the packages install directly, so the
    # update-alternatives step that unpacking skips is not needed.
fi

# ---- pinned downloads -------------------------------------------------------------------------------------------------
fetch() {   # url sha256 file
    local url=$1 sum=$2 out=".work/packages/$3"
    if [[ ! -f "$out" ]] || ! printf '%s  %s\n' "$sum" "$out" | sha256sum -c --status -; then
        echo "downloading $url"
        curl -fsSL --retry 3 -o "$out.part" "$url"
        mv "$out.part" "$out"
    fi
    printf '%s  %s\n' "$sum" "$out" | sha256sum -c -
}
say "Downloading SDL 2.32.10 (Android source, Windows MinGW package) and the AppImage runtime"
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
      5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 SDL2-2.32.10.tar.gz
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz \
      83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2 SDL2-devel-2.32.10-mingw.tar.gz
# The AppImage project publishes this runtime only as a rolling "continuous" build. The pinned hash is the one the
# releases were made with; if upstream replaced it, set KYUJO2_APPIMAGE_RUNTIME_SHA256 to the new file's hash.
fetch https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
      "${KYUJO2_APPIMAGE_RUNTIME_SHA256:-617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46}" appimage-runtime-x86_64
[[ -d .work/SDL2-2.32.10 ]] || tar -xzf .work/packages/SDL2-2.32.10.tar.gz -C .work
mkdir -p .work/sdl-windows
[[ -d .work/sdl-windows/SDL2-2.32.10 ]] || tar -xzf .work/packages/SDL2-devel-2.32.10-mingw.tar.gz -C .work/sdl-windows

# ---- Android ----------------------------------------------------------------------------------------------------------
if ((android)); then
    say "Installing the Android SDK pieces into .work/android-sdk"
    fetch https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
          2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258 android-cmdtools.zip
    sdk="$root/.work/android-sdk"
    if [[ ! -x "$sdk/cmdline-tools/latest/bin/sdkmanager" && ! -x "$sdk/cmdline-tools/cmdline-tools/bin/sdkmanager" ]]; then
        mkdir -p "$sdk/cmdline-tools"
        unzip -q .work/packages/android-cmdtools.zip -d "$sdk/cmdline-tools"
        mv "$sdk/cmdline-tools/cmdline-tools" "$sdk/cmdline-tools/latest"
    fi
    sdkmanager=$(ls "$sdk"/cmdline-tools/*/bin/sdkmanager | head -1)
    yes | "$sdkmanager" --sdk_root="$sdk" --licenses >/dev/null || true
    "$sdkmanager" --sdk_root="$sdk" "platforms;android-34" "build-tools;34.0.0" "build-tools;35.0.1" "ndk;27.3.13750724"
fi

# ---- Switch -----------------------------------------------------------------------------------------------------------
if ((switch_check)); then
    say "Checking devkitPro (Switch)"
    dkp=${DEVKITPRO:-/opt/devkitpro}
    need=(devkitA64 libnx switch-tools switch-cmake switch-pkg-config switch-sdl2 switch-mesa switch-libdrm_nouveau)
    if [[ -x "$dkp/devkitA64/bin/aarch64-none-elf-gcc" && -f "$dkp/portlibs/switch/lib/libSDL2.a" \
          && -f "$dkp/portlibs/switch/lib/libGLESv2.a" && -f "$dkp/cmake/Switch.cmake" ]]; then
        echo "devkitPro found at $dkp"
    else
        echo "devkitPro (Switch) is not installed at $dkp. Install it from https://devkitpro.org/wiki/Getting_Started," >&2
        echo "then run:  sudo dkp-pacman -S ${need[*]}" >&2
        echo "(Only needed for the Switch build; the other platforms do not need it.)" >&2
    fi
fi

say "Done. Next: put the cartridges and source archives in other_projects/ (see BUILDING.md), then run"
echo "  tools/prepare-sources.sh us"
echo "  tools/build.sh all"
