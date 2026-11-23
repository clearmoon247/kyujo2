/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp.h - the software RDP (the console's rasteriser) as used by this port. HOST-side header (plain C types only).
 *
 * What the real RDP does: it receives 64-bit commands (from the RSP microcode) that set up its state (textures, colours, modes) and draw
 * (fill rectangles, texture rectangles, triangles). It draws into the RDRAM: a colour image (16- or 32-bit pixels), optionally a z-buffer.
 * Everything the game sees of the RDP is in RDRAM, so this implementation writes the SAME kind of pixels to the SAME addresses; the video
 * interface (vi_out.c) then reads them like the real VI would.
 *
 * Command word layout (bit numbers are for the 64-bit command, see docs/rdp-notes.md for the sources): the first 32-bit word of a command
 * is called `hi` (bits 63:32), the second `lo` (bits 31:0), exactly as the game's display lists hold them (gbi.h "w0" and "w1").
 *
 * Known deviations from the hardware are listed at the top of rdp.c ("Deviations").
 */
#ifndef RDP_H
#define RDP_H

#include <stdint.h>

/* One transformed vertex, as the RSP side hands it to the rasteriser. */
typedef struct RdpVert {
    float x, y;          /* screen position in pixels (y grows downwards) */
    float z;             /* screen depth, 0 (near) .. 1023 (far): the viewport's z range (G_MAXZ) */
    float r, g, b, a;    /* shade colour, 0..255 */
    float s, t;          /* texture coordinates in texels of the tile (already scaled by gSPTexture) */
    float invw;          /* 1 / clip-space w, > 0 */
} RdpVert;

/* Resets all RDP state (as at power-up). */
void rdp_reset(void);

/* Executes one RDP command that fits in 64 bits and is not a triangle or a texture rectangle. `hi`/`lo` as described above. The op code is
 * the low 6 bits of the top byte of `hi` (the RSP hands them over with the 0xC0 bits set, as in gbi.h; those are ignored here). */
void rdp_command(uint32_t hi, uint32_t lo);

/* The RSP microcode keeps the "other modes" in two halves and sends them to the RDP as one command. `hi` holds bits 55:32 of the
 * 64-bit Set_Other_Modes command (gbi.h's "othermode high" word, G_MDSFT_* shifts), `lo` bits 31:0. */
void rdp_set_other_modes(uint32_t hi, uint32_t lo);

/* A texture rectangle: the two 64-bit words of the RDP command (4 x 32 bits: w0/w1 of G_TEXRECT, then the two RDPHALF words). */
void rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip);

/* A triangle. `tile` and `level` come from the command (G_TEXTURE), the flags say which attribute blocks the command carries. */
void rdp_triangle(const RdpVert *a, const RdpVert *b, const RdpVert *c, int tile, int level, int has_shade, int has_tex, int has_z);

/* Number of full syncs seen since reset (the caller converts it into the DP interrupt). */
uint32_t rdp_full_syncs(void);
/* The game's memory [addr, addr+len) now belongs to something else (the main pool handed it out): a colour surface the GPU back end holds over it is dead and is dropped
 * without being copied back into the game's memory (port patch 0035). Does nothing without a GPU back end. */
void rdp_memory_reused(uint32_t addr, uint32_t len);

/* Address of the current colour image (diagnostics). */
uint32_t rdp_color_image_addr(void);
void rdp_cimg_log_reset(void);                 /* a new task starts: forget the colour images the previous one selected */
int rdp_task_may_draw_to(uint32_t origin);     /* the running task selected a colour image starting at origin (or it is unknown) */

/* Development: hash of the colour image the last display list left (see KYUJO_GFX_BENCH in gfx_host.c). 0 if there is none. */
uint64_t rdp_dev_color_image_hash(void);

/* Reports (once, at exit or on request) which features the game used that this renderer does not model. Prints to stderr. */
void rdp_report_unimplemented(void);

/* Development profile (KYUJO_RDP_PROFILE=<min ms>): after a display list that took `task_ns`, prints the cost per kind of drawing command
 * if it took at least <min ms>, and clears the table. Does nothing when the variable is not set. */
void rdp_profile_report(unsigned long long task_ns);

/* Statistics of the work done since reset: triangles, rectangles, pixels written. */
void rdp_stats(uint32_t *triangles, uint32_t *rects, uint64_t *pixels);

#endif
