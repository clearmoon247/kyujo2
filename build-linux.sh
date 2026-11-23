#!/usr/bin/env bash
# Builds Kyujo2 for Linux (AppImage and Flatpak). Run as your normal user: ./build-linux.sh   (see BUILDING.md)
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need_packages linux
step "Finding your game files"
find_roms
step "Getting the build tools for Linux (first time only)"
ensure_appimage_runtime
step "Preparing the game's sources (first time only)"
ensure_sources
step "Building Kyujo2 for Linux"
run_build linux
finish
