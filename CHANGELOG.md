# Changelog

Versions are the revision labels shown in the log (`Kyujo2 English rNN`) and on the release folders.

## English r26
- Switch: notices ("Cheats on", "Gamepad connected", "F1 or Esc: menu") are drawn by the GPU as a layer over the
  picture. Before, each picture was read back from the GPU while a notice showed, which made the battle send-out lag.
- Switch: the GPU renderer's start-up screen reads "POKEMON STADIUM 2".
- Switch: `perf:` lines in the log every 2 s.

## English r25
- Switch: the menu is visible while the game runs; the game's picture no longer covers it.
- Switch: the GPU shader list is saved in `shader-keys.bin` and built at start-up on later runs.

## English r24
- Switch: GPU renderer (Kyujo's OpenGL ES 3 back end). The CPU cost of drawing fell from about 84% to 4% of a core.
- Software renderer fallback, automatic or through a `software-renderer` file.
- The game's memory allocator is hooked so that the GPU drops frame buffers whose memory is reused (fixes a crash at
  battle start).
- Correct buffer choice for 640-wide double-buffered screens (fixes flicker on the order screen).

## English r23
- Switch: Quit and Restart return to hbmenu cleanly. All game threads, the idle thread and the mod-settings thread
  are ended and joined before exit, and before the runtime frees the game's memory.

## English r22
- Switch: game threads are ended at exit (first pass).

## English r21
- Switch: Restart no longer crashes (audio and SDL are closed first).
- Switch: the Stadium's own save is written. The runtime's file replacement did not work on the SD card.
- Switch: icon, fully optimised build and CPU boost mode.
- Game Pak errors name the player's save file and are logged; private-copy file steps work on the Switch.

## English r20
- Switch: no crash at start (libnx cannot detach threads); settings and Game Boy saves are written on the SD card.
- Gold/Silver Game Paks with an oversized private save are repaired automatically.

## English r19
- Android: sound catches up after the app returns from the background.
- Every release builds Windows, Android, Linux (AppImage and Flatpak) and Switch.

## English r18
- Android: GB Tower no longer crashes (the Game Boy core's tail calls are guaranteed jumps).
- Free Battle (six Pokémon): the L + R cheat ends the battle instead of knocking out one Pokémon.

## Earlier
r1 to r17 brought the port from the first boot to a playable game on Windows, Linux and Android, with the Kyujo menus,
Transfer Pak, GB Tower and cheats. [docs/DEVELOPMENT-HISTORY.md](docs/DEVELOPMENT-HISTORY.md) keeps those notes.
