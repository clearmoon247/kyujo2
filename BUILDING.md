# Building Kyujo2

The build runs on **Debian 13 (trixie), x86-64**. It makes all four programs from one machine: Linux natively, Windows
with MinGW-w64, Android with the NDK, and Switch with devkitPro. Other distributions may work, but the setup script
uses Debian package names.

The process has three steps: set up the host (once), prepare the sources from your cartridge (once per checkout),
then build.

## 1. Host setup
```sh
tools/setup-host.sh --apt --android
```
- `--apt` installs the Debian packages with `sudo apt-get`. Without root, use `--local`: it downloads the same
  packages and unpacks them into `.work/sysroot`. The base development tools (compilers, CMake, Python, JDK) must
  still be installed, and the script names any that are missing.
- `--android` also installs Android SDK platform 34, build tools 34.0.0 and 35.0.1, and NDK 27.3.13750724 into
  `.work/android-sdk`. You accept Google's SDK licences when you do.
- It downloads SDL 2.32.10 (source for Android, MinGW package for Windows) and the AppImage runtime. Every download
  is checked against a pinned SHA-256.
- **Switch:** install devkitPro (https://devkitpro.org/wiki/Getting_Started), then
  `sudo dkp-pacman -S devkitA64 libnx switch-tools switch-cmake switch-pkg-config switch-sdl2 switch-mesa switch-libdrm_nouveau`.
  The releases were built with devkitA64 r30 (GCC 16.1), libnx 4.12.0, switch-sdl2 2.28.5 and switch-mesa 20.1.0.

## 2. Sources
Put these files in `other_projects/` (the folder is ignored by git; nothing in it may be published):

| File | SHA |
|---|---|
| `Pokemon Stadium 2 (USA).z64` | SHA-1 `d8343e69a7dc63b869cf6361d87cde64444281d3` |
| `Pokemon Stadium (USA).z64` (menu artwork, build time only) | SHA-1 `ed7bef5a306f88c0a6e96b15e71fee2ef32058f3` |
| `pokestadiumgs-master.zip`: the Pokémon Stadium 2 decompilation at commit `c0e10f23d90cc4f335b654711f13e53c2c07323b` | SHA-256 `bc655898f627dbd239f3384721aa47c6ba9f51155979091b25a849c2decfc1ff` |
| `Kyujo-main.zip`: Kyujo at commit `7b66b752ec364e015296a22c88409116827ee09e` | SHA-256 `f8117dbc84077b3604e0789830f0338e0e40b23e53289ccffcd6984447653bc5` |

Then:
```sh
tools/prepare-sources.sh us
```
This does the following, in order:
1. Checks the files.
2. Unpacks the decompilation and Kyujo into `source/`.
3. Rebuilds the game's N64 program from the decompilation and checks that it matches your cartridge byte for byte.
4. Fetches N64Recomp (`598ce2080004a5672d604dceadbda436045307b7`) and N64ModernRuntime
   (`e4342ae030a9d94aa97a81aafd3368ed7daa74df`) from GitHub and applies the patches in `patches/`.
5. Builds the recompiler and generates the game's C code into `build/recompiled/us` and the RSP microcode programs
   into `build/rsp`.

It needs network access for the Git checkouts and for the decomp's Python requirements, which are installed into
`.work/venv`.

## 3. Build
```sh
tools/build.sh all                 # or any of: linux windows android switch
```
Results go to `dist/<label>/`, where the label comes from `CMakeLists.txt`, for example `English-r26`:

| Folder | Files |
|---|---|
| `Linux/` | `Kyujo2-US-<label>.AppImage`, `Kyujo2-US-<label>.flatpak` |
| `Windows/` | `Kyujo2.exe`, `SDL2.dll` |
| `Android/` | `Kyujo2-US-<label>.apk` |
| `Switch/` | `Kyujo2-US-<label>.nro`, and the matching unstripped `.elf` (resolves crash addresses) |

Options:
- `--label NAME` sets your own version label.
- `--jobs N` limits parallel compiles.

The intermediate build directories are `build/native-linux`, `build/native-windows`, `build/game-android` and
`build/game-switch`. After a change, running `tools/build.sh` again rebuilds only what changed.

**Android signing:** Android only installs an update over an existing app that was signed with the same key. Without
settings, `tools/package-android.sh` makes a test key once in `.work/kyujo2-debug.jks`; keep that file. To use your
own key, set `KYUJO2_ANDROID_KEYSTORE`, `KYUJO2_ANDROID_KEYSTORE_PASS`, `KYUJO2_ANDROID_KEY_ALIAS` and
`KYUJO2_ANDROID_KEY_PASS`.

## Build settings worth knowing
- **CMake options:**
  - `KYUJO2_REGION`: `us`, or `jp` for the experimental Japanese build. `tools/build.sh` builds US only; JP needs
    `tools/prepare-sources.sh jp` and a manual configure with `-DKYUJO2_REGION=jp`.
  - `KYUJO2_GPU`: OpenGL ES 3 renderer. ON by default for Switch only.
  - `KYUJO2_REVISION`: label written to the log.
  - `KYUJO_REFERENCE_ROM`: Stadium 1 cartridge path.
- **GPU renderer on Linux (testing):** configure `build/native-linux` with `-DKYUJO2_GPU=ON -DGLES_ROOT=<prefix>`,
  where the prefix holds `include/GLES3` and `libGLESv2`.
- **Run-time switches:**
  - `KYUJO2_RENDERER=software` turns the GPU off; on Switch, use the `software-renderer` file instead.
  - `KYUJO2_PERF=1` logs speed lines every 2 s in GPU mode; always on for Switch.
  - `KYUJO2_TRACE_PRESENT=1`, `KYUJO_GPU_SYNCTRACE=1` and `KYUJO2_TRACE_BATTLE=1` are diagnostics.
  - `KYUJO2_CAPTURE_DIR=<dir>` saves periodic frame captures.

## Repository layout
| Path | Contents |
|---|---|
| `native/` | the port's host code: desktop/Android/Switch shell, shared menu glue, Transfer Pak, cheats, guest hooks |
| `patches/` | changes to N64Recomp, N64ModernRuntime, Kyujo's renderer and menu, applied at prepare/configure time |
| `recomp/` | N64Recomp configuration: function sizes, hooks and RSP microcode descriptions |
| `tools/` | setup, prepare, build and packaging scripts, recompiler helpers |
| `packaging/` | Android Java layer and resources, Linux desktop files and icons, Switch icon |
| `tests/` | regression programs (built as `Kyujo2*Test` targets; see docs/DEVELOPMENT-HISTORY.md) |

Generated or downloaded material never goes into git: `.work/`, `source/`, `build/`, `dist/` and `other_projects/`
are ignored.
