/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_gbjp.c - lets the US Stadium use a Japanese Pokemon Green ("Pocket Monsters Midori") cartridge.
 *
 * THE PROBLEM. The US Stadium knows the cartridges Pokemon Red, Blue and Yellow (international releases). A Japanese Green differs in three ways:
 *   1. The ROM header: destination code 0 (Stadium wants 1), ROM size 512 KiB (Stadium wants 1 MiB), and the title "POKEMON GREEN" is in
 *      the game's table of titles but disabled there (two 0xFF bytes behind it; port patch 0027 enables it).
 *   2. The Japanese games' save layout: names are 6 bytes (international: 11), a box holds 30 Pokemon (20), there are 8 boxes (12), and
 *      the text uses kana codes. The Stadium reads the save in the international layout (decomp src/pokemon_stats.c, src/gb_data.h).
 *   3. The Stadium's own Game Boy emulator (GB Tower) must still see the REAL cartridge: the Japanese game's own save.
 * WHAT THIS FILE DOES (all of it on the host side; the game's code is not changed except for the title entry and one hook, patch 0027):
 *   - gbjp_make_us_rom: the header values the US Stadium accepts (see above). The game's code is untouched, the cartridge image is what is adapted.
 *   - gbjp_save_to_us_layout: builds an international-layout copy ("view") of the Japanese save RAM. host_gbpak.c serves the view to the
 *     Stadium's own save code (player name, party, boxes, Pokedex), and the real save RAM to the Game Boy emulator. The view is
 *     rebuilt whenever the emulator has written to the real save; the Stadium's own writes go to the view only and are discarded,
 *     so the Japanese save file is never changed by the Stadium's code (it is changed only by the Game Boy game itself, as on a console).
 *   - names: the US font has no kana, so a Japanese name is written in romaji (Latin letters): ラプラス -> RAPURASU.
 *
 * WHERE THE LAYOUTS COME FROM. The decomp's own code for both versions (src/gb_data.h, src/pokemon_stats.c: the VERSION_JP branches are the
 * Japanese Stadium's reading of the same cartridge saves), checked against a real Pocket Monsters Midori save: its main-data checksum
 * (0xFF = sum of all bytes from the name to the checksum byte, SRAM 0x2598..0x3594) is valid, and the species lists and names decode. The
 * kana table is in gbjp_names.inc (generated from the Japanese Stadium 2 ROM by tools/native/gen_gbjp_names.py).
 *
 * WHAT IS NOT CONVERTED (the Stadium does not read it): the Hall of Fame and other regions of the save; event flags and everything between the
 * fields listed below is copied as it is (kept at its shifted place) or left zero. Pokemon records (33 bytes) are identical in both layouts.
 * LIMITS: only Green (Japan) is handled. The boxes are re-packed (see pack_boxes). Nothing the Stadium writes is stored.
 *
 * HOST-side file, standard C only.
 */
#include <stdlib.h>
#include <string.h>

#include "host_gbjp.h"

/* ---- the layouts (offsets are inside the save RAM; "rel" = relative to the start of the main data block) ------------------------------ */

#define MAIN_BASE      0x2580           /* both layouts: the main data block starts here (decomp: GbPak_MbcRead(.., 0x2580, sizeof(GbSaveMainData))) */
#define JP_MAIN_SIZE   0x1020           /* decomp VERSION_JP sizeof(GbSaveMainData) */
#define US_MAIN_SIZE   0x0FC0           /* decomp sizeof(GbSaveMainData), international */

#define NAME_REL       0x18             /* player name: 6 bytes (JP) / 11 bytes (US) */
#define JP_NAME_LEN    6
#define US_NAME_LEN    11

/* Main-data pieces copied unchanged (JP rel start, length, US rel start). The shifts are the differences between the decomp's field offsets of the two
 * versions: dex flags 0x1E -> 0x23 (+5: the player name is 5 bytes longer); the rival's name (6 -> 11 bytes) is converted separately; everything after
 * the rival's name up to 0x513 is at +10 (0x80 -> 0x8A, 0x25C -> 0x266, 0x2C2 -> 0x2CC, 0x437 -> 0x441, 0x46D -> 0x477, 0x513 -> 0x51D). */
#define JP_RIVAL_REL   0x71
#define US_RIVAL_REL   0x76
static const struct { unsigned jp, len, us; } kCopy[] = {
    { 0x000, 0x018, 0x000 },            /* before the player name */
    { 0x01E, 0x053, 0x023 },            /* Pokedex owned/seen, bag, money ... up to the rival's name */
    { 0x077, 0x49D, 0x081 },            /* after the rival's name up to 0x513 (+10): PC items, current box number, player ID ... */
    { 0x514, 0x441, 0x51E },            /* the rest up to the party (the Stadium reads none of it; kept at the start of the larger gap) */
};
#define JP_PARTY_REL   0x955
#define US_PARTY_REL   0x9AC
#define JP_CURBOX_REL  0xAAD
#define US_CURBOX_REL  0xB40
#define JP_CSUM_REL    0x1014           /* decomp: GbSaveMainData.unk_1000.unk_14 */
#define US_CSUM_REL    0x0FA3           /* decomp: GbSaveMainData.unk_FA0.unk_03 */
#define JP_BOXNUM_REL  0x2C2            /* current box number; bit 7 = boxes are in use (decomp: GbSave_GetBoxCount reads it) */
#define US_BOXNUM_REL  0x2CC

/* party (GbSaveParty): count at 0, species list at 1 (6 + terminator), 6 records of 44 bytes at 8, then the 6 original-trainer names and 6 nicknames */
#define PARTY_RECORDS  0x008
#define PARTY_RECSIZE  0x2C
#define JP_PARTY_OT    0x110
#define JP_PARTY_NICK  0x134
#define US_PARTY_OT    0x110
#define US_PARTY_NICK  0x152
#define JP_PARTY_SIZE  0x158
#define US_PARTY_SIZE  0x194

/* a box: count at 0, species list at 1 (n + terminator 0xFF), n records of 33 bytes, n original-trainer names, n nicknames */
#define MON_SIZE       33
#define JP_BOX_MONS    30
#define US_BOX_MONS    20
#define JP_BOX_RECORDS 0x20
#define JP_BOX_OT      0x3FE
#define JP_BOX_NICK    0x4B2
#define JP_BOX_SIZE    0x566
#define US_BOX_RECORDS 0x16
#define US_BOX_OT      0x2AA
#define US_BOX_NICK    0x386
#define US_BOX_SIZE    0x462
#define JP_BOXES       8
#define US_BOXES       12
#define BANK0          0x4000
#define BANK1          0x6000
#define JP_BOXES_PER_BANK 4
#define US_BOXES_PER_BANK 6

/* ---- names ------------------------------------------------------------------------------------------------------------------------------ */

static const char *const kRomaji[256] = {
#include "gbjp_names.inc"
};

/* International Game Boy text codes (the same ones the decomp's Text_UntranscodeName table D_80073030 maps): A-Z = 0x80.., 0 = 0xF6.. */
static unsigned char us_code(char c)
{
    if (c >= 'A' && c <= 'Z') return (unsigned char)(0x80 + (c - 'A'));
    if (c >= '0' && c <= '9') return (unsigned char)(0xF6 + (c - '0'));
    switch (c) {
    case ' ': return 0x7F;
    case '-': return 0xE3;
    case '.': return 0xE8;
    case '!': return 0xE7;
    case ',': return 0xF4;
    case 1:   return 0xEF;               /* male sign (our own marker for it, see gbjp_name_to_us) */
    case 2:   return 0xF5;               /* female sign */
    default:  return 0xE6;               /* '?' */
    }
}

static int is_vowel_letter(char c) { return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U'; }

void gbjp_name_to_us(const unsigned char *jp6, unsigned char *us11)
{
    char txt[16];
    int n = 0, i, double_next = 0;
    memset(us11, 0, US_NAME_LEN);
    for (i = 0; i < JP_NAME_LEN && jp6[i] != 0x50 && n < US_NAME_LEN - 1; i++) {
        unsigned char code = jp6[i];
        const char *r = kRomaji[code];
        if (code == 0xEF) { txt[n++] = 1; continue; }          /* the male and female signs have codes of their own in both layouts */
        if (code == 0xF5) { txt[n++] = 2; continue; }
        if (r[0] != '^') {
            /* A syllable. After a small "tsu" the next consonant is doubled: ... ッ + TO -> TTO, ッ + CHI -> TCHI. */
            if (double_next && !is_vowel_letter(r[0]) && r[0] >= 'A' && r[0] <= 'Z') {
                if (r[0] == 'C' && r[1] == 'H') { if (n < US_NAME_LEN - 1) txt[n++] = 'T'; }
                else if (n < US_NAME_LEN - 1) txt[n++] = r[0];
            }
            double_next = 0;
            for (; *r && n < US_NAME_LEN - 1; r++) txt[n++] = *r;
        } else if (r[1] == 'T') {                              /* ^TSU: small tsu */
            double_next = 1;
        } else if (r[1] == 'Y') {                              /* ^YA ^YU ^YO: small ya/yu/yo merge with a syllable ending in I: キ + ャ -> KYA */
            char vowel = r[2];                                 /* "^YA" -> 'A', "^YU" -> 'U', "^YO" -> 'O' */
            if (n > 0 && txt[n - 1] == 'I') {
                n--;                                           /* drop the I of the syllable before */
                /* SH, CH and J take no Y: シ + ャ -> SHA, チ + ュ -> CHU, ジ + ョ -> JO. Any other consonant does: キ + ャ -> KYA. */
                if (!((n >= 2 && txt[n - 1] == 'H' && (txt[n - 2] == 'S' || txt[n - 2] == 'C')) || (n >= 1 && txt[n - 1] == 'J'))) {
                    if (n < US_NAME_LEN - 1) txt[n++] = 'Y';
                }
                if (n < US_NAME_LEN - 1) txt[n++] = vowel;
            } else {                                           /* alone: YA */
                if (n < US_NAME_LEN - 1) txt[n++] = 'Y';
                if (n < US_NAME_LEN - 1) txt[n++] = vowel;
            }
        } else {                                               /* ^A ^I ^U ^E ^O: a small vowel (ファ, ディ ...) */
            /* After a consonant + vowel syllable it replaces that vowel: FU + small A -> FA, DE + small I -> DI. Otherwise it is just appended. */
            if (n >= 2 && is_vowel_letter(txt[n - 1]) && !is_vowel_letter(txt[n - 2]) && txt[n - 2] >= 'A' && txt[n - 2] <= 'Z') txt[n - 1] = r[1];
            else if (n < US_NAME_LEN - 1) txt[n++] = r[1];
        }
    }
    for (i = 0; i < n; i++) us11[i] = us_code(txt[i]);
    us11[n] = 0x50;                                            /* terminator, the rest stays 0 */
}

/* ---- the ROM ---------------------------------------------------------------------------------------------------------------------------- */

#define ROM_JP_SIZE 0x80000UL
#define ROM_US_SIZE 0x100000UL

static unsigned char header_checksum(const unsigned char *rom)
{
    unsigned char x = 0;
    unsigned i;
    for (i = 0x134; i <= 0x14C; i++) x = (unsigned char)(x - rom[i] - 1);      /* Pan Docs, "The Cartridge Header": x = x - byte - 1 */
    return x;
}

int gbjp_is_supported_rom(const unsigned char *rom, unsigned long size)
{
    if (size != ROM_JP_SIZE) return 0;
    if (memcmp(rom + 0x134, "POKEMON GREEN", 13) != 0) return 0;
    if (rom[0x14A] != 0) return 0;                              /* Japan */
    if (rom[0x147] != 0x03 || rom[0x148] != 0x04 || rom[0x149] != 0x03) return 0;     /* MBC1+RAM+BATTERY, 512 KiB, 32 KiB RAM */
    return header_checksum(rom) == rom[0x14D];
}

unsigned char *gbjp_make_us_rom(const unsigned char *rom, unsigned long size)
{
    unsigned char *out;
    unsigned long i, sum = 0;
    if (size != ROM_JP_SIZE) return NULL;
    out = (unsigned char *)calloc(1, ROM_US_SIZE);
    if (!out) return NULL;
    memcpy(out, rom, size);                                     /* the upper half stays zero: the game never selects a bank above 31 */
    out[0x148] = 0x05;                                          /* 1 MiB (the Stadium requires it) */
    out[0x14A] = 0x01;                                          /* destination: not Japan (the Stadium requires it) */
    /* Chip type: MBC3+RAM+BATTERY instead of Midori's MBC1+RAM+BATTERY. Found by running it (Linux, Midori ROM and real save):
     *  - the Stadium's transfer code reads the ROM in 16 KiB banks 0-63 with ONE 7-bit write to 0x2100, and selects save RAM banks 0-3 with a
     *    write to 0x4000; a real MBC1 takes only 5 bits there and, in the mode it starts in, ignores the RAM bank, so the host would have
     *    shown it the same 16 KiB of ROM twice and the same RAM bank four times;
     *  - with the header saying MBC1 (type 0x03) the Stadium's Game Boy core ran the title screen, then executed an invalid opcode as soon as
     *    START was pressed (its handler raises the "saved data corrupted" message); with MBC3 (the chip it is proven with: Red and Blue) the same
     *    ROM and save run, load, play and save.
     * Pokemon Green uses nothing of MBC1 that MBC3 lacks: ROM bank numbers below 32, RAM enable, RAM bank 0-3 (the mode write at 0x6000 is
     * the harmless clock latch on an MBC3). */
    out[0x147] = 0x13;
    out[0x14D] = header_checksum(out);
    /* The Stadium's GB emulator sums all 1 MiB it reads, subtracts the two global-checksum bytes (once) and compares with the global checksum in the
     * header (0x14E-0x14F, big endian; src/fragments/gb_tower_emulator, GbEmu state 3): so the stored value must be the sum of every byte except those two. */
    for (i = 0; i < ROM_US_SIZE; i++) if (i != 0x14E && i != 0x14F) sum += out[i];
    out[0x14E] = (unsigned char)((sum >> 8) & 0xFF);
    out[0x14F] = (unsigned char)(sum & 0xFF);
    return out;
}

/* ---- the save --------------------------------------------------------------------------------------------------------------------------- */

/* The Stadium's rule (GbSave_VerifyChecksum): the name has a 0x50 terminator and the bytes from the name to the checksum byte, that byte included, add up to 0xFF. */
static int jp_main_valid(const unsigned char *main)
{
    unsigned i, sum = 0;
    int marker = 0;
    for (i = 0; i < JP_NAME_LEN; i++) if (main[NAME_REL + i] == 0x50) marker = 1;
    if (!marker) return 0;
    for (i = NAME_REL; i <= JP_CSUM_REL; i++) sum += main[i];
    return (sum & 0xFF) == 0xFF;
}

/* One Pokemon of a Japanese box, collected before it is packed into the international boxes. */
typedef struct {
    unsigned char species;
    unsigned char record[MON_SIZE];
    unsigned char ot[JP_NAME_LEN];
    unsigned char nick[JP_NAME_LEN];
} PackedMon;

/* Reads the Pokemon of one Japanese box (`box` points at its JP_BOX_SIZE bytes) and appends them to `list`. */
static void collect_box(const unsigned char *box, PackedMon *list, int *count)
{
    int n = box[0], k;
    if (n > JP_BOX_MONS) n = JP_BOX_MONS;                       /* a damaged count must not read past the box */
    for (k = 0; k < n && *count < JP_BOXES * JP_BOX_MONS; k++) {
        PackedMon *m = &list[(*count)++];
        m->species = box[1 + k];
        memcpy(m->record, box + JP_BOX_RECORDS + k * MON_SIZE, MON_SIZE);
        memcpy(m->ot, box + JP_BOX_OT + k * JP_NAME_LEN, JP_NAME_LEN);
        memcpy(m->nick, box + JP_BOX_NICK + k * JP_NAME_LEN, JP_NAME_LEN);
    }
}

/* Writes `n` Pokemon starting at list[first] as one international box (US_BOX_SIZE bytes at `box`). */
static void write_us_box(unsigned char *box, const PackedMon *list, int first, int n)
{
    int k;
    memset(box, 0, US_BOX_SIZE);
    box[0] = (unsigned char)n;
    memset(box + 1, 0xFF, US_BOX_MONS + 1);                     /* species list: filled below, 0xFF-terminated */
    for (k = 0; k < n; k++) {
        const PackedMon *m = &list[first + k];
        box[1 + k] = m->species;
        memcpy(box + US_BOX_RECORDS + k * MON_SIZE, m->record, MON_SIZE);
        gbjp_name_to_us(m->ot, box + US_BOX_OT + k * US_NAME_LEN);
        gbjp_name_to_us(m->nick, box + US_BOX_NICK + k * US_NAME_LEN);
    }
}

int gbjp_save_to_us_layout(const unsigned char *jp, unsigned char *view)
{
    const unsigned char *jm = jp + MAIN_BASE;
    unsigned char *um = view + MAIN_BASE;
    PackedMon *list;
    int count = 0, i, m, boxes_in_use;
    unsigned sum;

    memset(view, 0xFF, GBJP_SRAM_SIZE);
    if (!jp_main_valid(jm)) return 0;
    list = (PackedMon *)calloc(JP_BOXES * JP_BOX_MONS, sizeof *list);
    if (!list) return 0;
    memset(view, 0, GBJP_SRAM_SIZE);                            /* regions the Stadium does not read: zero (a real cartridge holds sprite data there) */

    /* -- main data -- */
    for (i = 0; i < (int)(sizeof kCopy / sizeof kCopy[0]); i++) memcpy(um + kCopy[i].us, jm + kCopy[i].jp, kCopy[i].len);
    gbjp_name_to_us(jm + NAME_REL, um + NAME_REL);
    gbjp_name_to_us(jm + JP_RIVAL_REL, um + US_RIVAL_REL);

    /* party: the Pokemon records are identical, the names are converted */
    {
        const unsigned char *jpar = jm + JP_PARTY_REL;
        unsigned char *upar = um + US_PARTY_REL;
        int n = jpar[0];
        if (n > 6) n = 6;
        memcpy(upar, jpar, PARTY_RECORDS + 6 * PARTY_RECSIZE);          /* count, species list, records */
        upar[0] = (unsigned char)n;
        for (i = 0; i < 6; i++) {
            gbjp_name_to_us(jpar + JP_PARTY_OT + i * JP_NAME_LEN, upar + US_PARTY_OT + i * US_NAME_LEN);
            gbjp_name_to_us(jpar + JP_PARTY_NICK + i * JP_NAME_LEN, upar + US_PARTY_NICK + i * US_NAME_LEN);
        }
        (void)JP_PARTY_SIZE; (void)US_PARTY_SIZE;
    }

    /* -- boxes --
     * The Japanese game has 8 boxes of up to 30 Pokemon (240 places), the international one 12 boxes of 20 (240 places). Boxes cannot be carried over
     * one to one, so the Pokemon are listed in the Japanese order (box 1 .. box 8, each in its own order) and dealt out 20 to a box:
     * international box 1 gets the first 20, box 2 the next 20, and so on. All 240 places fit, so no Pokemon is lost. The Japanese box that is
     * the "current box" lives in the main data block (its copy in the bank is old), the others in the two banks.
     * Stadium's current box (the one kept in the main data) is international box 1; its number is stored with bit 7 set, as the games do. */
    boxes_in_use = (jm[JP_BOXNUM_REL] & 0x80) != 0;
    if (boxes_in_use) {
        int cur = jm[JP_BOXNUM_REL] & 0x7F;
        for (i = 0; i < JP_BOXES; i++) {
            const unsigned char *box = (i == cur) ? jm + JP_CURBOX_REL
                                     : jp + ((i < JP_BOXES_PER_BANK) ? BANK0 : BANK1) + (i % JP_BOXES_PER_BANK) * JP_BOX_SIZE;
            collect_box(box, list, &count);
        }
        um[US_BOXNUM_REL] = 0x80 | 0;
    } else {
        um[US_BOXNUM_REL] = jm[JP_BOXNUM_REL];                  /* boxes not set up yet: the Stadium formats them itself */
    }
    for (m = 0; m < US_BOXES; m++) {
        int first = m * US_BOX_MONS, n = count - first;
        unsigned char *bank_box = view + ((m < US_BOXES_PER_BANK) ? BANK0 : BANK1) + (m % US_BOXES_PER_BANK) * US_BOX_SIZE;
        if (n < 0) n = 0;
        if (n > US_BOX_MONS) n = US_BOX_MONS;
        write_us_box(bank_box, list, first, n);
        if (m == 0) memcpy(um + US_CURBOX_REL, bank_box, US_BOX_SIZE);       /* the current box's own copy in the main data */
    }
    free(list);

    /* -- checksum of the main data: 0xFF - (sum of the bytes from the name up to, not including, the checksum byte) -- */
    sum = 0;
    for (i = NAME_REL; i < US_CSUM_REL; i++) sum += um[i];
    um[US_CSUM_REL] = (unsigned char)(0xFF - (sum & 0xFF));
    (void)JP_MAIN_SIZE; (void)US_MAIN_SIZE;
    return 1;
}
