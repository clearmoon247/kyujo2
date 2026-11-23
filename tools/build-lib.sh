#!/usr/bin/env bash
# Shared by build-linux.sh, build-windows.sh, build-android.sh, build-switch.sh and build-all.sh (sourced, not run).
# It installs what a build needs, once: Debian packages (one sudo question), SDL and the AppImage runtime (pinned,
# SHA-256 checked), the Android SDK/NDK (after you accept Google's licence), devkitPro (with devkitPro's own
# installer). It finds your game files by their SHA-1, fetches the decompilation at its pinned commit, and prepares
# the generated sources (once; again only when the patches change). Everything it keeps goes into .work.
#
# Settings (all optional):
#   KYUJO2_ROMS=<folder>   where your game files are (default: roms/, then other_projects/, then ~/Downloads)
#   KYUJO2_DECOMP=<folder|zip>  a copy of the Pokémon Stadium 2 decompilation you already have
#   KYUJO2_JOBS=<n>        parallel jobs (default: from the CPU count and free memory)
#   KYUJO2_YES=1           answer "yes" to the questions (installing packages, Android licence, devkitPro)
#   KYUJO2_NO_SUDO=1       never use sudo: the Debian packages are unpacked into .work/sysroot instead
#   ANDROID_HOME, DEVKITPRO  existing Android SDK / devkitPro to use
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
WORK="$REPO/.work"
LOG="$WORK/logs"
mkdir -p "$WORK/packages" "$LOG"

if [ "$(id -u)" = "0" ] && [ -z "${KYUJO2_ALLOW_ROOT:-}" ]; then
    echo >&2; echo "Kyujo2 build stopped: do not run this with sudo (or as root)." >&2
    echo "   Run it as your normal user:  ./$(basename "$0")   It asks for sudo itself, only when it has to install something." >&2
    exit 1
fi

STEP_N=0
step() { STEP_N=$((STEP_N + 1)); echo; echo "== [$STEP_N/$STEP_TOTAL] $*"; }
note() { echo "   $*"; }
fail() { echo; echo "Kyujo2 build stopped: $*" >&2; exit 1; }
yes_or_stop() {     # question; KYUJO2_YES=1 answers yes
    [ "${KYUJO2_YES:-}" = "1" ] && return 0
    [ -t 0 ] || fail "$1 (not asked: no keyboard. Run again with KYUJO2_YES=1 to answer yes.)"
    read -r -p "   $1 [y/N] " a; case "$a" in y|Y|yes|YES) return 0 ;; *) fail "stopped, as you answered no" ;; esac
}
quiet() {           # logname command...  (runs quietly into .work/logs/<logname>.log; on failure shows its end)
    local name="$1"; shift
    if ! "$@" > "$LOG/$name.log" 2>&1; then
        echo "   the command failed: $*" >&2; tail -n 25 "$LOG/$name.log" >&2
        fail "$name failed; the whole log is $LOG/$name.log"
    fi
}
if [ -z "${KYUJO2_JOBS:-}" ]; then
    mem_jobs=$(awk '/MemAvailable/ { j = int($2 / 1500000); print (j < 1 ? 1 : j) }' /proc/meminfo 2>/dev/null || echo 4)
    KYUJO2_JOBS=$(nproc); [ "$mem_jobs" -lt "$KYUJO2_JOBS" ] && KYUJO2_JOBS=$mem_jobs
fi
export JOBS="$KYUJO2_JOBS"

