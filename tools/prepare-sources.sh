#!/usr/bin/env bash
# Run from Debian with CMake, Ninja, Clang, binutils-mips-linux-gnu, Python/venv and Git.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
region=${1:-us}
case "$region" in
    us) parent=source; rom='Pokemon Stadium 2 (USA).z64'; sha=d8343e69a7dc63b869cf6361d87cde64444281d3 ;;
    jp) parent=source/jp; rom='Pokemon Stadium Kin Gin (Japan).z64'; sha=05682e60b13479ca1c54656e5a5b1ee6d099c1a4 ;;
    *) echo 'Expected us or jp' >&2; exit 2 ;;
esac
mkdir -p "$parent" .work
printf '%s  %s\n' "$sha" "other_projects/$rom" | sha1sum -c -
# The decomp archive may carry a browser's duplicate-download suffix; either name is accepted, the hash decides.
decomp_zip='other_projects/pokestadiumgs-master.zip'
[[ -f "$decomp_zip" ]] || decomp_zip='other_projects/pokestadiumgs-master(1).zip'
printf '%s  %s\n' bc655898f627dbd239f3384721aa47c6ba9f51155979091b25a849c2decfc1ff "$decomp_zip" \
    f8117dbc84077b3604e0789830f0338e0e40b23e53289ccffcd6984447653bc5 'other_projects/Kyujo-main.zip' | sha256sum -c -
unzip -nq "$decomp_zip" -d "$parent"
unzip -nq 'other_projects/Kyujo-main.zip' -d source
decomp="$root/$parent/pokestadiumgs-master"
base="$decomp/baseroms/$region/baserom.z64"
if [[ ! -e "$base" ]]; then ln -s "$root/other_projects/$rom" "$base"; fi
printf '%s  %s\n' "$sha" "$base" | sha1sum -c -
if [[ ! -x .work/venv/bin/python ]]; then
    # Debian ships ensurepip in python3-venv. Without it (a host set up with tools/setup-host.sh --local), make the venv
    # without pip and install pip from Debian's pip wheel, which setup-host stages.
    if python3 -c 'import ensurepip' 2>/dev/null; then python3 -m venv .work/venv
    else python3 -m venv --without-pip .work/venv; fi
fi
python="$root/.work/venv/bin/python"
if ! "$python" -m pip --version >/dev/null 2>&1; then
    wheel=$(ls .work/sysroot/usr/share/python-wheels/pip-*.whl /usr/share/python-wheels/pip-*.whl 2>/dev/null | head -1 || true)
    [[ -n "$wheel" ]] || { echo 'pip is missing: install python3-venv (or run tools/setup-host.sh --local)' >&2; exit 1; }
    "$python" "$wheel/pip" install --no-index "$wheel"
fi
if ! "$python" -c 'import splat,spimdisasm,png,yaml' 2>/dev/null; then
    "$python" -m pip install -r "$decomp/requirements.txt"
fi
if [[ -d "$root/.work/sysroot/usr/bin" ]]; then
    export PATH="$root/.work/sysroot/usr/bin:$PATH"
    export LD_LIBRARY_PATH="$root/.work/sysroot/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
(
    cd "$decomp"
    cat "yamls/$region/header.yaml" "yamls/$region/rom.yaml" > "pokestadiumgs-$region.yaml"
    "$python" -m splat split "pokestadiumgs-$region.yaml"
    make -j"${JOBS:-6}" VERSION="$region" PYTHON="$python" rom
    cmp "$base" "build/pokestadiumgs-$region.z64"
)
if [[ "$region" == jp ]]; then
    # Keep the reference decomp and its matching ELF separate from the native overlay split.
    mkdir -p .work/jp-linked
    unzip -nq "$decomp_zip" -d .work/jp-linked
    export KYUJO2_JP_DECOMP="$root/.work/jp-linked/pokestadiumgs-master"
    jp_base="$KYUJO2_JP_DECOMP/baseroms/jp/baserom.z64"
    if [[ ! -e "$jp_base" ]]; then ln -s "$root/other_projects/$rom" "$jp_base"; fi
    printf '%s  %s\n' "$sha" "$jp_base" | sha1sum -c -
    "$python" tools/jp-overlay-layout.py "$KYUJO2_JP_DECOMP"
    (cd "$KYUJO2_JP_DECOMP"
     "$python" -m splat split pokestadiumgs-jp.yaml
     make -j"${JOBS:-6}" VERSION=jp PYTHON="$python" rom
     cmp "$jp_base" build/pokestadiumgs-jp.z64)
    decomp="$KYUJO2_JP_DECOMP"
fi

checkout() {
    local name=$1 revision=$2 patch_name=$3
    local dir="$root/.work/$name"
    if [[ ! -d "$dir/.git" ]]; then
        git clone --no-checkout "https://github.com/N64Recomp/$name.git" "$dir"
        git -C "$dir" checkout --detach "$revision"
    fi
    [[ $(git -C "$dir" rev-parse HEAD) == "$revision" ]] || { echo "Unexpected $name version" >&2; return 1; }
    git -C "$dir" submodule update --init --recursive
    if ! git -C "$dir" apply --reverse --check "$root/patches/$patch_name" 2>/dev/null; then
        git -C "$dir" apply "$root/patches/$patch_name"
    fi
}
checkout N64Recomp 598ce2080004a5672d604dceadbda436045307b7 recompiler-disjoint-overlays.patch
for name in recompiler-handwritten-code.patch; do
    if ! git -C .work/N64Recomp apply --reverse --check "$root/patches/$name" 2>/dev/null; then
        git -C .work/N64Recomp apply "$root/patches/$name"
    fi
done
checkout N64ModernRuntime e4342ae030a9d94aa97a81aafd3368ed7daa74df runtime-platforms.patch
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-stadium-save.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-stadium-save.patch"
fi

if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-transfer-pak.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-transfer-pak.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-vi-scale.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-vi-scale.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-rsp-dma.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-rsp-dma.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-yield-ready.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-yield-ready.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-switch-timer.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-switch-timer.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-switch-files.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-switch-files.patch"
fi
if ! git -C .work/N64ModernRuntime apply --reverse --check "$root/patches/runtime-switch-threads.patch" 2>/dev/null; then
    git -C .work/N64ModernRuntime apply "$root/patches/runtime-switch-threads.patch"
fi
CC=clang CXX=clang++ cmake -GNinja -S .work/N64Recomp -B .work/recompiler-build -DCMAKE_BUILD_TYPE=Release
cmake --build .work/recompiler-build --target N64Recomp RSPRecomp -j"${JOBS:-6}"
tools/link-recomp-elf.sh "$region"
"$python" tools/recompile-us.py --region "$region" --decomp "$decomp"
mkdir -p build/rsp
.work/recompiler-build/RSPRecomp "recomp/$region-audio.toml"
.work/recompiler-build/RSPRecomp "recomp/$region-jpeg.toml"
if [[ $region == us ]]; then
    for program in us-gbcore us-gbcore-a us-gbcore-b; do .work/recompiler-build/RSPRecomp "recomp/$program.toml"; done
fi
