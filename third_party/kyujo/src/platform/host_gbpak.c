/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gbpak.c - a Transfer Pak with a Game Boy cartridge in it, as the controller port sees it.
 *
 * What it is for: the game's "GB Tower" and Game Pak Check read the player's Game Boy cartridge (ROM and save RAM) through the
 * N64 Transfer Pak. This file is the accessory itself: the register block of the pak plus the cartridge behind it. The framing of
 * the joybus commands (address check, CRCs, command block) is done by the emulated PIF in src/ultra/hw_pif.c, which calls this file.
 *
 * HOST-side file (plain C, may use the C library). Bytes cross the interface as plain bytes (see the byte-order notes in platform.h).
 *
 * ---- Sources (what each statement below was checked against) ----------------------------------------------------------------
 *  [LU]  the decomp's libultra: lib/ultralib/src/io/gbpak*.c, controller.h (CONT_ADDR_GB_POWER 0x8000, _BANK 0xA000, _STATUS 0xB000,
 *        GB_POWER_ON 0x84 / OFF 0xFE) and include/PR/os_gbpak.h (status bits). Shows what the game's own code writes, reads and expects.
 *  [LD]  libdragon's reference, https://libdragon.dev/ref/tpak_8h.html and tpak_8c.html: the same addresses and values, status bits
 *        0x01 access, 0x04 / 0x08 reset, 0x40 cartridge removed, 0x80 powered, the 0xC000-0xFFFF window of 16 KiB banks 0..3, and the
 *        example "bank 1, write 0xE000 = Game Boy 0x6000".
 *  [AR]  ares (ISC licence), ares/n64/controller/gamepad/transfer-pak.cpp and gamepad.cpp, itself derived from BizHawk. READ, not
 *        copied: the register behaviour below (power writes, the reset-state countdown that every status byte read advances, bank
 *        reset to 3 at power-on, reads returning 0 while off) follows it. It is an independent emulator, not the console.
 *  [GB]  the MBC behaviour of the cartridge chips: ares/gb/cartridge/board/mbc1.cpp, mbc3.cpp, mbc5.cpp (again read, not copied).
 *  A Japanese Pokemon Green ("Pocket Monsters Midori") is accepted through host_gbjp.c: the Stadium is shown a ROM header it accepts and an
 *  international-layout copy of the save (the "view"); the Game Boy emulator inside the Stadium (GB Tower) uses the real save. Which of the two a
 *  save RAM access gets is told by the game's emulator code through __osHostGbPakRawRam (port patch 0027). See host_gbjp.c.
 *  ASSUMPTIONS (not verified on hardware): the status byte of the info command (0x03 on the first query after start-up, then 0x01) is
 *  taken from [AR]; the MBC3 real-time clock is NOT modelled (its registers read 0xFF; no Pokemon Red/Blue/Yellow cartridge has one);
 *  a blank save file is filled with 0xFF (what an erased battery-backed RAM chip usually holds, not checked).
 *
 * ---- Transfer Pak register map ([LU], [LD], [AR]; addresses are those sent in the joybus read/write commands) ------------------
 *   0x8000-0x9FFF  power:   write 0x84 = on, 0xFE = off. Read: 0x84 while on, 0x00 while off.
 *                           Turning it on resets: bank register := 3, cartridge access off, reset state := 0.
 *   0xA000-0xAFFF  bank:    which 16 KiB piece of the Game Boy address space shows in 0xC000-0xFFFF (0..3, other values act as 0). Reads return it.
 *   0xB000-0xBFFF  status / access:  write bit 0 = 1 switches the cartridge on (the cartridge is reset when this goes 0 -> 1).
 *                           Read, one status byte per address:
 *                             bit 0 access on        bits 2,3 reset state (3 right after switching on, counts down as status bytes are read)
 *                             bit 6 no cartridge     bit 7 power on
 *                           (the libultra names: 0x01 OS_GBPAK_POWER, 0x04 RSTB_DETECTION, 0x08 RSTB_STATUS, 0x40 GBCART_PULL, 0x80 GBCART_ON)
 *   0xC000-0xFFFF  the cartridge: Game Boy address = bank * 0x4000 + (address - 0xC000). Bank 0 = 0x0000-0x3FFF, bank 1 = 0x4000-0x7FFF,
 *                           bank 2 = 0x8000-0xBFFF (save RAM at 0xA000), bank 3 = 0xC000-0xFFFF (nothing here: reads 0).
 *                           Writes into the ROM area go to the cartridge's bank-switching registers, as on a Game Boy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include "platform.h"
