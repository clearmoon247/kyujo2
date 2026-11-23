# Third-party components

These keep their own licences. Apart from the Kyujo files in `third_party/kyujo/`, none of them is copied into this
repository: each is fetched from its official source (checked by commit or SHA-256) or installed from Debian or
devkitPro.

| Component | Version | Used for | Licence |
|---|---|---|---|
| N64Recomp | `598ce2080004a5672d604dceadbda436045307b7` + `patches/recompiler-*.patch` | recompiling the game's MIPS code to C; RSP microcode recompiler | MIT |
| N64ModernRuntime (librecomp, ultramodern) | `e4342ae030a9d94aa97a81aafd3368ed7daa74df` + `patches/runtime-*.patch` | the N64 OS, threads, timers, saves and PI/SI/VI emulation; linked into every program | GPL-3.0 |
| └ miniz, o1heap, xxHash, concurrentqueue (runtime submodules) | as pinned by the runtime | compression, heap, hashing, queues | MIT / MIT / BSD-2-Clause / BSD or Boost |
| Kyujo | `7b66b752ec364e015296a22c88409116827ee09e`, the files used are bundled in `third_party/kyujo/` + `patches/kyujo2-*.patch`, `*-word-order.patch`, `gpu_gles-kyujo2.patch` | renderer (F3DEX2, software RDP, GPU back end), menus, Transfer Pak / Game Boy cartridge hardware | GPL-3.0-only |
| Pokémon Stadium 2 decompilation ([team-hippodrome/pokestadiumgs](https://github.com/team-hippodrome/pokestadiumgs)) | `c0e10f23d90cc4f335b654711f13e53c2c07323b`, fetched by the build | build time only: recreates the N64 program and its symbols | none stated |
| SDL 2 | 2.32.10 (Windows, Android; Debian's 2.32.x on Linux); devkitPro switch-sdl2 2.28.5 | window, input, audio | zlib |
| Mesa (Switch port), libdrm_nouveau | devkitPro switch-mesa 20.1.0, switch-libdrm_nouveau 1.0.1 | OpenGL ES 3 on the Switch | MIT |
| libnx, devkitA64 | libnx 4.12.0, devkitA64 r30 | Switch system library and toolchain | ISC (libnx); GPL with exceptions (toolchain) |
| MinGW-w64 | Debian 13 (GCC 14) | Windows toolchain and run-time, linked statically | ZPL-2.1, public domain, BSD and LGPL parts |
| Android NDK / SDK | NDK 27.3.13750724, platform 34, build tools 34.0.0 / 35.0.1 | Android toolchain and packaging | Apache-2.0 (Android SDK licence) |
| AppImage type-2 runtime | the `continuous` build pinned by SHA-256 in `tools/build-lib.sh` | Linux AppImage start-up | MIT |
| Flatpak / org.freedesktop.Platform | 25.08 | Linux Flatpak bundle runtime | LGPL-2.1+ / various |
| splat, spimdisasm and the decomp's Python requirements | per the decomp's `requirements.txt` | build time only | MIT |

The Linux AppImage bundles SDL2 and, when built with `KYUJO2_NO_SUDO=1`, the Debian libraries SDL needs.
Their licences are the ones Debian ships with them.
