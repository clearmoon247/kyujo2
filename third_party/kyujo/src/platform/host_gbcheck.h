/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gbcheck.h - is a Game Boy save of Pokemon Red/Blue/Yellow (international) in a state the game would accept?
 *
 * Used before a save file is replaced: a save that fails this test was caught in the middle of a write (a crash, a quit while the Stadium was
 * still writing) and must not replace one that passes.
 */
#ifndef HOST_GBCHECK_H
#define HOST_GBCHECK_H

/* Returns 1 if `ram` (the cartridge's 32 KiB save RAM) is a valid international Red/Blue/Yellow main save, 0 if it is not.
 * The rule is the Stadium's own (GbSave_VerifyChecksum in the decomp, src/pokemon_stats.c): the player's name (11 bytes at 0x2598) contains the
 * terminator 0x50, and the bytes from 0x2598 up to and including the checksum byte at 0x3523 add up to 0xFF modulo 256. */
int gb_gen1_main_save_valid(const unsigned char *ram);

/* 1 if the cartridge header title (ROM bytes 0x134..) is POKEMON RED, POKEMON BLUE or POKEMON YELLOW, i.e. a save of the layout above. */
int gb_title_is_gen1_international(const unsigned char *rom_header_title);

#endif