#include "host_gbjp.h"
#include "host_gbcheck.h"

#define GB_SLOTS 4                      /* one per controller port; only port 1 has a controller in this port so far */

/* The kinds of cartridge chip ("memory bank controller") this file can emulate. */
typedef enum { MBC_NONE, MBC_1, MBC_3, MBC_5 } MbcKind;

typedef struct GbPak {
    int present;                        /* a cartridge was given for this port */
    /* --- the pak --- */
    int power;                          /* 0x8000 register: 1 after a write of 0x84 */
    int cart_on;                        /* bit 0 of the 0xB000 register */
    int reset_state;                    /* 0..3, see the register map */
    int bank;                           /* 0..3, the 0xA000 register */
    int detect_latch;                   /* the info command reports "pak newly inserted" (3) once, then "pak present" (1) */
    /* --- the cartridge --- */
    unsigned char *rom;
    unsigned long rom_size;
    unsigned char *ram;
    unsigned long ram_size;
    /* Japanese Green only (host_gbjp.c): `ram` stays the real save; the Stadium's own save code reads and writes `view`, the international-layout copy */
    int jp;                             /* 1: this cartridge is a Japanese Green shown as an international one */
    unsigned char *view;
    int view_stale;                     /* the real save changed since the view was built */
    int raw;                            /* 1 while the Stadium's Game Boy emulator is transferring save RAM: it gets the real save, not the view */
    MbcKind mbc;
    int rom_bank, ram_bank, ram_enable, mode;   /* the chip's registers (mode: MBC1 only) */
    /* --- the save file --- */
    char save_path[1024];
    int dirty;                          /* RAM changed since it was last written to the file */
    unsigned long long last_write_ns;
    int backed_up;
    int rejected_written;               /* the RAM failed the save check and was written to <save>.rejected (cleared when the RAM changes again) */
} GbPak;

static GbPak sPak[GB_SLOTS];
static int sAtexit;

/* ------------------------------------------------------------------------------------------------------------------------------
 * the save file
 * ------------------------------------------------------------------------------------------------------------------------------ */

/* Copies `from` to `to` (used once per run, before the player's save file is first modified: the original is kept as <save>.bak). */
static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[4096];
    size_t n;
    int ok = 1;
    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}

/* A save of an international Red/Blue/Yellow cartridge that fails the game's own checksum was caught in the middle of a write (the Stadium
 * writes the party, then the checksum; a crash or a quit between the two leaves RAM that the game calls "file data destroyed"). Such RAM must not
 * replace a file that is still good. Returns 1 if it is fine to write, 0 if not (the RAM is then kept in <save>.rejected, once, so nothing is lost).
 * Other cartridges (the Japanese Green, other games) have no check here: they are always written. */
static int save_safe_to_write(GbPak *p)
{
    unsigned char old[0x8000];
    size_t n = 0;
    FILE *f;
    char rej[1100];
    if (p->jp || p->ram_size != 0x8000 || !gb_title_is_gen1_international(p->rom + 0x134)) return 1;
    if (gb_gen1_main_save_valid(p->ram)) return 1;
    f = fopen(p->save_path, "rb");
    if (f) { n = fread(old, 1, sizeof old, f); fclose(f); }
    if (n != sizeof old || !gb_gen1_main_save_valid(old)) return 1;   /* the file on disk is not good either (or is new): nothing to protect */
    if (!p->rejected_written) {
        p->rejected_written = 1;
        snprintf(rej, sizeof rej, "%s.rejected", p->save_path);
        f = fopen(rej, "wb");
        if (f) { fwrite(p->ram, 1, p->ram_size, f); fclose(f); }
        fprintf(stderr, "kyujo: Game Boy save: the save RAM fails the game's checksum (a write was cut short?); %s keeps the last good save, the new RAM is in %s\n",
                p->save_path, rej);
    }
    return 0;
}

