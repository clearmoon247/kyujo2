# Notice

## Credit
Kyujo is built on the decompilation of the game by **Team Hippodrome** (https://github.com/team-hippodrome/pokestadium). The game's logic and structure are their work; Kyujo contributes the platform layer and the packaging. The decompilation is referenced as a git submodule (`ext/decomp`, pinned commit `8efced6`) and is neither copied here nor relicensed.

## No affiliation
This project is not affiliated with, authorised by or endorsed by Nintendo, Game Freak, Creatures Inc. or The Pokémon Company. "Pokémon", "Pokémon Stadium", "Nintendo", "Nintendo Switch" and "Nintendo 64" are trademarks of their owners and are used only to say which game and which hardware the project works with. The application icon is original artwork and uses no logo, character, ball design or lettering of any game.

## No game content
The repository contains no ROM, no data or code generated from a ROM, and no Game Boy ROMs or saves. The player supplies their own copy of the game; the port reads it at run time and during their own local build. Anything the build produces from the game's code or ROM (the built executable, extracted assets) is derived from copyrighted material and is for the builder's own use only. **Never publish a built game executable or generated sources.**

Two things a reader should know about, because they sit close to that line:
* `src/platform/gbjp_names.inc` holds only Latin letters written in `tools/native/gen_gbjp_names.py` (the romaji of each kana). The generator reads the Japanese Stadium 2 ROM's character-code table once, to learn which Game Boy code is which character; no ROM bytes are stored.
* Some files re-express routines that exist in the game only as assembly. `src/game_asm/libgsm.c` was written by reading the original machine code (the player's own ROM, disassembled) and says so; `src/game_asm/yay0.c` was written from the published description of the Yay0 format; `src/ultra/os_sched.c`, `os_libc.c` and similar files re-express the decomp's own assembly sources (`lib/ultralib/src/os/*.s`, `libc/*.s`) in C and say so. The licence below covers our authorship of that C code; it cannot grant anything in the original code whose behaviour it reproduces, and for the `.s` files that code is the decomp's. The team has not had that question answered by anyone qualified.

## Licence of this repository's code
Decision (project owner, 2026-10-28): the port's own code, tools, tests, documentation and artwork are licensed **GPL-3.0-only** (the GNU General Public License, version 3, *only*: not "or later"). The full text is in `LICENSE`. Copyright (C) 2026 Dan B. Cooper.

How it is marked: every source file (C, C++, headers, Python, shell, PowerShell, batch, CMake) starts with `SPDX-License-Identifier: GPL-3.0-only` and the copyright line. Files that cannot carry a comment (documentation, patches, data lists, images, the generated `.inc`) are covered by `REUSE.toml`.

What it covers, and what it does not:
* **Covered:** the files written for this project: `src/`, `tools/`, `tests/`, `docs/`, `dist/`, `assets/` (the icon is original artwork), `port/` lists, and our *changes* in `port/patches/`.
* **Not changed, not covered: the decompilation.** The decompilation by Team Hippodrome keeps whatever licence its authors give it (at the pinned commit `8efced6` it has no licence file of its own; the only licence files in it belong to third-party tools it bundles). `ext/decomp` is only a pointer to it. Nothing here relicenses it or grants rights in it. `port/patches/*.patch` contain none of its text: a patch holds only the lines we ADD, and names the original's lines by their SHA-256 and position (`tools/native/patchapply.py` explains the format; `--explain` shows a patch as a full diff, made on a player's own copy of the decomp). An added line is our work, but where it is a small edit of an original line it can still resemble it; that is the one place where we cannot rule out that a decomp author's expression is echoed, and it is why the permission request below is worth making.
* **Not covered: the game.** Nintendo, Game Freak, Creatures and The Pokémon Company keep their rights in the game, its code and its data.
* **Third-party components** keep their own licences (`THIRD-PARTY.md`).

What this means for what may be shared (the maintainers are not lawyers; this is a precaution, not legal advice):
* **The source of this repository** is published under GPL-3.0-only (owner's decision, 2026-11-14); anyone who receives it gets the GPL's rights *for our files*.
* **Safe to hand out now (nothing of the decompilation, the game or a ROM is in it):** this repository's source, the **online** Windows setup kit (`Kyujo-Setup-online-*.zip`: scripts, our sources, hash lists, our patches; the player's computer downloads the decomp and compiler from their official addresses and every file is checked against the hash list before use) and the Linux/Android/Switch *build instructions*. Each player makes their own program.
* **A built game executable, the offline setup kit, and any `.nro`, `.apk`, AppImage or Linux/Windows program zip may not be published.** They combine GPL code with decomp code that has no stated licence, and with data copied from the player's ROM; we cannot give anyone GPL rights in the parts that are not ours. The build-it-yourself approach (`docs/windows-setup.md`) exists for this reason: nobody receives a combined program, each player makes their own.
* **Best step before any public release:** ask Team Hippodrome (their repository's issue tracker) whether they agree to a project that compiles their code on the player's own computer with small patches, and whether they will state a licence. A written yes removes the open question; none of the above replaces it.
* If the decomp's authors later state a licence, check that it is compatible with this one before any release that combines the two; if it is not, the conflict is resolved first.
* **Contributors:** a GPL grant needs the agreement of everyone who holds copyright in a file. The project owner, Dan B. Cooper, holds the copyright in the files that carry his name; anyone who contributes later agrees to the GPL-3.0-only grant for their contribution.

## Third-party components
Moved to **`THIRD-PARTY.md`**: every library, tool and reference the project builds with, links, or learned from, with its licence and what was checked.

## Earlier prototype
An earlier private prototype informed the written behaviour specification in `docs/spec/`. No file or code from it is used here.
