# Kyujo2 development history

The notes below were written while the port was being brought up (r1 to r13). They are kept for the regression
tests and the background they describe; some paths and commands are out of date. README.md and BUILDING.md are current.

The active Kyujo2 release is the English native port of Pokémon Stadium 2 (USA). Use the USA cartridge for this build. English r10 reached the second Poké Cup trainer on Windows and the Linux Flatpak. The Windows build also reloaded six registered Pokémon after a restart. Full campaign progression, other save paths, audio fidelity, and physical Android and Switch execution still need confirmation.

## Build the guest

Place the supplied Kyujo-main.zip and pokestadiumgs-master(1).zip in other_projects alongside your region's cartridge. The setup script checks their hashes. Use a Debian host with Bash, Git, unzip, Clang, CMake 3.20+, Ninja, GNU patch, make, Python with venv, and binutils-mips-linux-gnu. The Python requirements include the decomp's pinned spimdisasm revision. The initial build fetches the N64Recomp and N64ModernRuntime revisions listed in the setup script.

```sh
tools/prepare-sources.sh us
```

The setup keeps the regional assembly, map and N64 ELF in separate decomp trees. Both the original rebuild and the section-preserving ELF used for native generation must reproduce the cartridge byte-for-byte. For JP the script makes an additional working tree, applies the 88 boundaries in recomp/jp-code-ends.txt and separates the last overlay from the following cartridge assets. Both region commands generate CPU sources and their audio and JPEG RSP programs. The RSP microcode bytes match across cartridges.

Use separate CMake build directories for each region. Pass -DKYUJO2_REGION=jp to build the Japanese program. The default region is us.

## Desktop

The renderer uses Kyujo's F3DEX2 and software RDP code with Kyujo2 word-order adapters. Supply SDL2 for the target architecture. Linux and Windows x86-64 require SSE4.1 for the recompiled RSP vector code.

```sh
CC=clang CXX=clang++ cmake -GNinja -S . -B build/native-linux -DBUILD_RUNTIME_TRACE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-linux --target Kyujo2
build/native-linux/Kyujo2 'other_projects/Pokemon Stadium 2 (USA).z64'
```

For the workspace-local Debian libraries, set SDL_ROOT to .work/sysroot/usr at configure time and add its lib/x86_64-linux-gnu and lib/x86_64-linux-gnu/pulseaudio directories to LD_LIBRARY_PATH for linking and running. For Windows use x86_64-w64-mingw32 GCC with POSIX threads and the 64-bit SDL2 MinGW development package. On a case-sensitive Debian host the runtime includes Windows.h. The example creates a project-local forwarding header.

```sh
mkdir -p .work/windows-includes
printf '#include <windows.h>\n' > .work/windows-includes/Windows.h
sdl="$PWD/.work/sdl-windows/SDL2-2.32.10/x86_64-w64-mingw32"
cmake -GNinja -S . -B build/native-windows -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/mingw-toolchain.cmake" \
  -DBUILD_RUNTIME_TRACE=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-I$PWD/.work/windows-includes" \
  -DSDL_HEADERS="$sdl/include/SDL2" -DSDL_LIB="$sdl/lib/libSDL2.dll.a"
cmake --build build/native-windows --target Kyujo2
cp "$sdl/bin/SDL2.dll" build/native-windows/
```

Launch with the matching ROM path, or drop a ROM on the initial desktop window. The runtime saves under the per-user Kyujo2 directory. An optional second argument selects a private data directory. Keyboard controls include Z for A, X for B, Enter for Start, arrow keys for the stick, WASD for the D-pad, IJKL for the C buttons, C for Z, and Q/E for L/R. An existing KYUJO2_CAPTURE_DIR enables periodic framebuffer captures.

tools/package-linux.sh accepts a revision label. It packages the executable under build/native-linux with the staged Debian SDL2 libraries, squashfs-tools, Flatpak and official AppImage type-2 runtime in .work/packages/appimage-runtime-x86_64. Set KYUJO2_BINARY to the regional executable when packaging a different build directory. It produces an x86-64 AppImage and a Flatpak bundle for org.freedesktop.Platform 25.08 under build/packages.

## Android

The game APK uses SDL2 for Android, a document picker for importing the matching .z64 and app-private game storage. Install SDK platform 34, build-tools 34.0.0 and 35.0.1, NDK 27.3.13750724 and a JDK. D8 from 35.0.1 compiles the SDL activities. API 24 is the minimum. Extract SDL2 2.32.10 to .work/SDL2-2.32.10 and use the SDK under .work/android-sdk. From the project root run:

```sh
ndk="$PWD/.work/android-sdk/ndk/27.3.13750724"
cmake -GNinja -S .work/SDL2-2.32.10 -B .work/sdl-android \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=24 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 \
  -DSDL_STATIC=OFF -DSDL_TEST=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .work/sdl-android
cmake -GNinja -S . -B build/game-android -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 -DANDROID_STL=c++_static -DBUILD_RUNTIME_TRACE=ON \
  -DSDL_HEADERS="$PWD/.work/SDL2-2.32.10/include" -DSDL_LIB="$PWD/.work/sdl-android/libSDL2.so"
cmake --build build/game-android --target Kyujo2
tools/package-android.sh English-r10
```