/* Writes the save RAM to its file (temporary file, then rename, so a crash cannot leave half a save). */
static void flush_one(GbPak *p)
{
    char tmp[1100];
    FILE *f;
    if (!p->present || !p->dirty || !p->ram_size || !p->save_path[0]) return;
    if (!save_safe_to_write(p)) return;                 /* dirty stays set: a later, complete state is written */
    if (!p->backed_up) {
        char bak[1100];
        FILE *probe = fopen(p->save_path, "rb");
        p->backed_up = 1;
        if (probe) {                                     /* an existing save: keep the original once */
            fclose(probe);
            snprintf(bak, sizeof bak, "%s.bak", p->save_path);
            probe = fopen(bak, "rb");
            if (probe) {
                fclose(probe);                           /* never overwrite an older backup */
            } else if (copy_file(p->save_path, bak)) {
                fprintf(stderr, "kyujo: Game Boy save: original kept as %s\n", bak);
            } else {
                fprintf(stderr, "kyujo: Game Boy save: could not make a backup copy %s; NOT writing the save\n", bak);
                return;
            }
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", p->save_path);
    f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "kyujo: Game Boy save: cannot write %s\n", tmp); return; }
    if (fwrite(p->ram, 1, p->ram_size, f) != p->ram_size || fclose(f) != 0) {
        fprintf(stderr, "kyujo: Game Boy save: write to %s failed\n", tmp);
        return;
    }
#ifdef _WIN32
    /* C's rename() refuses to replace an existing file on Windows. MoveFileExA with MOVEFILE_REPLACE_EXISTING replaces it in one step, so a
     * crash cannot fall between "old save removed" and "new save in place" (same approach as host_save.c; NOT run on Windows yet). */
    if (!MoveFileExA(tmp, p->save_path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "kyujo: Game Boy save: cannot replace %s\n", p->save_path);
        return;
    }
#else
    if (rename(tmp, p->save_path) != 0) {
        fprintf(stderr, "kyujo: Game Boy save: cannot replace %s\n", p->save_path);
        return;
    }
#endif
    p->dirty = 0;
}

void host_gbpak_flush(void)
{
    int i;
    for (i = 0; i < GB_SLOTS; i++) flush_one(&sPak[i]);
}

/* ------------------------------------------------------------------------------------------------------------------------------
 * loading
 * ------------------------------------------------------------------------------------------------------------------------------ */

/* Reads a whole file into a new buffer; NULL if it cannot be read. */
static unsigned char *slurp(const char *path, unsigned long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (unsigned long)n;
    return buf;
}

/* Size of the cartridge's save RAM from the header byte 0x149 (the Game Boy header documentation, Pan Docs "The Cartridge Header").
 * Returns -1 for a code that is not defined. Not checked against the ROMs given for testing beyond the codes they use (3 = 32 KiB). */
static long ram_size_from_code(int code)
{
    switch (code) {
    case 0: return 0;
    case 1: return 2 * 1024;
    case 2: return 8 * 1024;
    case 3: return 32 * 1024;
    case 4: return 128 * 1024;
    case 5: return 64 * 1024;
    default: return -1;
    }
}

static void free_pak(GbPak *p)
{
    free(p->rom);
    free(p->ram);
    free(p->view);
    memset(p, 0, sizeof *p);
}

/* Writes the cartridge's save file if it changed, and takes the cartridge out of the port. */
void host_gbpak_unload(int channel)
{
    if (channel < 0 || channel >= GB_SLOTS || !sPak[channel].present) return;
    flush_one(&sPak[channel]);
    free_pak(&sPak[channel]);
    fprintf(stderr, "kyujo: Game Boy cartridge removed from port %d\n", channel + 1);
}

