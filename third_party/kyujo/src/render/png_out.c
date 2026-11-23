/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * png_out.c - writes an 8-bit RGB PNG. Used by the frame dump (KYUJO_DUMP_DIR) that lets the renderer be checked without a window.
 * The image data is stored in "uncompressed" deflate blocks, so no compression library is needed and the file is easy to audit.
 * Format: PNG specification (signature, IHDR, IDAT with a zlib stream, IEND; CRC-32 per chunk; Adler-32 of the zlib payload).
 * HOST-side.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfx.h"

static uint32_t sCrcTable[256];
static int sCrcReady;

static void crc_init(void)
{
    uint32_t n, k;
    for (n = 0; n < 256; n++) {
        uint32_t c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        sCrcTable[n] = c;
    }
    sCrcReady = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    if (!sCrcReady) crc_init();
    for (i = 0; i < n; i++) crc = sCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Writes one chunk: length, type, data, CRC over type+data. */
static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8], tail[4];
    uint32_t crc;
    put32(hdr, len);
    hdr[4] = (uint8_t)type[0]; hdr[5] = (uint8_t)type[1]; hdr[6] = (uint8_t)type[2]; hdr[7] = (uint8_t)type[3];
    crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    put32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

int png_write_rgb(const char *path, const uint8_t *rgb, unsigned w, unsigned h)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13];
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3), z_len, pos = 0, y;
    uint8_t *raw, *z;
    uint32_t a = 1, b = 0;
    size_t i;
    int ok;

    if (!f) return 1;
    raw = (uint8_t *)malloc(raw_len);
    z_len = 2 + raw_len + ((raw_len + 65534) / 65535) * 5 + 4;
    z = (uint8_t *)malloc(z_len);
    if (!raw || !z) { free(raw); free(z); fclose(f); return 1; }
    for (y = 0; y < h; y++) {                                   /* each scanline starts with the filter type 0 (none) */
        raw[y * (1 + (size_t)w * 3)] = 0;
        for (i = 0; i < (size_t)w * 3; i++) raw[y * (1 + (size_t)w * 3) + 1 + i] = rgb[y * (size_t)w * 3 + i];
    }
    for (i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    z[pos++] = 0x78; z[pos++] = 0x01;                           /* zlib header: deflate, 32K window, no compression hint */
    for (i = 0; i < raw_len;) {
        size_t n = raw_len - i > 65535 ? 65535 : raw_len - i;
        z[pos++] = (i + n == raw_len) ? 1 : 0;                  /* BFINAL, BTYPE = 00 (stored) */
        z[pos++] = (uint8_t)n; z[pos++] = (uint8_t)(n >> 8); z[pos++] = (uint8_t)~n; z[pos++] = (uint8_t)(~n >> 8);
        for (y = 0; y < n; y++) z[pos++] = raw[i + y];
        i += n;
    }
    put32(z + pos, (b << 16) | a); pos += 4;
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;       /* 8 bit, colour type 2 (RGB), deflate, no filter set, no interlace */
    ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (uint32_t)pos) && chunk(f, "IEND", 0, 0);
    free(raw); free(z);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : 1;
}