For a JP APK configure a separate native build with -DKYUJO2_REGION=jp and set KYUJO2_ANDROID_BUILD to its path when running the packaging script. The two regions have distinct Android application IDs. The packaging script creates a local test signing key. Preserve that key to install later previews as updates on the same phone.

## Boot regression and diagnostics

tools/build-boot-test.sh takes linux, windows, android, or switch. The diagnostic executes the generated guest entry, checks the BSS and initial stack, tests the floating-point entry and jump-table cases, and replays the failing store through the uncached RDRAM alias. Main is a test double. The Switch build uses devkitA64 and libnx and emits an NRO, matching unstripped ELF and linker map. For a Switch crash, use the relative program counter from the crash report and the exact revision's ELF with aarch64-none-elf-addr2line -fC -e.

The decomp source revision is c0e10f23d90cc4f335b654711f13e53c2c07323b and Kyujo is 7b66b752ec364e015296a22c88409116827ee09e. The working directory has no Git commit history. The delivery checksum manifests identify the port source snapshot and individual binaries. The original reference source archives supply their upstream licensing and notices.

## Switch runtime preview

The full-game NRO uses devkitA64, libnx, portlibs SDL2 and the patched static runtime. Point DEVKITPRO and DEVKITA64 at the installed toolchains and configure in a separate directory.

```sh
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
cmake -GNinja -S . -B build/game-switch -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DBUILD_RUNTIME_TRACE=ON -DCMAKE_BUILD_TYPE=Debug \
  -DSDL_HEADERS="$DEVKITPRO/portlibs/switch/include/SDL2" -DSDL_LIB="$DEVKITPRO/portlibs/switch/lib/libSDL2.a"
cmake --build build/game-switch
```

Place the US .z64 at sdmc:/switch/Kyujo2/stadium2-us.z64, or the Japanese .z64 at sdmc:/switch/Kyujo2/stadium2-jp.z64, then launch the NRO from hbmenu in application mode. The runtime also uses this folder for its saves. The no-ROM path displays the expected SD path and exits on +. Device execution and the save path still need confirmation on the Switch.

Runtime preview r4 consumes US audio tasks before the submitting guest thread can reuse their buffers. It also supports one SDL game controller. Physical Android and Switch results are pending.

## US title preview r5

The guest polls one counter at 0x80035534 and 0x80035580. Yielding at both branches allows the runtime to deliver the interrupts that advance that counter. The next scene submits the JPEG microcode linked at 0x80085270. The host runs its static recompilation and the recompiled audio tasks before returning ownership of their buffers to the guest. The title screenshot was captured from the software renderer on Linux.

To repeat the packed scene-asset regression after configuring the Linux build, run the following from the project root.

```sh
clang -Inative -Ibuild/native-linux/guest-include tests/asset_entry.c native/asset_entry.c -lm -o build/native-linux/test-asset-entry
build/native-linux/test-asset-entry
```

## US scene preview r6

In r5, leaving the title idle triggered a guest call from 0x8410A570 to 0x80262CE0, the header of a loaded scene asset. This asset has a three-way accessor. The host now recognizes its full instruction sequence including its relocated pointer, and preserves the guest register and stack effects for all three selectors. The sanitizer regression in tests/asset_entry.c replays the words captured in GDB. An unattended Linux run then rendered a battle demonstration in the stadium. Start and A input in the r5 window had also reached the greeting scene.

## Battle preview r7

The guest discards each fragment's relocation table when it finishes loading. A battle-demo unload in r6 used the full ROM length of one fragment and counted part of its live neighbour as overlapping. r7 registers and releases the exact loaded section and base. Run the regression with the native desktop toolchain.

```sh
cmake --build build/native-linux --target Kyujo2OverlayTest
build/native-linux/Kyujo2OverlayTest
```

The JP cart SHA-1 is 05682e60b13479ca1c54656e5a5b1ee6d099c1a4. The US cart SHA-1 is d8343e69a7dc63b869cf6361d87cde64444281d3. The Switch ELF and NRO for a given region and revision share a build ID. Use the module-relative PC from a device report with that ELF and include the register dump and the full raw crash report.

Android r7a uses 16 KiB ELF load-segment alignment for both SDL2 and Kyujo2. Its Android package version code is 8.

## Menu preview r8

The US greeting replay in r7 stopped in the loop at guest PC 0x80003BE8 while the asset loader waited for another guest thread. The corresponding Japanese branch is 0x80003BC8. r8 yields when that branch is taken and leaves the completion condition to the guest. The US Linux replay progressed into White City, then the Stadium and Poké Cup selection screens. The Android package includes the 16 KiB aligned SDL2 build from r7a.

For the English completion-wait regression, build Kyujo2CompletionTest in the Linux build directory and run it with a five-second timeout. It executes the generated guest polling function on busy and ready inputs.