/* The cartridge's title from its header (up to 15 characters), or "" when the port is empty. */
void host_gbpak_title(int channel, char *out, unsigned size)
{
    unsigned i, n = 0;
    if (size == 0) return;
    out[0] = 0;
    if (channel < 0 || channel >= GB_SLOTS || !sPak[channel].present) return;
    for (i = 0; i < 15 && n + 1 < size; i++) {
        unsigned char c = sPak[channel].rom[0x134 + i];
        if (c == 0) break;
        out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[n] = 0;
}

const char *host_gbpak_load(int channel, const char *rom_path, const char *save_path)
{
    static char msg[1200];
    GbPak np;                           /* built aside: a cartridge that fails to load must not disturb the one that is in the port */
    GbPak *p = &np;
    unsigned long n = 0, i;
    unsigned char sum = 0;
    int type;
    long rs;
    const char *err = NULL;

    if (channel < 0 || channel >= GB_SLOTS) return "Game Boy cartridge: controller port out of range";
    flush_one(&sPak[channel]);                           /* so that a new cartridge using the same save file reads what the old one wrote */
    memset(p, 0, sizeof *p);
    p->rom = slurp(rom_path, &n);
    if (!p->rom) { snprintf(msg, sizeof msg, "cannot read the Game Boy ROM '%s'", rom_path); err = msg; goto fail; }
    p->rom_size = n;
    if (n < 0x4000 || (n & 0x3FFF) != 0) {
        snprintf(msg, sizeof msg, "'%s' is not a Game Boy ROM (size %lu is not a multiple of 16 KiB)", rom_path, n);
        err = msg; goto fail;
    }
    /* header checksum over 0x134..0x14C: x = x - byte - 1; stored at 0x14D (Pan Docs). A wrong sum means this is not a plain .gb file. */
    for (i = 0x134; i <= 0x14C; i++) sum = (unsigned char)(sum - p->rom[i] - 1);
    if (sum != p->rom[0x14D]) {
        snprintf(msg, sizeof msg, "'%s' has a bad Game Boy header checksum (not a .gb file, or damaged)", rom_path);
        err = msg; goto fail;
    }
    /* A Japanese Pokemon cartridge (destination code 0 at 0x14A, title "POKEMON ...") is not accepted by the US Stadium as it is: it takes only
     * destination code 1 and reads the save in the international layout. Japanese Green is adapted (host_gbjp.c, see its comment); any other
     * Japanese Pokemon cartridge is refused here with the reason, instead of loading it for the game to show "The Game Pak can't be used". */
    if (p->rom[0x14A] == 0 && memcmp(p->rom + 0x134, "POKEMON", 7) == 0) {
        unsigned char *adapted = gbjp_is_supported_rom(p->rom, n) ? gbjp_make_us_rom(p->rom, n) : NULL;
        if (!adapted) {
            snprintf(msg, sizeof msg, "'%s' is a Japanese Pokemon cartridge other than Pocket Monsters Green (Midori), which the US Stadium cannot use", rom_path);
            err = msg; goto fail;
        }
        free(p->rom);
        p->rom = adapted;
        p->rom_size = 0x100000;
        p->jp = 1;
    }
    type = p->rom[0x147];
    switch (type) {
    case 0x00: p->mbc = MBC_NONE; break;
    case 0x01: case 0x02: case 0x03: p->mbc = MBC_1; break;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: p->mbc = MBC_3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: p->mbc = MBC_5; break;
    default:
        snprintf(msg, sizeof msg, "'%s': Game Boy cartridge type 0x%02X is not supported (supported: none, MBC1, MBC3, MBC5)", rom_path, type);
        err = msg; goto fail;
    }
    if (type == 0x0F || type == 0x10) fprintf(stderr, "kyujo: Game Boy cartridge has a real-time clock; the clock is not emulated (reads 0xFF)\n");
    rs = ram_size_from_code(p->rom[0x149]);
    if (rs < 0) { snprintf(msg, sizeof msg, "'%s': unknown save RAM size code 0x%02X", rom_path, p->rom[0x149]); err = msg; goto fail; }
    p->ram_size = (unsigned long)rs;
    if (p->ram_size) {
        p->ram = (unsigned char *)malloc(p->ram_size);
        if (!p->ram) { err = "out of memory"; goto fail; }
        memset(p->ram, 0xFF, p->ram_size);               /* ASSUMPTION: see the file comment */
        if (save_path && save_path[0]) {
            unsigned long sn = 0;
            unsigned char *sv = slurp(save_path, &sn);
            if (sv) {
                if (sn != p->ram_size) {
                    /* Not guessed around: a save of another size is refused, because half-using it could damage it. */
                    snprintf(msg, sizeof msg, "save file '%s' is %lu bytes but the cartridge has %lu bytes of save RAM", save_path, sn, p->ram_size);
                    free(sv);
                    err = msg; goto fail;
                }
                memcpy(p->ram, sv, sn);
                free(sv);
            }                                           /* no file yet: the blank RAM stays, the file is created on the first change */
        }
    }
    if (p->jp) {
        if (p->ram_size != GBJP_SRAM_SIZE) { snprintf(msg, sizeof msg, "'%s': a Japanese Green has 32 KiB of save RAM", rom_path); err = msg; goto fail; }
        p->view = (unsigned char *)malloc(GBJP_SRAM_SIZE);
        if (!p->view) { err = "out of memory"; goto fail; }
        p->view_stale = 1;                                 /* built on the first access by the Stadium's save code */
    }
    if (save_path) snprintf(p->save_path, sizeof p->save_path, "%s", save_path);
    p->present = 1;
    p->detect_latch = 1;
    p->rom_bank = 1;
    host_gbpak_unload(channel);                          /* the cartridge that was there (its save is written first) */
    sPak[channel] = np;
    if (!sAtexit) { sAtexit = 1; atexit(host_gbpak_flush); host_set_exit_hook(host_gbpak_flush); }   /* atexit: normal return from main; the hook: host_exit() uses _Exit */
    if (np.jp) fprintf(stderr, "kyujo: Game Boy cartridge on port %d is a Japanese Pocket Monsters Green: the Stadium sees it as an international cartridge (header values and save layout adapted, host_gbjp.c); the Game Boy game itself uses the real ROM and save\n", channel + 1);
    fprintf(stderr, "kyujo: Game Boy cartridge on port %d: %lu KiB ROM, %lu KiB save RAM, chip type 0x%02X, title '%.15s', save file %s\n",
            channel + 1, np.rom_size / 1024, np.ram_size / 1024, type, (const char *)np.rom + 0x134,
            (np.save_path[0] && np.ram_size) ? np.save_path : "(none)");
    return NULL;
fail:
    free_pak(p);
    return err;
}

/* ------------------------------------------------------------------------------------------------------------------------------
 * the cartridge chip ([GB]; the register meanings are the standard Game Boy ones)
 * ------------------------------------------------------------------------------------------------------------------------------ */

static void mbc_reset(GbPak *p)
{
    p->rom_bank = 1;
    p->ram_bank = 0;
    p->ram_enable = 0;
    p->mode = 0;
}

static unsigned char rom_at(const GbPak *p, unsigned long offset)
{
    return p->rom[offset % p->rom_size];                 /* banks beyond the ROM repeat it, as the address lines would */
}

/* The save RAM an access gets: the cartridge's own, except that for a Japanese Green the Stadium's save code sees the international-layout view
 * (rebuilt first when the real save changed, which also drops what the Stadium wrote into the old view). */
static unsigned char *ram_in_use(GbPak *p)
{
    if (!p->jp || p->raw) return p->ram;
    if (p->view_stale) {
        p->view_stale = 0;
        if (!gbjp_save_to_us_layout(p->ram, p->view)) fprintf(stderr, "kyujo: Japanese Green save: not a valid save, the Stadium sees an empty one\n");
    }
    return p->view;
}

/* Called by the Game Boy emulator code of the Stadium (port patch 0027) around its save RAM transfers: on = 1 gives it the real save. */
void __osHostGbPakRawRam(int channel, int on)
{
    if (channel < 0 || channel >= GB_SLOTS) return;
    sPak[channel].raw = on ? 1 : 0;
}

/* A read at a Game Boy address (0x0000-0xBFFF). */
static unsigned char cart_read(GbPak *p, unsigned addr)
{
    if (addr < 0x4000) {
        if (p->mbc == MBC_1 && p->mode) return rom_at(p, ((unsigned long)p->ram_bank << 19) | addr);
        return rom_at(p, addr);
    }
    if (addr < 0x8000) {
        unsigned long bank = (unsigned long)p->rom_bank;
        if (p->mbc == MBC_1) bank |= (unsigned long)p->ram_bank << 5;      /* the 2 "upper bank" bits sit above the 5 lower bits */
        return rom_at(p, (bank << 14) | (addr & 0x3FFF));
    }
    if (addr >= 0xA000 && addr < 0xC000) {
        unsigned long off;
        if (p->mbc == MBC_NONE || !p->ram_enable || !p->ram_size) return 0xFF;
        if (p->mbc == MBC_3 && p->ram_bank > 3) return 0xFF;               /* 0x08-0x0C would be the clock registers (not emulated) */
        off = (p->mbc == MBC_1 && !p->mode) ? 0 : (unsigned long)p->ram_bank << 13;
        return ram_in_use(p)[(off | (addr & 0x1FFF)) % p->ram_size];
    }
    return 0;
}

/* A write at a Game Boy address: the ROM area is the chip's registers, 0xA000-0xBFFF is the save RAM. */
static void cart_write(GbPak *p, unsigned addr, unsigned char v)
{
    if (addr >= 0xA000 && addr < 0xC000) {
        unsigned long off, idx;
        if (p->mbc == MBC_NONE || !p->ram_enable || !p->ram_size) return;
        if (p->mbc == MBC_3 && p->ram_bank > 3) return;
        off = (p->mbc == MBC_1 && !p->mode) ? 0 : (unsigned long)p->ram_bank << 13;
        idx = (off | (addr & 0x1FFF)) % p->ram_size;
        if (p->jp && !p->raw) {                          /* the Stadium's own save code: into the view only, never into the Japanese save */
            ram_in_use(p)[idx] = v;
            return;
        }
        if (p->ram[idx] != v) {
            p->ram[idx] = v;
            p->dirty = 1;
            p->rejected_written = 0;
            p->view_stale = 1;                           /* (only used by a Japanese cartridge) the view must be rebuilt from this */
            p->last_write_ns = host_now_ns();
        }
        return;
    }
    if (addr >= 0x8000 || p->mbc == MBC_NONE) return;
    switch (p->mbc) {
    case MBC_1:
        if (addr < 0x2000) p->ram_enable = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) { p->rom_bank = v & 0x1F; if (!p->rom_bank) p->rom_bank = 1; }
        else if (addr < 0x6000) p->ram_bank = v & 0x03;
        else p->mode = v & 1;
        break;
    case MBC_3:
        if (addr < 0x2000) p->ram_enable = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) { p->rom_bank = v & 0x7F; if (!p->rom_bank) p->rom_bank = 1; }
        else if (addr < 0x6000) p->ram_bank = v;             /* 0-3 RAM bank, 8-12 clock register (not emulated) */
        /* 0x6000-0x7FFF: clock latch: nothing to latch */
        break;
    case MBC_5:
        if (addr < 0x2000) p->ram_enable = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) p->rom_bank = (p->rom_bank & 0x100) | v;
        else if (addr < 0x4000) p->rom_bank = (p->rom_bank & 0xFF) | ((v & 1) << 8);
        else if (addr < 0x6000) p->ram_bank = v & 0x0F;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------
 * the pak registers: one byte per call, as [AR] does it. `a` is the address within the pak's 0x8000-0xFFFF range.
 * ------------------------------------------------------------------------------------------------------------------------------ */

