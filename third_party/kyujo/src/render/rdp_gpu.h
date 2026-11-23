/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp_gpu.h - the interface between the RDP model (rdp.c: state, texture memory, texture decoding) and a GPU back end (shell/core/gpu_gles.c).
 * HOST-side, plain C types only; compiled into the native shell (RDP_SHELL_NATIVE) only.
 * Design and reasons: docs/switch-android-plan.md, section 11.
 *
 * The front end (rdp.c) keeps everything the console's RDP keeps - registers, TMEM, tiles - and turns each drawing command into
 *   - a GpuState (all settings the pixel stage needs),
 *   - vertices in pixel coordinates (triangle lists; rectangles become two triangles),
 *   - textures: the tile's texels decoded to RGBA8888 (so the GPU never sees TMEM formats) and cached by content.
 * The back end draws them into colour surfaces (one per N64 colour image) and can read a surface back as pictures or as N64 frame-buffer bytes.
 *
 * Calls come from the thread that runs the RDP (the graphics task); the back end queues them to its own render thread, except the calls that return
 * data (gpu_read_*), which wait for the queue to drain first.
 */
#ifndef RDP_GPU_H
#define RDP_GPU_H

#include <stdint.h>

/* One vertex. x, y in pixels of the colour image (y down); z in 0..1 (0 = near; already quantised the way the z-buffer holds it); r,g,b,a 0..255;
 * sw, tw, w: the texture coordinates divided by w (or the plain coordinates and 1 when perspective correction is off) - interpolated linearly in screen
 * space, the fragment shader divides. */
typedef struct GpuVert { float x, y, z, r, g, b, a, sw, tw, w; } GpuVert;

/* How one of the (up to two) textures of a primitive is sampled; the fields are the tile's, the sampling code is rdp.c's sample_tile_px. */
typedef struct GpuTex {
    uint32_t id;                    /* back-end texture (from gpu_texture), 0 = this input is not used */
    int16_t w, h;                   /* size of the decoded texture */
    int16_t sl, tl;                 /* tile upper left, 10.2 fixed point */
    int16_t sw, th;                 /* tile size in texels */
    uint8_t shift_s, shift_t, clamp_s, clamp_t, mirror_s, mirror_t, mask_s, mask_t;
    uint8_t fmt, siz;               /* the tile's texel format and size (gbi.h G_IM_FMT_*, G_IM_SIZ_*): copy mode tests them */
} GpuTex;

/* Everything the pixel stage depends on. */
typedef struct GpuState {
    uint32_t comb_hi, comb_lo;      /* Set_Combine_Mode */
    uint32_t omh, oml;              /* Set_Other_Modes (omh = bits 55:32) */
    uint8_t prim[4], env[4], fog[4], blend[4];
    uint8_t prim_lod_frac, cycle;   /* cycle: 0 one, 1 two, 2 copy, 3 fill */
    int16_t k4, k5;
    uint16_t prim_z;
    int16_t scis_x0, scis_y0, scis_x1, scis_y1;      /* scissor box in pixels, x1/y1 exclusive */
    uint32_t cimg_addr, zimg_addr;
    uint16_t cimg_width;
    uint8_t cimg_siz, cimg_fmt;
    uint8_t has_tex, has_shade, has_z, z_prim;    /* has_z: the primitive takes part in z-buffering; z_prim: its depth comes from the primitive-depth register (the vertices carry it already) */
    uint16_t surf_h;                /* rows the surface needs (the scissor's bottom, at least) */
    GpuTex tex[2];
} GpuState;

