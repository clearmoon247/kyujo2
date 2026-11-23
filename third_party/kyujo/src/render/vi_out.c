/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * vi_out.c - what the video interface (VI) shows: reads the frame buffer the game points it at and produces RGB pixels.
 * Register meanings: n64brew "Video Interface" page (VI_CTRL, VI_ORIGIN, VI_WIDTH, VI_H_VIDEO, VI_V_VIDEO, VI_X_SCALE, VI_Y_SCALE).
 *
 * Modelled: pixel size (16-bit 5551 / 32-bit 8888) and the size of the visible picture in SOURCE pixels (active window times the scale
 * factors, 2.10 fixed point); the picture is returned at that size, one output pixel per source pixel (no resampling).
 * NOT modelled (deviations): the VI's anti-aliasing / resampling filters, divot filter, dither filter, gamma. Interlace (serrate) is shown
 * as the single field the origin points to, with the second field's vertical offset removed (see "interlace field offset" below).
 * HOST-side.
 */
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "rdram.h"
#ifdef RDP_GPU
#include "rdp_gpu.h"
#endif

int vi_scanout(const ViRegs *r, uint8_t *rgb, unsigned max_w, unsigned max_h, unsigned *out_w, unsigned *out_h)
{
    unsigned type = r->control & 3;
    int h_start = (int)((r->h_video >> 16) & 0x3FF), h_end = (int)(r->h_video & 0x3FF);
    int v_start = (int)((r->v_video >> 16) & 0x3FF), v_end = (int)(r->v_video & 0x3FF);
    unsigned xs = r->x_scale & 0xFFF, xo = (r->x_scale >> 16) & 0xFFF, ys = r->y_scale & 0xFFF, yo = (r->y_scale >> 16) & 0xFFF;
    unsigned stride = r->width & 0xFFF, bpp = (type == 3) ? 4u : 2u, x, y, w, h;
    int dy = 0;     /* interlace field offset, in output rows (0 or 1) */

    if (type < 2 || xs == 0 || ys == 0 || stride == 0 || h_end <= h_start || v_end <= v_start) {
        return 0;
    }
    w = (unsigned)((h_end - h_start) * (int)xs / 1024);
    h = (unsigned)(((v_end - v_start) >> 1) * (int)ys / 1024);
    if (w == 0 || h == 0 || w > max_w || h > max_h) {
        return 0;
    }
    if (!host_game_range_ok(r->origin, 1)) {                   /* garbage frame-buffer address: show nothing (every pixel is checked again below) */
        return 0;
    }
    /*
     * Interlace field offset.  With VI_CONTROL bit 6 (serrate) the game alternates two VI register sets, one per field.  libultra's
     * interlaced NTSC modes (lib/ultralib/src/vimodes/vimodentsc{h,l}{a,p}{f,n}{1,2}.c, all ten checked) give field 0 vStart 35 and field 1
     * vStart 37 (half-line units, so 2 = one frame line lower on the TV).  In the hi-res modes (yscale 2.0, e.g. osViModeNtscHpf1) the origins
     * also differ by exactly one row (field 0 = row 1, field 1 = row 2), so on a real TV the two fields weave into ONE steady picture.
     * Showing a single field verbatim therefore moved the picture up and down by one row on every other frame (the "vertical shaking in the
     * menus" the Windows tester reported).  Subtracting the vStart difference, converted to output rows, makes both fields show the same rows.
     * Output rows per frame line = ys / 2048, so the hi-res menus get 1 and the low-res interlaced modes (which keep the SAME origin in both
     * fields, i.e. one source row spans two frame lines) get 0 - integer division, the half-row shift cannot be represented.
     * Assumption (not checked on hardware): the baseline vStart is 35 for field 0; no other value occurs in libultra's mode tables.
     */
    if ((r->control & 0x40u) && v_start > 35) {
        dy = (int)(((unsigned)(v_start - 35) * ys) / 4096u);
        if (dy > 1) {
            dy = 1;
        }
    }
#ifdef RDP_GPU
    if (gpu_active() && gpu_read_picture(r->origin + (xo >> 10) * bpp, stride, bpp, w, h, (int)(yo >> 10) - dy, rgb)) {
        *out_w = w;
        *out_h = h;
        return 1;
    }
#endif
    for (y = 0; y < h; y++) {
        int sy = (int)y + (int)(yo >> 10) - dy;     /* the output has one pixel per SOURCE pixel (w, h above already include the scale); the offset is a sub-pixel start */
        for (x = 0; x < w; x++) {
            unsigned sx = x + (xo >> 10);
            uint32_t off = (uint32_t)(sy * (int)stride + (int)sx) * bpp;     /* may wrap below the origin (row -1 of field 1): that is intended, the sum is formed modulo 2^32 */
            uint32_t addr = r->origin + off;
            const uint8_t *fb = RDRAM_PTR(addr);
            uint8_t *o = rgb + ((size_t)y * w + x) * 3;
            if (!host_game_range_ok(addr, bpp)) {
                o[0] = o[1] = o[2] = 0;
                continue;
            }
            if (bpp == 2) {
                unsigned v = ((unsigned)fb[0] << 8) | fb[1];
                unsigned rr = (v >> 11) & 31, gg = (v >> 6) & 31, bb = (v >> 1) & 31;
                o[0] = (uint8_t)((rr << 3) | (rr >> 2)); o[1] = (uint8_t)((gg << 3) | (gg >> 2)); o[2] = (uint8_t)((bb << 3) | (bb >> 2));
            } else {
                o[0] = fb[0]; o[1] = fb[1]; o[2] = fb[2];
            }
        }
    }
    *out_w = w;
    *out_h = h;
    return 1;
}

/* The same register decoding as vi_scanout, for the direct path: the GPU shows its colour surface itself. */
int vi_present(const ViRegs *r)
{
#ifdef RDP_GPU
    unsigned type = r->control & 3;
    int h_start = (int)((r->h_video >> 16) & 0x3FF), h_end = (int)(r->h_video & 0x3FF);
    int v_start = (int)((r->v_video >> 16) & 0x3FF), v_end = (int)(r->v_video & 0x3FF);
    unsigned xs = r->x_scale & 0xFFF, xo = (r->x_scale >> 16) & 0xFFF, ys = r->y_scale & 0xFFF, yo = (r->y_scale >> 16) & 0xFFF;
    unsigned stride = r->width & 0xFFF, bpp = (type == 3) ? 4u : 2u, w, h;
    int dy = 0;
    if (!gpu_active()) return 0;
    if (type < 2 || xs == 0 || ys == 0 || stride == 0 || h_end <= h_start || v_end <= v_start) return 0;
    w = (unsigned)((h_end - h_start) * (int)xs / 1024);
    h = (unsigned)(((v_end - v_start) >> 1) * (int)ys / 1024);
    if (w == 0 || h == 0 || w > 1024 || h > 768) return 0;
    if (!host_game_range_ok(r->origin, 1)) return 0;
    if ((r->control & 0x40u) && v_start > 35) {            /* interlace field offset: see vi_scanout */
        dy = (int)(((unsigned)(v_start - 35) * ys) / 4096u);
        if (dy > 1) dy = 1;
    }
    return gpu_show_picture(r->origin + (xo >> 10) * bpp, stride, bpp, w, h, (int)(yo >> 10) - dy);
#else
    (void)r;
    return 0;
#endif
}