static unsigned char pak_read_byte(GbPak *p, unsigned a)
{
    a &= 0x7FFF;
    if (!p->power) return 0;
    if (a <= 0x1FFF) return 0x84;
    if (a <= 0x2FFF) return (unsigned char)p->bank;
    if (a <= 0x3FFF) {
        unsigned char st = (unsigned char)((p->cart_on ? 0x01 : 0) | (p->reset_state << 2) | 0x80);   /* no cartridge (0x40) never happens: a pak exists only with one */
        /* every status byte read advances the reset countdown, so the 32 bytes of one read differ (libultra ORs them, see osGbpakGetStatus) */
        if (p->cart_on && p->reset_state == 3) p->reset_state = 2;
        else if (!p->cart_on && p->reset_state == 2) p->reset_state = 1;
        else if (!p->cart_on && p->reset_state == 1) p->reset_state = 0;
        return st;
    }
    if (!p->cart_on) return 0;
    return cart_read(p, (unsigned)p->bank * 0x4000u + (a - 0x4000u));
}

static void pak_write_byte(GbPak *p, unsigned a, unsigned char v)
{
    a &= 0x7FFF;
    if (a <= 0x1FFF) {
        int was = p->power;
        if (v == 0x84) p->power = 1;
        else if (v == 0xFE) p->power = 0;
        if (!was && p->power) { p->bank = 3; p->cart_on = 0; p->reset_state = 0; }
        if (was && !p->power) flush_one(p);              /* the game switched the pak off: a good moment to save */
        return;
    }
    if (!p->power) return;
    if (a <= 0x2FFF) { p->bank = v > 3 ? 0 : v; return; }
    if (a <= 0x3FFF) {
        int was = p->cart_on;
        p->cart_on = v & 1;
        if (!was && p->cart_on) { p->reset_state = 3; mbc_reset(p); }
        if (was && !p->cart_on) flush_one(p);
        return;
    }
    if (!p->cart_on) return;
    cart_write(p, (unsigned)p->bank * 0x4000u + (a - 0x4000u), v);
}

