#!/usr/bin/env bash
# Builds Kyujo2 for Windows (Kyujo2.exe). Run as your normal user: ./build-windows.sh   (see BUILDING.md)
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need_packages windows
step "Finding your game files"
find_roms
step "Getting the build tools for Windows (first time only)"
ensure_sdl_mingw
step "Preparing the game's sources (first time only)"
ensure_sources
step "Building Kyujo2 for Windows"
run_build windows
finish