# ---- Debian packages ----------------------------------------------------------------------------------------------
PKGS_common="git clang cmake ninja-build make gcc patch unzip curl python3 python3-venv binutils-mips-linux-gnu"
PKGS_linux="libsdl2-dev squashfs-tools flatpak"
PKGS_windows="binutils-mingw-w64-x86-64 gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev"
PKGS_android="default-jdk-headless"
PKGS_switch=""
# KYUJO2_NO_SUDO: everything the builds take from Debian, plus the libraries SDL and flatpak need, unpacked into
# .work/sysroot (the AppImage then bundles them). This is the set the tested releases were built with.
STAGE_PKGS="binutils-mips-linux-gnu binutils-mingw-w64-x86-64 gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
 gcc-mingw-w64-x86-64-posix-runtime mingw-w64-common mingw-w64-x86-64-dev libsdl2-dev libsdl2-2.0-0 squashfs-tools
 flatpak patchelf bubblewrap fuse3 libfuse3-4 xdg-dbus-proxy libostree-1-1 libappstream5 libxmlb2 libstemmer0d
 libmalcontent-0-0 libpolkit-agent-1-0 libpolkit-gobject-1-0 libgpgme11t64 libjson-glib-1.0-0 libjson-glib-1.0-common
 libdconf1 libavahi-glib1 libgdk-pixbuf-2.0-0 libgdk-pixbuf2.0-common liblzo2-2 libjson-c5 libpulse0 libasyncns0
 libsndfile1 libflac14 libogg0 libvorbis0a libvorbisenc2 libopus0 libmpg123-0t64 libmp3lame0 libsamplerate0
 libdecor-0-0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 libxkbcommon0 xkb-data
 libx11-xcb1 libxcursor1 libxfixes3 libxi6 libxinerama1 libxrandr2 libxrender1 libxss1 libxtst6 libxcb-dri3-0
 libxcb-present0 libxcb-randr0 libxcb-shape0 libxcb-sync1 libxcb-xfixes0 libxshmfence1 libdrm2 libdrm-common
 libdrm-amdgpu1 libdrm-intel1 libpciaccess0 libgbm1 mesa-libgallium libsensors5 libsensors-config x11-common libdw1t64
 python3-pip-whl python3-setuptools-whl"
STAGE_PKGS=$(echo $STAGE_PKGS)    # one line, single spaces (the membership tests below rely on it)

