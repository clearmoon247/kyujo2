#!/usr/bin/env bash
# Builds Kyujo2 for Nintendo Switch (NRO). Run as your normal user: ./build-switch.sh   (see BUILDING.md)
. "$(dirname "${BASH_SOURCE[0]}")/tools/build-lib.sh"
STEP_TOTAL=5

step "Checking the tools"
need_packages switch
step "Finding your game files"
find_roms
step "Getting the build tools for Switch (first time only)"
ensure_devkitpro
step "Preparing the game's sources (first time only)"
ensure_sources
step "Building Kyujo2 for Switch"
run_build switch
finish
