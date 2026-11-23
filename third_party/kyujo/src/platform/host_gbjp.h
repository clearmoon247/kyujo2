/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gbjp.h - lets the US Stadium use a Japanese Pokemon Green ("Pocket Monsters Midori") cartridge.
 * See host_gbjp.c for what is converted and why. Plain C, no dependency on the rest of the port, so a test can compile it alone.
 */
#ifndef HOST_GBJP_H
#define HOST_GBJP_H

#define GBJP_SRAM_SIZE 0x8000UL         /* a Pokemon Red/Green/Blue cartridge has 32 KiB of save RAM (4 banks of 8 KiB) */

/* 1 when `rom` is the Japanese Pokemon Green this module handles: title "POKEMON GREEN", destination code 0, MBC1 + RAM + battery, 512 KiB ROM,
 * 32 KiB save RAM, valid header checksum. */
int gbjp_is_supported_rom(const unsigned char *rom, unsigned long size);

/* The ROM as the US Stadium accepts it: a new 1 MiB image (the 512 KiB ROM followed by zeros) whose header says ROM size 1 MiB, destination
 * "international", with the header and global checksums recomputed. Returns NULL when out of memory. Free with free(). */
unsigned char *gbjp_make_us_rom(const unsigned char *rom, unsigned long size);

/* Builds the save RAM as the US Stadium reads it (international layout) from the Japanese save RAM `jp` (GBJP_SRAM_SIZE bytes).
 * Returns 1 when `jp` is a valid Japanese save and `view` holds its conversion; returns 0 when it is not (a blank or damaged save): `view` is then
 * filled with 0xFF, which the Stadium reports as an unusable save, as it would for an international cartridge in that state. */
int gbjp_save_to_us_layout(const unsigned char *jp, unsigned char *view);

/* The Latin-letter form of a 6-byte Japanese Game Boy name (0x50-terminated), as 11 bytes of the international text codes. Exposed for tests. */
void gbjp_name_to_us(const unsigned char *jp6, unsigned char *us11);

#endif