use_sysroot() {
    if [ -f "$WORK/sysroot/.complete" ]; then
        export KYUJO2_SYSROOT="$WORK/sysroot"
        export PATH="$WORK/sysroot/usr/bin:$PATH"
        export LD_LIBRARY_PATH="$WORK/sysroot/usr/lib/x86_64-linux-gnu:$WORK/sysroot/usr/lib/x86_64-linux-gnu/pulseaudio${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
}
stage_sysroot() {
    [ -f "$WORK/sysroot/.complete" ] && return 0
    command -v apt-get >/dev/null && command -v dpkg-deb >/dev/null || fail "KYUJO2_NO_SUDO needs a Debian system (apt-get, dpkg-deb)"
    note "downloading the Debian packages into .work/packages and unpacking them into .work/sysroot (no sudo)"
    (cd "$WORK/packages" && quiet stage-download apt-get download $STAGE_PKGS)
    mkdir -p "$WORK/sysroot"
    for deb in "$WORK"/packages/*.deb; do dpkg-deb -x "$deb" "$WORK/sysroot"; done
    touch "$WORK/sysroot/.complete"
}
need_packages() {   # platforms...
    local want="$PKGS_common" p missing=()
    for p in "$@"; do eval "want=\"\$want \$PKGS_$p\""; done
    use_sysroot
    if ! command -v dpkg >/dev/null; then note "not a Debian system: the packages are not checked ($want)"; return 0; fi
    for p in $want; do
        dpkg -s "$p" >/dev/null 2>&1 && continue
        [ "$p" = python3-venv ] && [ -f "$WORK/sysroot/.complete" ] && continue    # the staged pip wheel stands in
        [ -f "$WORK/sysroot/.complete" ] && [[ " $STAGE_PKGS " == *" $p "* ]] && continue
        missing+=("$p")
    done
    if [ ${#missing[@]} -eq 0 ]; then note "all found; using $JOBS parallel jobs"; return 0; fi
    if [ -n "${KYUJO2_NO_SUDO:-}" ]; then
        local base=() s
        for p in "${missing[@]}"; do [[ " $STAGE_PKGS python3-venv " == *" $p "* ]] || base+=("$p"); done
        [ ${#base[@]} -eq 0 ] || fail "these must be installed by an administrator: ${base[*]}   (sudo apt install ${base[*]})"
        stage_sysroot; use_sysroot
    else
        note "missing: ${missing[*]}"
        yes_or_stop "Install them now with sudo apt install?"
        sudo apt-get update -qq || true
        sudo apt-get install -y "${missing[@]}" || fail "could not install ${missing[*]}"
    fi
    note "all found; using $JOBS parallel jobs"
}

# ---- pinned downloads ---------------------------------------------------------------------------------------------
download_checked() {   # url file sha256
    local url=$1 out="$WORK/packages/$2" sum=$3
    if [ -f "$out" ] && printf '%s  %s\n' "$sum" "$out" | sha256sum -c --status -; then return 0; fi
    note "downloading $2"
    curl -fsSL --retry 3 -o "$out.part" "$url" || fail "could not download $url (needs internet access)"
    printf '%s  %s\n' "$sum" "$out.part" | sha256sum -c --status - \
        || fail "$2 does not match its expected SHA-256; the download is not used"
    mv "$out.part" "$out"
}
ensure_sdl_source() {
    download_checked https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
        SDL2-2.32.10.tar.gz 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    [ -d "$WORK/SDL2-2.32.10" ] || tar -xzf "$WORK/packages/SDL2-2.32.10.tar.gz" -C "$WORK"
}
ensure_sdl_mingw() {
    download_checked https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz \
        SDL2-devel-2.32.10-mingw.tar.gz 83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
    mkdir -p "$WORK/sdl-windows"
    [ -d "$WORK/sdl-windows/SDL2-2.32.10" ] || tar -xzf "$WORK/packages/SDL2-devel-2.32.10-mingw.tar.gz" -C "$WORK/sdl-windows"
}
ensure_appimage_runtime() {
    # Published by the AppImage project only as a rolling "continuous" build; the hash is the one the releases used.
    # If upstream replaced the file, set KYUJO2_APPIMAGE_RUNTIME_SHA256 to the new one's hash.
    download_checked https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 appimage-runtime-x86_64 \
        "${KYUJO2_APPIMAGE_RUNTIME_SHA256:-617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46}"
}

# ---- Android SDK and NDK ------------------------------------------------------------------------------------------
ensure_android() {
    export ANDROID_HOME="${ANDROID_HOME:-$WORK/android-sdk}"
    if [ -f "$ANDROID_HOME/ndk/27.3.13750724/build/cmake/android.toolchain.cmake" ] && [ -x "$ANDROID_HOME/build-tools/35.0.1/d8" ] \
       && [ -x "$ANDROID_HOME/build-tools/34.0.0/apksigner" ] && [ -f "$ANDROID_HOME/platforms/android-34/android.jar" ]; then
        note "Android SDK: found in $ANDROID_HOME"; return 0
    fi
    note "The Android SDK and NDK (about 1.5 GB) are installed into $ANDROID_HOME. Using them means accepting Google's"
    note "Android SDK licence (https://developer.android.com/studio/terms)."
    yes_or_stop "Accept the licence and download them?"
    download_checked https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
        android-cmdtools.zip 2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258
    if [ ! -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]; then
        mkdir -p "$ANDROID_HOME/cmdline-tools"
        rm -rf "$ANDROID_HOME/cmdline-tools/cmdline-tools"
        unzip -q "$WORK/packages/android-cmdtools.zip" -d "$ANDROID_HOME/cmdline-tools"
        mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
    fi
    local sdkm="$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager"
    yes | "$sdkm" --sdk_root="$ANDROID_HOME" --licenses > "$LOG/android-licences.log" 2>&1 || true
    quiet android-sdk "$sdkm" --sdk_root="$ANDROID_HOME" "platforms;android-34" "build-tools;34.0.0" "build-tools;35.0.1" "ndk;27.3.13750724"
}

# ---- devkitPro (Switch) -------------------------------------------------------------------------------------------
DKP_PACKAGES="devkitA64 libnx switch-tools switch-cmake switch-pkg-config switch-sdl2 switch-mesa switch-libdrm_nouveau"
dkp_ok() {
    [ -x "$1/devkitA64/bin/aarch64-none-elf-gcc" ] && [ -f "$1/cmake/Switch.cmake" ] && [ -x "$1/tools/bin/elf2nro" ] \
        && [ -f "$1/portlibs/switch/lib/libSDL2.a" ] && [ -f "$1/portlibs/switch/lib/libGLESv2.a" ] \
        && [ -f "$1/portlibs/switch/lib/libEGL.a" ] && [ -f "$1/portlibs/switch/lib/libdrm_nouveau.a" ]
}
ensure_devkitpro() {
    export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
    if dkp_ok "$DEVKITPRO"; then note "devkitPro: found in $DEVKITPRO"; return 0; fi
    [ -z "${KYUJO2_NO_SUDO:-}" ] || fail "devkitPro is needed (https://devkitpro.org/wiki/Getting_Started; packages: $DKP_PACKAGES)"
    echo "   devkitPro (the Switch homebrew toolchain) is not installed, or is missing a package."
    if ! command -v dkp-pacman >/dev/null 2>&1; then
        echo "   It is installed system-wide into /opt/devkitpro with devkitPro's own installer, which needs sudo once."
        yes_or_stop "Install devkitPro now?"
        quiet dkp-installer-download curl -fsSL -o "$WORK/install-devkitpro-pacman" https://apt.devkitpro.org/install-devkitpro-pacman
        chmod +x "$WORK/install-devkitpro-pacman"
        sudo "$WORK/install-devkitpro-pacman" || fail "devkitPro's installer failed; see https://devkitpro.org/wiki/devkitPro_pacman"
    else
        yes_or_stop "Install the missing devkitPro packages with sudo?"
    fi
    sudo dkp-pacman -S --needed --noconfirm $DKP_PACKAGES || fail "could not install $DKP_PACKAGES with dkp-pacman"
    export DEVKITPRO=/opt/devkitpro
    dkp_ok "$DEVKITPRO" || fail "devkitPro is installed but something is still missing in $DEVKITPRO"
}

# ---- your game files ----------------------------------------------------------------------------------------------
STADIUM2_SHA1=d8343e69a7dc63b869cf6361d87cde64444281d3     # Pokémon Stadium 2 (USA)
STADIUM1_SHA1=ed7bef5a306f88c0a6e96b15e71fee2ef32058f3     # Pokémon Stadium (USA): the menu's font and wallpaper
find_roms() {
    local dirs=() d f sum
    [ -n "${KYUJO2_ROMS:-}" ] && dirs+=("$KYUJO2_ROMS")
    dirs+=("$REPO/roms" "$REPO/other_projects" "$HOME/Downloads")
    KYUJO2_ROM= KYUJO_REFERENCE_ROM=
    for d in "${dirs[@]}"; do
        [ -d "$d" ] || continue
        while IFS= read -r -d '' f; do
            case $(stat -L -c %s "$f") in 67108864|33554432) ;; *) continue ;; esac
            sum=$(sha1sum "$f" | cut -c1-40)
            [ "$sum" = "$STADIUM2_SHA1" ] && [ -z "$KYUJO2_ROM" ] && KYUJO2_ROM=$f
            [ "$sum" = "$STADIUM1_SHA1" ] && [ -z "$KYUJO_REFERENCE_ROM" ] && KYUJO_REFERENCE_ROM=$f
        done < <(find -L "$d" -maxdepth 1 -type f \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*.bin' \) -print0)
    done
    [ -n "$KYUJO2_ROM" ] || fail "Pokémon Stadium 2 (USA) was not found. Put the .z64 file (SHA-1 $STADIUM2_SHA1) in the folder roms/ here (any file name)."
    [ -n "$KYUJO_REFERENCE_ROM" ] || fail "Pokémon Stadium (USA) was not found. Kyujo2's menu uses its font and wallpaper: put the .z64 file (SHA-1 $STADIUM1_SHA1) in roms/ too."
    export KYUJO2_ROM KYUJO_REFERENCE_ROM
    note "Pokémon Stadium 2: $KYUJO2_ROM"
    note "Pokémon Stadium:   $KYUJO_REFERENCE_ROM"
}

# ---- generated sources --------------------------------------------------------------------------------------------
ensure_sources() {
    local stamp="$WORK/sources.stamp" want
    want=$(cat tools/prepare-sources.sh tools/recompile-us.py tools/link-recomp-elf.sh tools/install-recompiled.py \
               tools/extra-fragments.py recomp/us*.toml patches/recompiler-*.patch patches/runtime-*.patch 2>/dev/null | sha256sum | cut -c1-64)
    if [ -f build/recompiled/us/funcs_0.c ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
        note "already prepared"; return 0
    fi
    note "fetching the decompilation and the recompiler, rebuilding the game's N64 program from it (checked against"
    note "your cartridge byte for byte) and recompiling it to C. This takes about 5 to 15 minutes, once."
    quiet prepare-sources tools/prepare-sources.sh us
    echo "$want" > "$stamp"
}

run_build() {       # platform
    note "compiling (several minutes the first time; the log is .work/logs/build-$1.log)"
    quiet "build-$1" tools/build.sh "$1"
    grep -A20 '^Built into' "$LOG/build-$1.log" | sed 's/^/   /'
}
finish() {
    local label
    label=$(sed -n 's/^set(KYUJO2_REVISION "\(.*\)" CACHE.*/\1/p' CMakeLists.txt); label=${label// /-}
    echo
    echo "Done. Your programs are in dist/$label/"
    echo "They contain the game's code: they are for you; do not share them or attach them to a release (see NOTICE.md)."
}
