/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * gfx.h - the graphics side of the host (RSP display-list interpreter, software RDP, video output). HOST-side, internal to src/render.
 * The game side reaches it only through the functions declared in src/platform/platform.h (host_gfx_task, host_vi_commit).
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* Resets the RSP-side state (matrices, segments, geometry mode...). Called at the start of every graphics task: the microcode's data is
 * reloaded from the task at every start, so nothing survives from the previous task. */
void gfx_reset_rsp(void);

/* Runs the display list starting at `start` (a segmented or physical address). Returns the number of RDP full syncs it contained. */
uint32_t gfx_run_display_list(uint32_t start);

/* The resumable form (f3dex2.c): gfx_task_begin sets the list up; gfx_task_run executes up to `budget` commands (0 = no limit) and returns
 * HOST_GFX_* (platform.h). Stops before a command while `*yield_req` is nonzero. *syncs receives the syncs run in this call. The state of
 * the task is kept between calls, so the next gfx_task_run continues it. */
void gfx_task_begin(uint32_t start);
int gfx_task_run(uint32_t budget, const volatile int *yield_req, uint32_t *syncs);

/* Video output (vi_out.c). */
typedef struct ViRegs {
    uint32_t control, origin, width, h_video, v_video, x_scale, y_scale;
} ViRegs;
/* Converts the frame the VI would scan out into 8-bit RGB. `rgb` must hold at least max_w * max_h * 3 bytes. Returns 1 and the size on
 * success, 0 when the VI is blank or its registers describe nothing displayable. */
int vi_scanout(const ViRegs *regs, uint8_t *rgb, unsigned max_w, unsigned max_h, unsigned *out_w, unsigned *out_h);

/* Shows the same picture in the window without reading it back, when the GPU back end holds it (see gpu_show_picture): returns 1 when it was queued
 * (the caller must not present it again), 0 when vi_scanout has to be used. */
int vi_present(const ViRegs *regs);

/* PNG writer (png_out.c): 8-bit RGB, uncompressed deflate. Returns 0 on success. */
int png_write_rgb(const char *path, const uint8_t *rgb, unsigned w, unsigned h);

#endif
