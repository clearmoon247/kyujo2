/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper
 * Canvas and font drawing from Kyujo host_gameui.c. Kyujo2 uses the two menu
 * sizes and tile extracted at build time so first-run setup can draw them. */
#include "menu_canvas.h"
#include <stddef.h>
#include "kyujo_menu_assets.inc"
#define HEADER_BYTES 0x90u
static int cell(int size) {
    return size == 4 ? 24 : 16;
}
static const unsigned char *font(int size) {
    return size == 4 ? title_font : list_font;
}
static int glyph_slot(unsigned char c) {
    if (c >= 0xa0)
        return glyph_table[0x60 + c - 0xa0];
    if (c >= 0x20 && c < 0x80)
        return glyph_table[c - 0x20];
    return 0;
}
static int advance(int size, unsigned char c) {
    return font(size)[glyph_slot(c)];
}
void host_canvas_blend(HostCanvas *cv, int x, int y, unsigned rgb, unsigned alpha) {
    unsigned fx = cv->fx ? cv->fx : 65536u, fy = cv->fy ? cv->fy : 65536u;
    long px0, px1, py0, py1, px, py;
    unsigned inv;
    if (x < 0 || y < 0 || alpha == 0)
        return;
    if (alpha > 255)
        alpha = 255;
    inv = 255 - alpha;
    px0 = (long)(((unsigned long long)x * fx) >> 16);
    px1 = (long)(((unsigned long long)(x + 1) * fx) >> 16);
    py0 = (long)(((unsigned long long)y * fy) >> 16);
    py1 = (long)(((unsigned long long)(y + 1) * fy) >> 16);
    if (px1 <= px0)
        px1 = px0 + 1; /* a shrunk picture still shows every drawing pixel, once or overlapped */
    if (py1 <= py0)
        py1 = py0 + 1;
    for (py = py0; py < py1 && py < cv->h; py++) {
        for (px = px0; px < px1 && px < cv->w; px++) {
            unsigned char *p = cv->rgb + ((size_t)py * (size_t)cv->w + (size_t)px) * 3u;
            p[0] = (unsigned char)((p[0] * inv + (rgb >> 16 & 255) * alpha + 127) / 255);
            p[1] = (unsigned char)((p[1] * inv + (rgb >> 8 & 255) * alpha + 127) / 255);
            p[2] = (unsigned char)((p[2] * inv + (rgb & 255) * alpha + 127) / 255);
        }
    }
}

void host_canvas_fill(HostCanvas *cv, int x, int y, int w, int h, unsigned rgb, unsigned alpha) {
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            host_canvas_blend(cv, x + i, y + j, rgb, alpha);
}

int host_text_width(int size, const char *s) {
    int x = 0, widest = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            if (x > widest)
                widest = x;
            x = 0;
            continue;
        }
        x += advance(size, (unsigned char)*s) - (size >= 2 ? 2 : 1);
    }
    return x > widest ? x : widest;
}

void host_text_draw(HostCanvas *cv, int size, int x, int y, const char *s, unsigned rgb, int shadow) {
    int x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int adv, slot, gx, gy;
        const unsigned char *bm;
        if (c == '\n') {
            x = x0;
            y += cell(size) + 2;
            continue;
        }
        slot = glyph_slot(c);
        adv = font(size)[slot];
        bm = font(size) + HEADER_BYTES + (unsigned)slot * cell(size) * cell(size);
        for (gy = 0; gy < cell(size); gy++) {
            for (gx = 0; gx < adv && gx < cell(size); gx++) {
                unsigned v = bm[gy * cell(size) + gx], inten = v >> 4, alpha = v & 15;
                if (alpha == 0)
                    continue;
                if (shadow)
                    host_canvas_blend(cv, x + gx + 1, y + gy + 1, 0x000000u,
                                      alpha * 17 / 2); /* a soft dark edge, half as strong */
                host_canvas_blend(cv, x + gx, y + gy,
                                  ((rgb >> 16 & 255) * inten / 15) << 16 |
                                      ((rgb >> 8 & 255) * inten / 15) << 8 | ((rgb & 255) * inten / 15),
                                  alpha * 17);
            }
        }
        x += adv - (size >= 2 ? 2 : 1);
    }
}

void menu_background(HostCanvas *cv) {
    static const int prim[] = {120, 100, 40}, env[] = {180, 180, 100};
    for (int y = 0; y < cv->h; ++y)
        for (int x = 0; x < cv->w; ++x) {
            int idx = (y % 64) * 128 + x % 128;
            int texel = (idx & 1 ? menu_tile[idx / 2] & 15 : menu_tile[idx / 2] >> 4) * 17;
            for (int c = 0; c < 3; ++c)
                cv->rgb[(y * cv->w + x) * 3 + c] = prim[c] + (env[c] - prim[c]) * texel / 255;
        }
    host_canvas_fill(cv, 28, 20, 584, 440, 0x04081C, 150);
    host_canvas_fill(cv, 28, 20, 584, 2, 0xffffff, 110);
    host_canvas_fill(cv, 28, 458, 584, 2, 0xffffff, 110);
    host_canvas_fill(cv, 28, 20, 2, 440, 0xffffff, 110);
    host_canvas_fill(cv, 610, 20, 2, 440, 0xffffff, 110);
}
void menu_cursor(HostCanvas *cv, int y) {
    host_canvas_fill(cv, 44, y - 2, 550, 22, 0xffffff, 46);
    for (int i = 0; i < 6; ++i)
        for (int j = i - 1; j <= 10 - i && j < 6; ++j)
            if (j >= 0 && j <= 11 - 2 * i)
                host_canvas_blend(cv, 48 + i, y + 3 + j, 0xffe680, 255);
}

int host_gameui_font_ok(int size) { return size == 2 || size == 4; }
const unsigned char *host_gameui_background(void) {
    static unsigned char background[640 * 480 * 3];
    static int ready;
    if (!ready) {
        const unsigned char prim[] = {120,100,40}, env[] = {180,180,100};
        for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x) {
            unsigned idx = (y % 64) * 128 + x % 128;
            unsigned shade = (idx & 1 ? menu_tile[idx/2] & 15 : menu_tile[idx/2] >> 4) * 17;
            for (int c = 0; c < 3; ++c)
                background[(y * 640 + x) * 3 + c] = prim[c] + (env[c] - prim[c]) * shade / 255;
        }
        ready = 1;
    }
    return background;
}
