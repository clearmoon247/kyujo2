# Building Kyujo2

**You need two game files to build.** Kyujo2's game code is recompiled from your own Pokémon Stadium 2 cartridge, and
its menu uses the font and wallpaper of the first Pokémon Stadium. A finished program contains the game's code, so it
is for your own use: don't share it or attach it to a release (see [`NOTICE.md`](NOTICE.md)).

| System | Command | Notes |
|---|---|---|
| [Linux](#linux) | `./build-linux.sh` | an AppImage and a Flatpak |
| [Windows](#windows) | `./build-windows.sh` | built on Linux or WSL; `Kyujo2.exe` + `SDL2.dll` |
| [Android](#android) | `./build-android.sh` | first run downloads about 1.5 GB |
| [Nintendo Switch](#nintendo-switch) | `./build-switch.sh` | installs devkitPro if needed |
| everything | `./build-all.sh` | all four |

All of these run on Debian or Ubuntu, or on Windows inside **WSL** (see [below](#on-windows-use-wsl)). Each script
installs what it needs, checks every download, and tells you in plain words if something is missing. The first run
takes 20 to 40 minutes, most of it preparing the game's sources once; later runs take a few minutes.

**1. Get the sources:**

```
git clone https://github.com/clearmoon247/kyujo2.git
cd kyujo2
```

Or download the source zip from the green **Code** button, unpack it, and open a terminal in that folder.

**2. Put your two game files in the folder `roms/`.** Any file names work; they are found by their SHA-1:

| Game | SHA-1 |
|---|---|
| Pokémon Stadium 2 (USA), `.z64` | `d8343e69a7dc63b869cf6361d87cde64444281d3` |
| Pokémon Stadium (USA), `.z64` | `ed7bef5a306f88c0a6e96b15e71fee2ef32058f3` |

**3. Run the script for your system,** for example `./build-switch.sh`. The result is in `dist/English-r26/` (the
folder name follows the version).

**Run the scripts as your normal user, without `sudo`.** A script asks for your password itself, only when it has to
install something: missing Debian packages, or devkitPro for the Switch build. If a script says "Permission denied"
(some unzip tools drop the executable flag), run it as `bash ./build-switch.sh`, or run `chmod +x *.sh` once.

## Linux

```
./build-linux.sh
```

It writes `dist/English-r26/Linux/Kyujo2-US-English-r26.AppImage` and a `.flatpak`.
- **AppImage:** `chmod +x Kyujo2-US-*.AppImage` and start it. If it asks for FUSE, run `sudo apt install libfuse2`
  once.
- **Flatpak:** `flatpak install --user Kyujo2-US-*.flatpak` (needs `org.freedesktop.Platform` 25.08 from Flathub), then
  `flatpak run org.kyujo2.Kyujo2`.

On first start, choose your Pokémon Stadium 2 file.

## Windows

```
./build-windows.sh
```

This builds on Linux or WSL with MinGW and writes `dist/English-r26/Windows/Kyujo2.exe` and `SDL2.dll`. Copy both to
the same folder on Windows and start `Kyujo2.exe`. On first start, choose your Pokémon Stadium 2 file. Settings and
saves go to `%APPDATA%\Kyujo2\Kyujo2`.

## Android

```
./build-android.sh
```

The script asks once to accept Google's Android SDK licence (or run it with `KYUJO2_YES=1`). It then downloads the
SDK and NDK into `.work/android-sdk` and writes `dist/English-r26/Android/Kyujo2-US-English-r26.apk`. Install it on
your phone: copy it over and open it, allowing "install unknown apps", or use `adb install`. Open Kyujo2 and pick your
Pokémon Stadium 2 file. It runs on arm64 phones with Android 7 or newer.

Android installs an update only if it is signed with the same key. The first build makes a key in
`.work/kyujo2-debug.jks`: keep that file. To sign with your own key, set `KYUJO2_ANDROID_KEYSTORE`,
`KYUJO2_ANDROID_KEYSTORE_PASS`, `KYUJO2_ANDROID_KEY_ALIAS` and `KYUJO2_ANDROID_KEY_PASS`.

## Nintendo Switch

```
./build-switch.sh
```

The Switch build needs **devkitPro**, the Switch homebrew toolchain.
- **Not installed:** the script offers to install it with devkitPro's own installer. It asks for `sudo` once and
  installs into `/opt/devkitpro`.
- **Already installed** (`DEVKITPRO` set, or `/opt/devkitpro`): the script uses that.

It writes `dist/English-r26/Switch/Kyujo2-US-English-r26.nro`, plus a matching `.elf` that resolves crash addresses.

On the SD card:
- Copy the `.nro` as `switch/Kyujo2/Kyujo2.nro`.
- Copy your Pokémon Stadium 2 file as `switch/Kyujo2/stadium2-us.z64`.

Start Kyujo2 from the Homebrew Menu in **application mode** (hold R while starting a game), not applet mode.

## On Windows: use WSL

The scripts need Linux. On Windows 10/11 that means WSL:
1. Open PowerShell as administrator and run `wsl --install` (it installs Ubuntu).
2. Restart, then open **Ubuntu** from the Start menu.
3. Follow the steps above inside it.

Your Windows files are under `/mnt/c/...`, but build in your Linux home folder (`~`): it is much faster. To copy the
finished files out, open the folder in Windows Explorer with `explorer.exe dist`.

## Settings you will rarely need

All optional, set in front of the command, for example `KYUJO2_YES=1 ./build-all.sh`:
- `KYUJO2_ROMS=<folder>`: where your game files are. The default is `roms/`, then `~/Downloads`.
- `KYUJO2_DECOMP=<folder or zip>`: use a copy of the Pokémon Stadium 2 decompilation you already have. By default it
  is fetched from [team-hippodrome/pokestadiumgs](https://github.com/team-hippodrome/pokestadiumgs) at the tested
  commit.
- `KYUJO2_JOBS=<n>`: parallel jobs. The default comes from your CPU count and free memory.
- `KYUJO2_YES=1`: answer "yes" to the questions (installing packages, Android licence, devkitPro).
- `KYUJO2_NO_SUDO=1`: never use sudo. The Debian packages are unpacked into `.work/sysroot` instead; only the basic
  tools (git, clang, cmake, python3, ...) must already be installed.
- `ANDROID_HOME=<folder>`, `DEVKITPRO=<folder>`: an Android SDK or devkitPro you already have.

Downloaded tools, logs and intermediate files go into `.work/` (logs in `.work/logs/`), `source/` and `build/`. Delete
them to start fresh.

## For developers

The scripts above call three tools you can also run yourself:
- `tools/prepare-sources.sh us`: fetch the decompilation, N64Recomp and N64ModernRuntime at their pinned commits, apply
  `patches/`, rebuild the game's N64 program (checked against your cartridge byte for byte), and recompile it to C in
  `build/recompiled/us` and `build/rsp`.
- `tools/build.sh <linux|windows|android|switch>`: configure, build and package one platform. The build directories
  are `build/native-linux`, `build/native-windows`, `build/game-android` and `build/game-switch`.
- `tools/package-linux.sh`, `tools/package-android.sh`: the packaging steps.

Useful CMake options:
- `KYUJO2_GPU`: OpenGL ES 3 renderer, ON for Switch.
- `KYUJO2_REGION`: `us`, or `jp` for the experimental Japanese build, prepared with `tools/prepare-sources.sh jp`.
- `KYUJO_REFERENCE_ROM`: path of the Stadium 1 file.

Run-time switches:
- `KYUJO2_RENDERER=software`: turn the GPU off. On the Switch, put a `software-renderer` file in the folder instead.
- `KYUJO2_PERF=1`: speed lines every 2 s. Always on for the Switch.
- `KYUJO2_CAPTURE_DIR=<dir>`: frame captures.

The regression tests are `Kyujo2*Test` targets; see [`docs/DEVELOPMENT-HISTORY.md`](docs/DEVELOPMENT-HISTORY.md).

| Path | Contents |
|---|---|
| `native/` | Kyujo2's host code |
| `patches/` | changes to N64Recomp, N64ModernRuntime and Kyujo |
| `recomp/` | recompiler configuration |
| `third_party/kyujo/` | the Kyujo files Kyujo2 uses, unmodified |
| `tools/` | build scripts and helpers |
| `packaging/` | Android, Linux and Switch packaging |
| `tests/` | regression programs |