#ifdef __cplusplus
extern "C" {
#endif

int gpu_active(void);                                   /* a back end is installed and running */
uint32_t gpu_tex_epoch(void);                           /* changes when the back end forgets all textures: ids from before are invalid */

/* The colour combiner's inputs, resolved (rdp.c): idx[cycle][operand A,B,C,D][channel R,G,B,A] indexes the table v[] below. */
enum { GPU_V_COMB = 0, GPU_V_T0 = 4, GPU_V_T1 = 8, GPU_V_PRIM = 12, GPU_V_SHADE = 16, GPU_V_ENV = 20, GPU_V_ONE = 24, GPU_V_ZERO = 25, GPU_V_NOISE = 26,
       GPU_V_K4 = 27, GPU_V_K5 = 28, GPU_V_LOD_FRAC = 29, GPU_V_PRIM_LOD_FRAC = 30, GPU_V_COUNT = 31 };
void rdp_comb_plan(uint32_t hi, uint32_t lo, uint8_t idx[2][4][4]);

/* Triangle list: nverts is a multiple of 3. */
void gpu_draw(const GpuState *st, const GpuVert *v, int nverts);
/* Fill mode: the rectangle [x0,x1) x [y0,y1) takes the fill colour (two 16-bit pixels or one 32-bit pixel, as in the register). If the colour image is the
 * z-buffer image the rectangle clears the depth instead. */
void gpu_fill(const GpuState *st, int x0, int y0, int x1, int y1, uint32_t fill_color);
/* A texture: w*h texels, bytes R,G,B,A. Returns an id; equal contents return the same id (the back end caches by hash). */
uint32_t gpu_texture(const uint32_t *rgba, int w, int h);

/* Copies the part of RDRAM the colour surfaces cover back into game memory: called before something reads game memory that the GPU may have drawn to
 * (a texture load, a CPU read). [addr, addr+len): only the surfaces overlapping it are written back. */
void gpu_sync_to_ram(uint32_t addr, uint32_t len);
/* [addr, addr+len) of game memory now belongs to something else: colour surfaces over it are dropped WITHOUT being copied back (they are dead). */
void gpu_invalidate(uint32_t addr, uint32_t len);
/* The picture the video interface shows: the colour surface that contains `origin`. Returns 1 and the RGB data (3 bytes per pixel, w*h) or 0 if no surface
 * covers it (the caller then falls back to game memory). */
int gpu_read_picture(uint32_t origin, uint32_t stride_px, uint32_t bytes_per_px, unsigned w, unsigned h, int row_offset, uint8_t *rgb);
/* Same picture, shown in the window straight from the colour surface on the back end's own thread: no read-back, and the caller does not wait for the
 * GPU (only when the GPU is more than a couple of pictures behind). Returns 1 when the picture was queued, 0 when it cannot be shown this way (no back
 * end, no window, or no surface covers the origin): the caller then uses gpu_read_picture / game memory. */
int gpu_show_picture(uint32_t origin, uint32_t stride_px, uint32_t bytes_per_px, unsigned w, unsigned h, int row_offset);

/* Called by the front end when the task ends / the RDP is reset. */
void gpu_flush(void);
void gpu_reset(void);

/* Installed by the shell before the first draw. */
typedef struct GpuBackend {
    void (*draw)(const GpuState *st, const GpuVert *v, int nverts);
    void (*fill)(const GpuState *st, int x0, int y0, int x1, int y1, uint32_t fill_color);
    uint32_t (*texture)(const uint32_t *rgba, int w, int h);
    void (*sync_to_ram)(uint32_t addr, uint32_t len);
    int (*read_picture)(uint32_t origin, uint32_t stride_px, uint32_t bytes_per_px, unsigned w, unsigned h, int row_offset, uint8_t *rgb);
    void (*invalidate)(uint32_t addr, uint32_t len);
    int (*show_picture)(uint32_t origin, uint32_t stride_px, uint32_t bytes_per_px, unsigned w, unsigned h, int row_offset);
    void (*flush)(void);
    void (*reset)(void);
    uint32_t (*tex_epoch)(void);
} GpuBackend;
void gpu_install(const GpuBackend *b);

#ifdef __cplusplus
}
#endif

#endif
