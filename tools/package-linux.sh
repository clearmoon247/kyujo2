#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
rev=${1:?Pass the build revision, e.g. r3}
[[ "$rev" =~ ^[A-Za-z0-9_-]+$ ]]
binary=${KYUJO2_BINARY:-build/native-linux/Kyujo2}
out="$PWD/build/packages/$rev"
mkdir -p "$out"
stage=$(mktemp -d "$out/stage.XXXXXX")
app="$stage/AppDir"
flat="$stage/flatpak"
repo="$stage/repo"
id=org.kyujo2.Kyujo2
region=$(sed -n 's/^KYUJO2_REGION:STRING=//p' "$(dirname "$binary")/CMakeCache.txt")
[[ "$region" == us || "$region" == jp ]]
if [[ "$region" == jp ]]; then id=org.kyujo2.Kyujo2.JP; fi
name="Kyujo2-${region^^}-$rev"
mkdir -p "$app/usr/"{bin,lib,share/doc/Kyujo2} "$flat/files/"{bin,share/applications,share/icons/hicolor/scalable/apps} "$flat/export/share/applications"
install -m755 "$binary" "$app/usr/bin/Kyujo2"
strip --strip-debug "$app/usr/bin/Kyujo2"
install -m755 packaging/linux/AppRun "$app/AppRun"
cp packaging/linux/org.kyujo2.Kyujo2.svg "$app/$id.svg"
sed "s/^Icon=.*/Icon=$id/" packaging/linux/org.kyujo2.Kyujo2.desktop > "$app/$id.desktop"
cp "$app/$id.svg" "$app/.DirIcon"
cp third_party/kyujo/{LICENSE,NOTICE.md,THIRD-PARTY.md} "$app/usr/share/doc/Kyujo2/"
cp .work/N64ModernRuntime/COPYING "$app/usr/share/doc/Kyujo2/N64ModernRuntime-COPYING"
cp .work/N64Recomp/LICENSE "$app/usr/share/doc/Kyujo2/N64Recomp-LICENSE"
# Staged Debian packages (KYUJO2_NO_SUDO=1 builds, .work/sysroot), or the system's own SDL2.
if [[ -n "${KYUJO2_SYSROOT:-}" ]]; then sysroot="$KYUJO2_SYSROOT/usr"
elif [[ -d .work/sysroot/usr ]]; then sysroot="$PWD/.work/sysroot/usr"
else sysroot=/usr; fi
export PATH="$sysroot/bin:$PATH"
if [[ -x "$sysroot/libexec/flatpak-validate-icon" ]]; then
    export FLATPAK_VALIDATE_ICON="${FLATPAK_VALIDATE_ICON:-$sysroot/libexec/flatpak-validate-icon}"
fi
export LD_LIBRARY_PATH="$sysroot/lib/x86_64-linux-gnu:$sysroot/lib/x86_64-linux-gnu/pulseaudio${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Bundle SDL and dependencies from the staging prefix. glibc comes from the host. With the system's SDL2 only SDL2
# itself is bundled; the libraries it uses (X11, Wayland, PulseAudio) are on every desktop.
while read -r soname path; do
    if [[ "$sysroot" != /usr && "$path" == "$sysroot/"* ]] || [[ "$soname" == libSDL2-2.0.so.0 ]]; then
        install -m644 "$path" "$app/usr/lib/$soname"
    fi
done < <(ldd "$binary" | awk '$2 == "=>" && $3 ~ /^\// { print $1,$3 }')
mksquashfs "$app" "$stage/game.squashfs" -noappend -root-owned -comp zstd -quiet
cat "${APPIMAGE_RUNTIME:-.work/packages/appimage-runtime-x86_64}" "$stage/game.squashfs" > "$out/$name.AppImage"
chmod +x "$out/$name.AppImage"
cp "$app/usr/bin/Kyujo2" "$flat/files/bin/"
cp -r "$app/usr/share/doc" "$flat/files/share/"
cp "$app/$id.svg" "$flat/files/share/icons/hicolor/scalable/apps/"
cp "$app/$id.desktop" "$flat/files/share/applications/"
cp "$app/$id.desktop" "$flat/export/share/applications/"
install -Dm644 packaging/linux/org.kyujo2.Kyujo2.png "$flat/files/share/icons/hicolor/128x128/apps/$id.png"
install -Dm644 packaging/linux/org.kyujo2.Kyujo2.png "$flat/export/share/icons/hicolor/128x128/apps/$id.png"
mkdir -p "$flat/files/lib"
cp -L "$sysroot/lib/x86_64-linux-gnu/libSDL2-2.0.so.0" "$flat/files/lib/"
cat > "$flat/metadata" <<META
[Application]
name=$id
runtime=org.freedesktop.Platform/x86_64/25.08
sdk=org.freedesktop.Sdk/x86_64/25.08
command=Kyujo2

[Context]
shared=ipc;
sockets=x11;wayland;pulseaudio;
devices=dri;input;
filesystems=xdg-download:ro;

[Environment]
LD_LIBRARY_PATH=/app/lib
META
flatpak build-export "$repo" "$flat" stable
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo "$repo" "$out/$name.flatpak" "$id" stable