The English Linux AppImage and Windows executable advanced from the title through the greeting to Poké Cup team registration using Start and A. The Windows executable also rendered a stadium battle in attract mode. See the English r8 delivery report for package checks, captures and physical-device status.

The Linux packaging script exports the project icon and requests Flatpak input-device access for SDL controllers. A standard Flatpak installation supplies the icon validator and GdkPixbuf under /usr. When staging those tools under a private prefix, make the validator and its GdkPixbuf library visible inside Flatpak's icon-validation namespace. FLATPAK_VALIDATE_ICON can select the staged validator.

## English Poké Cup preview r9

Confirming a rental team in r8 exposed a missing relocation for the end of an eight-entry table in fragment 20. The generated guest loop stepped past the eight entries. The cartridge marks both instructions forming the end pointer as local relocations. The decompilation ELF had marked their target as absolute. English source generation now recovers local overlay BSS relocations from the cartridge table and records them in a separate recompilation ELF. Converting that ELF back to a binary reproduces the US cartridge.

Run the generated table initializer regression at two load addresses with the following commands.

```sh
cmake --build build/native-linux --target Kyujo2CupTableTest
timeout 5 build/native-linux/Kyujo2CupTableTest
```

## English cup progression r10

Advancing past the first Windows Poké Cup victory in r9 reached another guest routine that reads PI_STATUS_REG and then a word at cartridge offset 0x800. English r10 routes those reads and the same operations at two other verified US call sites through the native PI and cartridge APIs. The guest retains its own status test, expected-value comparison, interrupt-mask handling and return values.

Build Kyujo2CartProbeTest in the Linux build directory and run it to exercise busy, matching and mismatching cartridge values in all three generated routines. The shared delivery report includes the original Windows fault offset and the exact corresponding r9 symbols.

## English minigame relocation r11

In an unattended English title demo, fragment 50 passed a list in fragment 64 to the scene initializer. The original cartridge relinks both instructions forming that pointer. The port now uses the loaded fragment 64 base for the two instructions and applies the same rule to cartridge-confirmed external overlay BSS references. Build and run Kyujo2MinigameLinkTest to check the generated caller with two independent pairs of load addresses.

## English Windows menus and battle controls (r13)

Open `Windows/Kyujo2.exe` with SDL2.dll beside it. Select the USA Stadium 2 ROM on first launch, then configure the four Transfer Pak slots in **Game Pak Setup** or select **Start game**. A Game Boy ROM with a matching .sav imports both files. Each cartridge has a private working save. The host can import 32 KiB SRAM with a 44- or 48-byte RTC trailer. RTC registers and GB Tower gameplay still require work or validation.

The standalone executable draws the game menu using Kyujo's Game Pak Check tile, IA glyphs and original menu drawing code. At build time `tools/kyujo-menu-assets.py` reads the two font sizes and the wallpaper tile from the supplied US Stadium 1 reference cartridge. The default build path is `other_projects/Pokemon Stadium (USA).z64`. Set the CMake cache variable `KYUJO_REFERENCE_ROM` to select another path. End users need only the English Stadium 2 cartridge.

Open the menu with F1, Esc or Share on a DualShock 4. Use the arrows, D-pad or left stick to select a row and Enter, a mouse click or Cross to choose it. **Controllers** lists the four ports and the keyboard. Select a port to assign a device, bind an N64 action and see live input. **Options** controls volume, full screen and the two optional battle cheats. **Game Paks** changes cartridges on the next restart.

L + R arms the knockout until a damaging move hits the opponent. The US battle overlay applies the requested damage before its own HP clamp, subtraction and faint handling. Z sets the first party Pokémon's HP to 255. Both switches start disabled.

Build `Kyujo2WindowsInputTest` and run it on Windows with a disposable settings JSON path. Its optional `--listen` argument samples physical Cross and Options. `Kyujo2BattleCheatsTest` exercises the armed chord, the target and damage checks, bench slots and first-party HP. `tests/windows_setup.ps1` runs the Windows menu regression with isolated profiles, two Game Boy cartridges and an incorrect N64 cartridge to test rejection. Supply Windows paths in KYUJO2_TEST_EXE, KYUJO2_TEST_DIR, KYUJO2_DATA_DIR, KYUJO2_CAPTURE_DIR, KYUJO2_TEST_ROM, KYUJO2_TEST_WRONG_ROM, KYUJO2_TEST_GB and KYUJO2_TEST_GB2.

## English Android touch controls (r13)

The SDL game activity mounts Kyujo's TouchOverlay above its surface at startup. The overlay shows a digital pad, A/B, Start, shoulders, Z and four C buttons. Tap **Menu** to switch between the digital pad and analog stick. Pairing a physical gamepad hides the overlay, disconnecting it restores the overlay, and pausing the activity releases the inputs. A JNI adapter maps the touch bitmask and axes to Kyujo2's player-one input callback. The native regression in `tests/android_touch.cpp` can also run on a build host with JDK JNI headers.
