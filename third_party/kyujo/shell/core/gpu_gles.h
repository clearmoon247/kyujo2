/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * gpu_gles.h - the OpenGL ES 3.0 back end of the RDP model (rdp_gpu.h). Native shell only.
 *
 * The back end owns a render thread: the thread calls host->make_current() once, draws everything on it, and calls host->release() at the end. The
 * shell that creates the context (EGL surfaceless in the headless front, SDL2 on Switch and Android) must therefore NOT keep it current in another thread.
 */
#ifndef GPU_GLES_H
#define GPU_GLES_H

#include <stdint.h>
#include <stddef.h>

typedef struct GlesHost {
    void *user;
    int (*make_current)(void *user);        /* called on the render thread; returns 0 on success */
    void (*release)(void *user);
    /* Optional: showing the picture in a window. Both are called on the render thread. swap shows the back buffer of the window's surface; window_size
     * gives the drawable size in pixels. Without them (headless) gles_present does nothing. */
    void (*swap)(void *user);
    void (*window_size)(void *user, int *w, int *h);
    const char *cache_dir;                                  /* where shader-keys.bin lives (NULL: the list of shaders is not kept) */
} GlesHost;

/* Starts the render thread and installs the back end (gpu_install). Returns 0 on success. */
int gles_start(const GlesHost *host);
void gles_stop(void);
/* Waits until the render thread has run everything queued and drops its caches (before the game's memory is replaced by a new instance). */
void gles_quiesce(void);
/* 1: the picture fills the window; 0 (default): it keeps its shape with black bars. Any thread. */
void gles_set_stretch(int on);
void gles_graphics_go(void);               /* the game's first-start page is over (or not needed): the shader warm-up may begin, behind the start-up picture */
void gles_set_backdrop(const unsigned char *rgb, unsigned w, unsigned h);   /* the game's menu wallpaper tile (RGB, copied): the start-up picture uses it */
void gles_show_notice(const char *heading, const char *line1, const char *line2);   /* the start-up picture with a message (upper case, digits, . : / - ! only); used when the game stops by itself */
int gles_cycle_filter(void);               /* picture scaling style: 0 smooth, 1 sharp, 2 firm; returns the new one */
int gles_filter(void);
void gles_set_filter(int mode);

/* Shows a finished picture (8-bit RGB, top row first) in the window: queued to the render thread, which uploads it, draws it letterboxed into the default
 * framebuffer and swaps. Copies the data. Safe to call from any thread. */
void gles_present(const unsigned char *rgb, unsigned w, unsigned h);

/* Counters for diagnostics (read at exit). */
typedef struct GlesStats { uint64_t batches, triangles, fills, textures, shaders, syncs, unsupported_blend, gl_errors; } GlesStats;
void gles_stats(GlesStats *out);

/* Pictures shown straight from a surface so far (see gpu_show_picture); main_sdl counts the uploaded ones itself. */
uint64_t gles_pictures_shown(void);
/* A line telling where the render thread's time went since the last call (and resets the counters). */
void gles_period_stats(char *buf, size_t n);

/* One line saying what the render thread is doing (for a stall report). */
void gles_describe(char *buf, size_t n);

#endif
