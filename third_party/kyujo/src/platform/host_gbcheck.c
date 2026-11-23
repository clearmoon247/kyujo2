/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* host_gbcheck.c - see host_gbcheck.h. Pure functions (no I/O), tested by tests/native/gbjp/test_gbcheck.c. */
#include <string.h>

#include "host_gbcheck.h"

int gb_gen1_main_save_valid(const unsigned char *ram)
{
    const unsigned char *name = ram + 0x2598, *p;
    unsigned sum = 0;
    int marker = 0, i;
    for (i = 0; i < 11; i++) if (name[i] == 0x50) marker = 1;
    for (p = ram + 0x2598; p <= ram + 0x3523; p++) sum += *p;
    return marker && (sum & 0xFF) == 0xFF;
}

int gb_title_is_gen1_international(const unsigned char *t)
{
    /* the title field is 15 bytes, padded with zeros */
    return (strncmp((const char *)t, "POKEMON RED", 11) == 0 && t[11] == 0)
        || (strncmp((const char *)t, "POKEMON BLUE", 12) == 0 && t[12] == 0)
        || (strncmp((const char *)t, "POKEMON YELLOW", 14) == 0 && t[14] == 0);
}
