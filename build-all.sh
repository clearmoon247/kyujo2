#!/usr/bin/env bash
# Builds Kyujo2 for Linux, Windows, Android and Switch in one go. Run as your normal user: ./build-all.sh
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need_packages linux windows android switch
step "Finding your game files"
find_roms
step "Getting the build tools (first time only)"
ensure_appimage_runtime; ensure_sdl_mingw; ensure_sdl_source; ensure_android; ensure_devkitpro
step "Preparing the game's sources (first time only)"
ensure_sources
step "Building Kyujo2 for every platform"
for p in linux windows android switch; do run_build "$p"; done
finish
