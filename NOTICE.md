# Notice

## No affiliation
This project is not affiliated with, authorised by or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon
Company. "Pokémon", "Pokémon Stadium", "Nintendo", "Nintendo 64" and "Nintendo Switch" are trademarks of their
owners. They are used only to say which game and which hardware the project works with.

## No game content
The repository contains no ROM, no code or data generated or extracted from a ROM, no Game Boy ROMs or saves, and no
built program. The player supplies their own cartridge dumps; the build reads them on the player's own computer.

Everything the build produces is derived from copyrighted material and is for the builder's own use only:
- the generated C code in `build/recompiled`;
- the RSP programs in `build/rsp`;
- the menu artwork read from the Stadium 1 cartridge;
- every executable, APK, NRO, AppImage and Flatpak.

**Never publish a built program or generated sources.**

The build compiles the Pokémon Stadium 2 decompilation (pokestadiumgs) on the player's computer, only to recreate
the N64 program that is then recompiled. The decompilation is not copied into or relicensed by this repository. It
has no licence file of its own, which is a further reason why no combined program is distributed.

## Licence of this repository's code
Kyujo2's own files are licensed under the GNU General Public License, version 3 (`LICENSE`). They are:
- the native code, tools, scripts, patches (our changes), recompiler configuration, packaging and documentation;
- the application icons.

Kyujo2 is combined with:
- **Kyujo** (GPL-3.0-only): the files Kyujo2 uses are bundled unmodified in `third_party/kyujo/`, and `patches/`
  changes them at build time;
- **N64ModernRuntime** (GPL-3.0).

Changes to other projects in `patches/` are offered under the licence of the project they change.

## Third-party components
See `THIRD-PARTY.md`.
