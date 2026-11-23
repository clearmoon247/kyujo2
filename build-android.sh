#!/usr/bin/env bash
# Builds Kyujo2 for Android (APK). Run as your normal user: ./build-android.sh   (see BUILDING.md)
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need_packages android
step "Finding your game files"
find_roms
step "Getting the build tools for Android (first time only)"
ensure_sdl_source; ensure_android
step "Preparing the game's sources (first time only)"
ensure_sources
step "Building Kyujo2 for Android"
run_build android
finish
