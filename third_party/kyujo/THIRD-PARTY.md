# Third-party components

Everything the project builds with, links into a program, or learned from, with its licence. Our own code is **GPL-3.0-only** (`LICENSE`, `NOTICE.md`); the components below keep their own licences. Licences were read from the pinned sources or the release packages on the dates given; where nothing was checked, the table says so. The maintainers are not lawyers: this is a record of what was found, not legal advice.

## A. Inside a program the port builds

| Component | Where it ends up | Licence | Status |
|---|---|---|---|
| **musl libc** (kraj/musl, commit `bd54dc3e…`) | Linux program: the C library, linked statically | MIT | Read from the source tree 2026-10-24 |
| **mingw-w64** run-time (start-up objects `crt2.o`, `crtbegin.o`, `libmingw32`, `libmingwex`, the UCRT import library) | Windows exe: linked in | Zope Public License 2.1, with parts public domain, BSD and **LGPL** (per its `COPYING`) | **Not audited**: which LGPL parts, if any, reach a built exe is not known. Do this before distributing any binary (none is distributed) |
| **compiler-rt builtins** (LLVM 18.1.3, `compiler-rt/lib/builtins`; the llvm-mingw package carries its own copy) | Both programs: 64-bit arithmetic helpers | Apache-2.0 with LLVM exception | Read 2026-10-24 |
| **Windows system libraries**: `KERNEL32`, `USER32`, `GDI32`, `WINMM`, `COMDLG32`, the `api-ms-win-crt-*` set (Windows' own C runtime) | Windows exe: **imported at run time, not included** | Part of Windows | Import list read from the exe (`pe_check.py` checks it) |
| **XInput** (`xinput1_4.dll`, then `xinput9_1_0.dll`) | Windows exe: loaded at run time if present; the program runs without it (no Xbox-layout pads) | Part of Windows | `host_window.c` |
| **Linux kernel interfaces**: evdev (gamepads), X11 and PulseAudio *protocols over sockets* | Linux program | Not libraries: the port speaks the protocols itself (`host_x11.c`, `host_pulse.c`, `host_evdev.c`) | No X11, SDL, PulseAudio or ALSA code or library is linked |

Not used in the Windows and Linux native programs, although earlier plans named them: **SDL2** (their window, input and sound use the system's own API directly; SDL2 is used only by the Switch and Android programs, A2 below), **Xlib**, **libpng/zlib** (`png_out.c` writes uncompressed PNG itself).

### A2. Inside the Switch and Android programs (the wasm2c route; `tools/native/build_shell.py`)

| Component | Where it ends up | Licence | Status |
|---|---|---|---|
| **wabt: wasm2c translator and run-time** (`wasm2c/wasm-rt-*.c`, `.h`) | Switch NRO, Android `libmain.so`, Linux SDL shell: compiled in | Apache-2.0 | `LICENSE` of the checkout read 2026-11-14 |
| **wasi-libc** (the C library the game is compiled against for wasm32) | Inside the game's wasm module, hence inside the same programs | Apache-2.0 with LLVM exception, Apache-2.0 and MIT; parts: dlmalloc CC0, cloudlibc BSD-2-Clause, musl MIT | Top-level `LICENSE` read 2026-11-14; the parts were **not** audited one by one |
| **SDL2 2.32.10** | Switch: linked from devkitPro's portlibs. Android: built from source with the NDK into `libSDL2.so`; its Java layer (`org.libsdl.app`) is compiled into `classes.dex`. Linux SDL shell: the system library | zlib | `LICENSE.txt` read 2026-11-14 |
| **libnx** | Switch NRO: linked | ISC | `LICENSE.md` read 2026-11-14 (devkitPro's portlibs and Mesa pieces linked on the Switch are **not** audited) |
| **Android NDK r27 run-time** (libc++ statically inside `libSDL2.so`, compiler-rt builtins) | Android app | Apache-2.0 with LLVM exception | Not read in detail; NDK 27.3.13750724 |
| **Khronos OpenGL ES / EGL / KHR headers** | Compile time only (declarations) | Apache-2.0 / MIT (per the headers) | Not read in detail |

## B. Build tools (used to make a program; not part of it, not distributed by us)

| Component | Used for | Licence | Status |
|---|---|---|---|
| **llvm-mingw 20240619, UCRT, x86_64 Windows zip** (Martin Storsjö; clang 18.1.8, lld, llvm-ar/nm/readobj/rc, mingw-w64 headers and libraries, a bundled Python 3.11) | The Windows setup kit builds the game with it. Downloaded by the setup from `github.com/mstorsjo/llvm-mingw` and **SHA-256 checked** (`setup/components.json`); shipped unmodified inside the *offline* kit | Apache-2.0 with LLVM exception (LLVM); ZPL, public domain, BSD and LGPL parts (mingw-w64); PSF (Python). Their licence texts are inside the zip | Package contents read; the mingw-w64 LGPL question under A applies here too |
| **LLVM/clang/lld 18.x** (Linux development machine) | Cross-building the Linux and Windows programs and the sysroots (`tools/toolchain/`) | Apache-2.0 with LLVM exception | Read 2026-10-24 |
| **Python 3** standard library | Every build script, packager and test (`tools/`, `tests/`). No pip packages are needed to build | PSF | |
| **Pillow** and **numpy** | Only `tools/make_icon.py` (maintainer script that drew the icon), the optional icon fallback in `winres.py`, and one test helper (`tests/native/render/jpeg_ref_prep.py`). Not needed to build or play | Pillow: HPND; numpy: BSD-3-Clause | Not read in this repository; their package licences are the standard ones |
| **Xvfb, GNU make, git, patch** | Developer machine only (tests, sysroot building). The player's build needs none of them | Various | |
| **devkitPro / libnx** | Switch toolchain (devkitA64, elf2nro, nacptool, portlibs) | GPL-2.0-or-later with exceptions for the tools; the libraries per A2 | Used by `build_shell.py --target switch`; the tools' own licences were **not** read |
| **Binaryen** (`wasm-opt`) | `tools/native/optimize_wasm.py` shrinks the game's wasm before translation | Apache-2.0 | `LICENSE` read 2026-11-14 |
| **Android SDK** (build-tools 34: aapt2, zipalign, apksigner; platform android-34 `android.jar`) and **NDK r27** (clang) | `tools/android/build_apk.py` builds and signs the app; `android.jar` is only compiled against | Apache-2.0 (SDK licence accepted with `sdkmanager --licenses`) | Not audited in detail |
| **R8/D8 8.13.19** (Google Maven, SHA-1 checked) | Turns the Java classes into `classes.dex` (the SDK's own d8 8.2.2 fails on JDK 21 output) | BSD-3-Clause (the R8 project's licence; not re-read here) | Build tool; not shipped |
| **CMake, Ninja, a JDK (javac, keytool)** | Build SDL2 for Android, compile and sign | BSD-3-Clause / Apache-2.0 / GPL-2.0 with Classpath exception | Developer machine only |

## C. Game files (not ours, not in this repository)

| Component | Role | Licence status |
|---|---|---|
| **Team Hippodrome decompilation** (commit `8efced6`; submodule `ext/decomp`) | The game's source code; fetched and compiled on the player's computer; its 1,160 build files are hash-listed in `port/decomp-manifest.txt` | The repository has **no licence file** (checked 2026-10-22). Not relicensed by us. The offline setup kit contains these files and is therefore private-use only |
| **The player's ROM** | Read at run time and during their own build | Nintendo / Game Freak / Creatures / The Pokémon Company |
| **SGI IDO compiler** | Only the decomp's own matching build; not needed by the port | Proprietary; never included |

## D. Documentation and specifications consulted (no code copied; statements by the authors of our code, not audits)

| Source | What was taken from it | Licence of the source |
|---|---|---|
| n64brew wiki, libdragon documentation, the libultra headers in the decomp | RSP/RDP/PIF/Joybus/Transfer Pak behaviour for `src/rsp/`, `src/render/`, `src/ultra/hw_*.c`, `host_gbpak.c` | Wiki text / MIT / the decomp's |
| **cxd4 RSP plugin** | Behaviour of two RSP vector load/store details was *read* for that purpose | See that project; **no file or function copied** |
| **ares** (ISC licence; derived from BizHawk) | Transfer Pak and Game Boy MBC register behaviour in `host_gbpak.c` was *read*, not copied | ISC |
| **PulseAudio** sources (native protocol: `pstream.c`, `tagstruct.h`, …) | The framing and command layout in `host_pulse.c` | LGPL-2.1-or-later. **Protocol facts only; no code copied** |
| **X11 protocol** specification | `host_x11.c` | Public specification |
| **PNG** specification | `png_out.c` | Public specification |
| **GSM 06.10** (ETSI full-rate speech; the public `libgsm` of Degener and Bormann is the same algorithm) | Described in `libgsm.c`, which was written from the game's machine code and *differs* from libgsm; no libgsm code used | libgsm has its own permissive licence; **not used** |
| **Yay0 / MIO0 format** (Paper Mario Yay0 wiki page; the Yaz0 token coding in libAthena's `Compression.cpp`) | The format, in `yay0.c` | Documentation; **no code copied** |
| **Pocket Monsters Midori save and Japanese Stadium 2 character table** | Character mapping for `gbjp_names.inc` (see `NOTICE.md`) | The player's own files; not stored |

## E. Artwork

`assets/icon.jpg` and `assets/icon.ico` are original artwork drawn by `tools/make_icon.py`; they use no logo, character, ball design or lettering of any game. GPL-3.0-only like the rest.

## Compatibility notes (what is known, what is not)

* Apache-2.0 (with the LLVM exception), MIT, BSD, ZPL-2.1 and public domain parts can be combined with GPL-3.0 code.
* LGPL parts of mingw-w64 depend on exactly which files and which LGPL version; **not yet checked**.
* The decompilation's licence is unknown, and a GPL-3.0 combination with it cannot be offered to others until it is known. See `NOTICE.md`: no built game program is distributed.
