/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/* gpu_dispatch.c - the entry points of rdp_gpu.h, forwarded to the installed back end (gpu_gles.c). */
#include <string.h>

#include "rdp_gpu.h"

static GpuBackend sB;

void gpu_install(const GpuBackend *b) { sB = *b; }
int gpu_active(void) { return sB.draw != NULL; }
uint32_t gpu_tex_epoch(void) { return sB.tex_epoch ? sB.tex_epoch() : 0; }
void gpu_draw(const GpuState *st, const GpuVert *v, int n) { sB.draw(st, v, n); }
void gpu_fill(const GpuState *st, int x0, int y0, int x1, int y1, uint32_t c) { sB.fill(st, x0, y0, x1, y1, c); }
uint32_t gpu_texture(const uint32_t *rgba, int w, int h) { return sB.texture(rgba, w, h); }
void gpu_sync_to_ram(uint32_t addr, uint32_t len) { if (sB.sync_to_ram) sB.sync_to_ram(addr, len); }
int gpu_read_picture(uint32_t origin, uint32_t stride_px, uint32_t bpp, unsigned w, unsigned h, int row_offset, uint8_t *rgb)
{
    return sB.read_picture ? sB.read_picture(origin, stride_px, bpp, w, h, row_offset, rgb) : 0;
}
int gpu_show_picture(uint32_t origin, uint32_t stride_px, uint32_t bpp, unsigned w, unsigned h, int row_offset)
{
    return sB.show_picture ? sB.show_picture(origin, stride_px, bpp, w, h, row_offset) : 0;
}
void gpu_invalidate(uint32_t addr, uint32_t len) { if (sB.invalidate) sB.invalidate(addr, len); }
void gpu_flush(void) { if (sB.flush) sB.flush(); }
void gpu_reset(void) { if (sB.reset) sB.reset(); }