/* ------------------------------------------------------------------------------------------------------------------------------
 * the interface to the PIF (platform.h)
 * ------------------------------------------------------------------------------------------------------------------------------ */

int host_gbpak_info_status(int channel)
{
    GbPak *p;
    int st;
    if (channel < 0 || channel >= GB_SLOTS || !sPak[channel].present) return 0;
    p = &sPak[channel];
    st = p->detect_latch ? 0x03 : 0x01;                  /* [AR]: 0x03 = pak newly inserted (reported once), 0x01 = pak present */
    p->detect_latch = 0;
    return st;
}

int host_gbpak_present(int channel)
{
    return channel >= 0 && channel < GB_SLOTS && sPak[channel].present;
}

int host_gbpak_latched(int channel)
{
    return channel >= 0 && channel < GB_SLOTS && sPak[channel].present && sPak[channel].detect_latch;
}

/* Development trace: KYUJO_GBPAK_TRACE=<n> prints the first n pak accesses (what the game asked and what the pak answered). */
static int trace_left(void)
{
    static int init, left;
    if (!init) {
        const char *e = getenv("KYUJO_GBPAK_TRACE");
        init = 1;
        left = e ? atoi(e) : 0;
    }
    return left > 0 ? left-- : 0;
}

void host_gbpak_trace_cmd(int channel, int command, int arg1, int arg2, int rx)
{
    if (trace_left()) fprintf(stderr, "pif: channel %d command %02X args %02X %02X rx %d\n", channel, command, arg1, arg2, rx);
}

