# Kyujo2

Kyujo2 is a native port of **Pokémon Stadium 2** (USA, English) for **Windows, Linux, Android and Nintendo Switch**
(homebrew). The game's own code is statically recompiled from the N64 program with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and runs on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). Graphics, the menus and the Game Boy Transfer Pak
come from [Kyujo](#credits), the native port of the first Pokémon Stadium.

**This repository contains no game.** You build the program yourself from your own cartridge dumps (see
[No game content](#no-game-content)).

## Features
- The full game, including GB Tower (Game Boy games run inside the Stadium) and Transfer Pak links with Pokémon
  Red/Blue/Yellow/Green and Gold/Silver/Crystal saves, with real-time clock for Gold/Silver.
- An in-game menu (F1 or Esc; the menu button on a gamepad) for controllers, Game Paks, video and sound options,
  restart and quit.
- Optional battle cheats: **L + R** wins the battle after your next move; **Z** restores the first Pokémon's HP.
- Windows, Linux and Android draw with the software renderer. The Switch uses the GPU renderer (OpenGL ES 3).

## What you need
| File | Used for | SHA-1 |
|---|---|---|
| Pokémon Stadium 2 (USA) `.z64`, 64 MiB | the game (build time and every platform at run time) | `d8343e69a7dc63b869cf6361d87cde64444281d3` |
| Pokémon Stadium (USA) `.z64` | build time only: the menu's font and wallpaper are read from it | `ed7bef5a306f88c0a6e96b15e71fee2ef32058f3` |
| Game Boy ROMs and `.sav` files (optional) | Transfer Pak and GB Tower | |

To build, put your two game files in `roms/` and run the script for your system on Linux or WSL. Each script
installs what it needs. [BUILDING.md](BUILDING.md) has the details.

```sh
./build-linux.sh      # or ./build-windows.sh, ./build-android.sh, ./build-switch.sh, ./build-all.sh
```

## Playing
**Windows:** keep `Kyujo2.exe` and `SDL2.dll` together. On first start, choose the Stadium 2 cartridge, then set up
the Game Paks or start the game. Saves and settings are kept in `%APPDATA%\Kyujo2\Kyujo2`.

**Linux:** `chmod +x Kyujo2-US-*.AppImage` and run it (optionally with the cartridge path), or install the Flatpak
bundle (needs `org.freedesktop.Platform` 25.08) and run `flatpak run org.kyujo2.Kyujo2`.

**Android (arm64, Android 7.0+):** install the APK and pick the cartridge in the app. The touch controls hide when a
gamepad is connected.

**Switch (homebrew, Atmosphère):** copy the `.nro` to `sdmc:/switch/Kyujo2/Kyujo2.nro` and the cartridge to
`sdmc:/switch/Kyujo2/stadium2-us.z64`. Start it from hbmenu in **application mode** (hold R while starting a game),
not applet mode. Settings, saves, Game Paks and `kyujo-sdl-log.txt` are kept in `sdmc:/switch/Kyujo2/`.
- The first run builds GPU shaders as new effects appear, which causes short pauses. Later runs build them while the
  game starts, from `shader-keys.bin`.
- An empty file named `software-renderer` in that folder selects the software renderer instead of the GPU.

**Keyboard (desktop):**

| Key | N64 button |
|---|---|
| X | A |
| Z | B |
| Enter | Start |
| Arrow keys | stick |
| W A S D | D-pad |
| I J K L | C buttons |
| C | Z |
| Q / E | L / R |
| F1 or Esc | menu |

Gamepads are set up in the menu under Controllers.

**Problems:** every platform writes `kyujo-sdl-log.txt` in its data folder. Include it, and on the Switch the
Atmosphère crash report, when reporting a problem.

## No game content
This repository holds only the port's own source, build scripts and patches. It contains **no ROM, no code or data
extracted from a ROM, and no built program**. Everything the build makes from your cartridge is derived from
copyrighted material and is **for your own use only**. Do not publish or share built executables, APKs, NROs,
AppImages, Flatpaks or generated sources. Upstream Kyujo explains the reasons in its NOTICE: the decompilation it
builds on has no stated licence, and the game belongs to its owners.

Pokémon, Pokémon Stadium, Nintendo, Nintendo 64 and Nintendo Switch are trademarks of their owners. This project is
not affiliated with, authorised or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company.

## Licence
Kyujo2's own code is released under the **GNU General Public License, version 3** (see [LICENSE](LICENSE)). It is
combined with Kyujo (GPL-3.0-only) and N64ModernRuntime (GPL-3.0). Third-party components keep their own licences;
see [THIRD-PARTY.md](THIRD-PARTY.md) and [NOTICE.md](NOTICE.md).

## Credits
- **Kyujo** by Dan B. Cooper: renderer, menus, Transfer Pak and Game Boy hardware, GPU back end (GPL-3.0-only).
- **N64Recomp** and **N64ModernRuntime** by Wiseguy (Mr-Wiseguy) and contributors.
- **The Pokémon Stadium 2 decompilation** (pokestadiumgs) and its contributors, and the Pokémon Stadium
  decompilation by Team Hippodrome, on which Kyujo is built.
- **SDL** by the SDL developers; **devkitPro**, libnx and the Switch Mesa port by their teams.

[CHANGELOG.md](CHANGELOG.md) lists the releases. [docs/DEVELOPMENT-HISTORY.md](docs/DEVELOPMENT-HISTORY.md) keeps the
earlier development notes and regression tests.