void host_gbpak_trace_reject(int channel, int is_write, unsigned address, int latched, int checksum_ok)
{
    (void)channel;
    if (trace_left()) fprintf(stderr, "gbpak: %s %04X REFUSED (%s)\n", is_write ? "write" : "read ", address, latched ? "newly-inserted report pending" : checksum_ok ? "?" : "address checksum wrong");
}

void host_gbpak_read(int channel, unsigned address, unsigned char *out, int len)
{
    GbPak *p = &sPak[channel];
    int i;
    for (i = 0; i < len; i++) out[i] = pak_read_byte(p, address + (unsigned)i);
    if (trace_left()) {
        fprintf(stderr, "gbpak: read  %04X ->", address);
        for (i = 0; i < len && i < 8; i++) fprintf(stderr, " %02X", out[i]);
        fprintf(stderr, "%s   [power %d cart %d bank %d reset %d]\n", len > 8 ? " ..." : "", p->power, p->cart_on, p->bank, p->reset_state);
    }
}

void host_gbpak_write(int channel, unsigned address, const unsigned char *in, int len)
{
    GbPak *p = &sPak[channel];
    int i;
    if (trace_left()) {
        fprintf(stderr, "gbpak: write %04X <-", address);
        for (i = 0; i < len && i < 8; i++) fprintf(stderr, " %02X", in[i]);
        fprintf(stderr, "%s   [before: power %d cart %d bank %d]\n", len > 8 ? " ..." : "", p->power, p->cart_on, p->bank);
    }
    for (i = 0; i < len; i++) pak_write_byte(p, address + (unsigned)i, in[i]);
    /* Save changed RAM soon after the game stopped changing it (no thread is used; this runs on the next pak access). */
    if (p->dirty && host_now_ns() - p->last_write_ns > 2000000000ull) flush_one(p);
}
