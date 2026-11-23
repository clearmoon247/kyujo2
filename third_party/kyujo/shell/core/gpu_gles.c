/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * gpu_gles.c - OpenGL ES 3.0 back end of the RDP model (rdp_gpu.h; design: docs/switch-android-plan.md, section 11). Native shell only.
 *
 * WHAT IT DOES. The front end (src/render/rdp.c) hands over batches of triangles with the complete pixel-stage settings. This file draws them into one GL
 * texture per N64 colour image ("surface"), with a fragment shader that reproduces the RDP's colour combiner, texture sampling (3-point filter, wrap, mirror,
 * clamp, shift), alpha test and the first blender cycle with the same integer arithmetic as the software renderer (rdp.c). The last blender cycle uses
 * fixed-function blending for the forms the game uses (docs/switch-android-plan.md 11.1): ordinary alpha blend, reversed alpha blend, "keep the frame buffer".
 * Other forms are drawn without blending and counted (GlesStats.unsupported_blend).
 *
 * THREADS. The RDP thread only appends to a queue; the render thread owns the GL context. Calls that return data (picture, write-back to game memory) wait
 * until the queue before them has been executed.
 *
 * DEVIATIONS (all measured against the software renderer by the headless test): colour surfaces are RGBA8, not 5-5-5-1, so there is no 5-bit banding; dither is
 * not drawn; the z-buffer is a 24-bit depth buffer holding the same linear 14-bit depth as the software renderer; coverage is always full.
 * STATUS: written and tested on Mesa llvmpipe (GLES 3.2) only; not run on a Switch or an Android device.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GLES3/gl3.h>

#include "gpu_gles.h"
#include "rdp_gpu.h"
#include "rdram.h"

/* ---- what the render thread is doing (read by the watchdog) ------------------------------------------------------------------------ */
static volatile const char *gPhase = "idle";
static volatile uint64_t gPhaseSince;
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void phase(const char *what) { gPhase = what; gPhaseSince = now_ms(); }

/* ---- commands for the render thread ------------------------------------------------------------------------------------------------- */
enum { OP_DRAW = 1, OP_FILL, OP_TEXTURE, OP_DELETE_ALL, OP_SYNC, OP_READ, OP_FLUSH, OP_RESET, OP_QUIT, OP_PRESENT, OP_SHOW, OP_INVALIDATE };

typedef struct Cmd {
    struct Cmd *next;
    int op;
    GpuState st;
    int x0, y0, x1, y1;
    uint32_t color;
    void *data;                     /* vertices or texels, owned by the command */
    int count;
    uint32_t id;
    int w, h;
    uint32_t addr, len;             /* sync */
    uint32_t stride_px, bpp;        /* read */
    int row_offset;
    uint8_t *out;
    int result;
    volatile int done;
    int skip;                       /* a show that a newer one replaced before it started (set under gQm while queued) */
} Cmd;

static pthread_mutex_t gQm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gQc = PTHREAD_COND_INITIALIZER;       /* the render thread waits for work */
static pthread_cond_t gQd = PTHREAD_COND_INITIALIZER;       /* producers wait for room or for a result */
static Cmd *gHead, *gTail;
static int gQn;
#define QUEUE_MAX 8192
static pthread_t gThread;
static int gRunning;
static GlesHost gHost;
static GlesStats gStats;
static volatile int gStarted, gStartFailed;
static uint32_t gEpoch = 1;

/* Pictures shown straight from a surface (gles_show_picture): queued and finished counts (the game may be at most SHOW_AHEAD pictures ahead of the GPU),
 * and where the render thread's time went since the last gles_period_stats (nanoseconds per kind of command). */
static uint64_t gShowDone, gShowDropped;                   /* guarded by gQm */
static struct Cmd *gLastShow;                              /* the newest show command that is still queued (not yet taken by the render thread); guarded by gQm */
enum { T_DRAW, T_FILL, T_TEXTURE, T_SYNC_READ, T_SHOW, T_SWAP, T_PRESENT, T_OTHER, T_N };
static uint64_t gBusyNs[T_N];                              /* written by the render thread only; read racily for diagnostics */
static uint64_t gShowsPeriod, gPresentsPeriod, gShowTotal;
static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int gWarmGo;                                          /* the shader warm-up may start (gles_graphics_go, or the first command that draws); guarded by gQm */
static int gWarmDone;                                        /* the warm-up has run */
static int gSplashForce;                                     /* gles_graphics_go: the start-up picture shows again even though pictures of the menu were drawn since; guarded by gQm */
static int gSplashDirty;                                     /* the start-up picture must be drawn again (a new backdrop, a restart); guarded by gQm */
static void push(Cmd *c)
{
    pthread_mutex_lock(&gQm);
    while (gQn >= QUEUE_MAX) pthread_cond_wait(&gQd, &gQm);
    c->next = NULL;
    if (gTail) gTail->next = c; else gHead = c;
    gTail = c;
    gQn++;
    if (c->op != OP_PRESENT) gWarmGo = 1;                   /* the game draws: the graphics set-up (shader warm-up) must come first, whether or not it was asked for */
    pthread_cond_signal(&gQc);
    pthread_mutex_unlock(&gQm);
}

static void wait_done(Cmd *c)
{
    pthread_mutex_lock(&gQm);
    while (!c->done) pthread_cond_wait(&gQd, &gQm);
    pthread_mutex_unlock(&gQm);
}

/* ---- producer side: batching, texture cache, shadow of the surfaces --------------------------------------------------------------- */
static pthread_mutex_t gProd = PTHREAD_MUTEX_INITIALIZER;     /* the producer-side state: several game threads call in (the RDP thread draws, the main thread asks for pictures) */
static GpuState gPendSt;
static GpuVert *gPendV;
static int gPendN, gPendCap, gPendValid;

static void flush_pending(void)
{
    Cmd *c;
    if (!gPendValid || gPendN == 0) { gPendValid = 0; gPendN = 0; return; }
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_DRAW;
    c->st = gPendSt;
    c->data = malloc((size_t)gPendN * sizeof(GpuVert));
    memcpy(c->data, gPendV, (size_t)gPendN * sizeof(GpuVert));
    c->count = gPendN;
    gPendN = 0;
    gPendValid = 0;
    push(c);
}

typedef struct Shadow { uint32_t addr, width, siz, rows; int dirty; } Shadow;
#define MAX_SHADOW 64
static Shadow gShadow[MAX_SHADOW];
static int gShadowN;

static uint32_t shadow_bytes(const Shadow *s) { return s->width * s->rows * (s->siz == 3 ? 4u : 2u); }

static void shadow_touch(const GpuState *st)
{
    int i;
    Shadow *s = NULL;
    if (st->cimg_siz != 2 && st->cimg_siz != 3) return;
    for (i = 0; i < gShadowN; i++) {
        if (gShadow[i].addr == st->cimg_addr && gShadow[i].width == st->cimg_width && gShadow[i].siz == st->cimg_siz) { s = &gShadow[i]; break; }
    }
    if (!s) {
        if (gShadowN == MAX_SHADOW) { memmove(&gShadow[0], &gShadow[1], (MAX_SHADOW - 1) * sizeof gShadow[0]); gShadowN--; }
        s = &gShadow[gShadowN++];
        s->addr = st->cimg_addr; s->width = st->cimg_width; s->siz = st->cimg_siz; s->rows = 0; s->dirty = 0;
    }
    if (st->surf_h > s->rows) s->rows = st->surf_h;
    s->dirty = 1;
}

static void gles_draw(const GpuState *st, const GpuVert *v, int nverts)
{
    pthread_mutex_lock(&gProd);
    if (gPendValid && memcmp(&gPendSt, st, sizeof *st) == 0) {
        /* same settings as the batch being collected */
    } else {
        flush_pending();
        gPendSt = *st;
        gPendValid = 1;
    }
    if (gPendN + nverts > gPendCap) {
        gPendCap = (gPendN + nverts) * 2 + 256;
        gPendV = (GpuVert *)realloc(gPendV, (size_t)gPendCap * sizeof(GpuVert));
    }
    memcpy(gPendV + gPendN, v, (size_t)nverts * sizeof(GpuVert));
    gPendN += nverts;
    shadow_touch(st);
    if (gPendN > 30000) flush_pending();
    pthread_mutex_unlock(&gProd);
}

static void gles_fill(const GpuState *st, int x0, int y0, int x1, int y1, uint32_t color)
{
    Cmd *c;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_FILL; c->st = *st; c->x0 = x0; c->y0 = y0; c->x1 = x1; c->y1 = y1; c->color = color;
    if (st->cimg_addr != st->zimg_addr) shadow_touch(st);
    push(c);
    pthread_mutex_unlock(&gProd);
}

/* texture cache: content hash -> id */
typedef struct TexEnt { uint64_t hash; uint32_t id; int w, h; } TexEnt;
#define TEX_SLOTS 8192
#define TEX_LIMIT 3000
static TexEnt gTex[TEX_SLOTS];
static uint32_t gTexNext = 1;
static int gTexCount;

static uint64_t hash_texels(const uint32_t *p, int w, int h)
{
    uint64_t x = 1469598103934665603ULL ^ ((uint64_t)w << 32) ^ (uint64_t)h;
    size_t n = (size_t)w * (size_t)h, i;
    for (i = 0; i < n; i++) { x ^= p[i]; x *= 1099511628211ULL; x ^= x >> 29; }
    return x ? x : 1;
}

static uint32_t gles_texture(const uint32_t *rgba, int w, int h)
{
    uint64_t hv = hash_texels(rgba, w, h);
    uint32_t slot = (uint32_t)(hv % TEX_SLOTS), n;
    Cmd *c;
    pthread_mutex_lock(&gProd);
    for (n = 0; n < TEX_SLOTS; n++) {
        TexEnt *e = &gTex[(slot + n) % TEX_SLOTS];
        if (e->hash == hv && e->w == w && e->h == h) { uint32_t id = e->id; pthread_mutex_unlock(&gProd); return id; }
        if (!e->hash) break;
    }
    if (gTexCount >= TEX_LIMIT) {                           /* forget everything: draws already queued still use the old ids, which the render thread deletes after them */
        flush_pending();
        c = (Cmd *)calloc(1, sizeof *c); c->op = OP_DELETE_ALL; push(c);
        memset(gTex, 0, sizeof gTex);
        gTexCount = 0;
        gEpoch++;
        slot = (uint32_t)(hv % TEX_SLOTS);
    }
    for (n = 0; n < TEX_SLOTS; n++) {
        TexEnt *e = &gTex[(slot + n) % TEX_SLOTS];
        if (!e->hash) { e->hash = hv; e->w = w; e->h = h; e->id = gTexNext++; gTexCount++; break; }
    }
    {
        TexEnt *e = NULL;
        for (n = 0; n < TEX_SLOTS; n++) { TexEnt *q = &gTex[(slot + n) % TEX_SLOTS]; if (q->hash == hv && q->w == w && q->h == h) { e = q; break; } }
        c = (Cmd *)calloc(1, sizeof *c);
        c->op = OP_TEXTURE; c->id = e->id; c->w = w; c->h = h;
        c->data = malloc((size_t)w * (size_t)h * 4u);
        memcpy(c->data, rgba, (size_t)w * (size_t)h * 4u);
        push(c);
        pthread_mutex_unlock(&gProd);
        return e->id;
    }
}

static uint32_t gles_tex_epoch(void) { return gEpoch; }

static void gles_sync_to_ram(uint32_t addr, uint32_t len)
{
    int i, need = 0;
    Cmd *c;
    pthread_mutex_lock(&gProd);
    for (i = 0; i < gShadowN; i++) {
        const Shadow *s = &gShadow[i];
        if (s->dirty && addr < s->addr + shadow_bytes(s) && s->addr < addr + len) need = 1;
    }
    pthread_mutex_unlock(&gProd);
    if (!need) return;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_SYNC; c->addr = addr; c->len = len;
    push(c);
    pthread_mutex_unlock(&gProd);
    wait_done(c);
    free(c);
    pthread_mutex_lock(&gProd);
    for (i = 0; i < gShadowN; i++) {
        const Shadow *s = &gShadow[i];
        if (addr < s->addr + shadow_bytes(s) && s->addr < addr + len) gShadow[i].dirty = 0;
    }
    pthread_mutex_unlock(&gProd);
}

static int gles_read_picture(uint32_t origin, uint32_t stride_px, uint32_t bpp, unsigned w, unsigned h, int row_offset, uint8_t *rgb)
{
    Cmd *c;
    int r;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_READ; c->addr = origin; c->stride_px = stride_px; c->bpp = bpp; c->w = (int)w; c->h = (int)h; c->row_offset = row_offset; c->out = rgb;
    push(c);
    pthread_mutex_unlock(&gProd);
    wait_done(c);
    r = c->result;
    free(c);
    return r;
}

/* The picture straight from the surface that holds it, on the render thread. Decided here, on the producer side, whether a surface covers the picture
 * (the shadow list mirrors the render thread's surfaces); otherwise 0 and the caller reads game memory as before. */
/* The game's memory [addr, addr+len) was handed to something else: the surfaces over it are dead. Producer side: forget the shadows, then tell the render thread
 * (in order with the draws queued before, which still belong to the old owner). */
static void gles_invalidate(uint32_t addr, uint32_t len)
{
    int i, any = 0;
    Cmd *c;
    pthread_mutex_lock(&gProd);
    for (i = 0; i < gShadowN; ) {
        const Shadow *s = &gShadow[i];
        if (addr < s->addr + shadow_bytes(s) && s->addr < addr + len) {
            memmove(&gShadow[i], &gShadow[i + 1], (size_t)(gShadowN - i - 1) * sizeof gShadow[0]);
            gShadowN--;
            any = 1;
        } else i++;
    }
    if (any) {
        flush_pending();
        c = (Cmd *)calloc(1, sizeof *c);
        c->op = OP_INVALIDATE; c->addr = addr; c->len = len;
        push(c);
    }
    pthread_mutex_unlock(&gProd);
}

static int gles_show_picture(uint32_t origin, uint32_t stride_px, uint32_t bpp, unsigned w, unsigned h, int row_offset)
{
    int i, found = 0;
    Cmd *c;
    static int off = -1;
    if (off < 0) off = getenv("KYUJO_NO_DIRECT_PRESENT") != NULL;      /* development / fallback: always read the picture back */
    if (off || !gRunning || !gHost.swap) return 0;
    pthread_mutex_lock(&gProd);
    for (i = 0; i < gShadowN; i++) {
        const Shadow *s = &gShadow[i];
        uint32_t sbpp = s->siz == 3 ? 4u : 2u, off, x0;
        if (s->width != stride_px || sbpp != bpp) continue;
        if (origin < s->addr || origin >= s->addr + shadow_bytes(s) || (origin - s->addr) % bpp) continue;
        off = (origin - s->addr) / bpp;
        x0 = off % s->width;
        if (x0 + w > s->width) continue;                    /* the picture wraps into the next row: not a rectangle of the surface */
        found = 1;
        break;
    }
    if (!found) { pthread_mutex_unlock(&gProd); return 0; }
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_SHOW; c->addr = origin; c->stride_px = stride_px; c->bpp = bpp; c->w = (int)w; c->h = (int)h; c->row_offset = row_offset;
    pthread_mutex_lock(&gQm);
    if (gLastShow) gLastShow->skip = 1;                     /* the GPU is behind: the older picture that has not started is never shown, this newer one replaces it */
    gLastShow = c;
    pthread_mutex_unlock(&gQm);
    push(c);                                                /* the game's thread never waits for the picture (it would hold up every other game thread) */
    pthread_mutex_unlock(&gProd);
    return 1;
}

static void gles_flush(void)
{
    Cmd *c;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_FLUSH;
    push(c);
    pthread_mutex_unlock(&gProd);
    wait_done(c);
    free(c);
}

static void gles_reset(void)
{
    Cmd *c;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_RESET;
    push(c);
    pthread_mutex_unlock(&gProd);
    wait_done(c);
    free(c);
    pthread_mutex_lock(&gProd);
    gShadowN = 0;
    pthread_mutex_unlock(&gProd);
}

/* ---- render thread: GL objects ---------------------------------------------------------------------------------------------------- */
typedef struct Surf {
    uint32_t addr, width, siz;
    int h;
    GLuint tex, fbo;
    GLuint depth_attached;
    int dirty;
    uint64_t serial;
} Surf;
#define MAX_SURF 64
static Surf gSurf[MAX_SURF];
static int gSurfN;
static uint64_t gSerial;

typedef struct DepthBuf { uint32_t addr; int w, h; GLuint rb; } DepthBuf;
#define MAX_DEPTH 8
static DepthBuf gDepth[MAX_DEPTH];
static int gDepthN;

static GLuint *gTexName;        /* texture id -> GL name */
static uint32_t gTexNameCap;
static GLuint gVao, gVbo, gScratchFbo;

static void gl_check(const char *where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        if (gStats.gl_errors++ < 8) fprintf(stderr, "gles: GL error 0x%X after %s\n", e, where);
    }
}

static uint8_t c5to8(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }

/* RDRAM -> RGBA8 for a new surface (the game may have prepared the frame buffer without the RDP). */
static void surf_upload_from_ram(Surf *s)
{
    size_t n = (size_t)s->width * (size_t)s->h, i;
    uint32_t bpp = s->siz == 3 ? 4u : 2u;
    uint8_t *px = (uint8_t *)malloc(n * 4u);
    const uint8_t *src = gRdpMem && (uint64_t)s->addr + n * bpp <= gRdpMemSize ? gRdpMem + s->addr : NULL;
    if (!px) return;
    for (i = 0; i < n; i++) {
        if (!src) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; continue; }
        if (bpp == 2) {
            unsigned v = ((unsigned)src[i * 2] << 8) | src[i * 2 + 1];
            px[i * 4] = c5to8((v >> 11) & 31); px[i * 4 + 1] = c5to8((v >> 6) & 31); px[i * 4 + 2] = c5to8((v >> 1) & 31); px[i * 4 + 3] = 255;
        } else {
            px[i * 4] = src[i * 4]; px[i * 4 + 1] = src[i * 4 + 1]; px[i * 4 + 2] = src[i * 4 + 2]; px[i * 4 + 3] = 255;
        }
    }
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->width, s->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
}

static void surf_alloc(Surf *s, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)s->width, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s->tex) {                                           /* grow: copy the old rows */
        GLuint old = s->tex, f2;
        glGenFramebuffers(1, &f2);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f2);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glBlitFramebuffer(0, 0, (GLint)s->width, s->h, 0, 0, (GLint)s->width, s->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &f2);
        glDeleteFramebuffers(1, &s->fbo);
        glDeleteTextures(1, &old);
        s->fbo = 0;
        s->tex = t;
        s->h = h;
    } else {
        s->tex = t;
        s->h = h;
        surf_upload_from_ram(s);
    }
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    s->depth_attached = 0;
    gl_check("surface");
}

static void surf_free(Surf *s)
{
    glDeleteFramebuffers(1, &s->fbo);
    glDeleteTextures(1, &s->tex);
    memset(s, 0, sizeof *s);
}

static void surf_write_back(Surf *s);

static Surf *surf_get(const GpuState *st)
{
    int i, h = st->surf_h > 0 ? st->surf_h : 1;
    uint32_t bpp = st->cimg_siz == 3 ? 4u : 2u;
    Surf *s = NULL;
    for (i = 0; i < gSurfN; i++) {
        if (gSurf[i].addr == st->cimg_addr && gSurf[i].width == st->cimg_width && gSurf[i].siz == st->cimg_siz) { s = &gSurf[i]; break; }
    }
    if (!s) {
        /* a surface of another shape over the same memory is obsolete: write it back if it holds pictures, then drop it */
        for (i = 0; i < gSurfN; ) {
            Surf *o = &gSurf[i];
            uint32_t ob = o->width * (uint32_t)o->h * (o->siz == 3 ? 4u : 2u);
            if (st->cimg_addr < o->addr + ob && o->addr < st->cimg_addr + (uint32_t)st->cimg_width * (uint32_t)h * bpp) {
                if (o->dirty) surf_write_back(o);
                surf_free(o);
                memmove(o, o + 1, (size_t)(gSurfN - i - 1) * sizeof *o);
                gSurfN--;
            } else i++;
        }
        if (gSurfN == MAX_SURF) { surf_free(&gSurf[0]); memmove(&gSurf[0], &gSurf[1], (MAX_SURF - 1) * sizeof gSurf[0]); gSurfN--; }
        s = &gSurf[gSurfN++];
        memset(s, 0, sizeof *s);
        s->addr = st->cimg_addr; s->width = st->cimg_width; s->siz = st->cimg_siz;
        surf_alloc(s, h);
    } else if (h > s->h) {
        surf_alloc(s, h);
    }
    s->serial = ++gSerial;
    return s;
}

static DepthBuf *depth_get(uint32_t addr, int w, int h)
{
    int i;
    DepthBuf *d = NULL;
    for (i = 0; i < gDepthN; i++) if (gDepth[i].addr == addr) { d = &gDepth[i]; break; }
    if (!d) {
        if (gDepthN == MAX_DEPTH) { glDeleteRenderbuffers(1, &gDepth[0].rb); memmove(&gDepth[0], &gDepth[1], (MAX_DEPTH - 1) * sizeof gDepth[0]); gDepthN--; }
        d = &gDepth[gDepthN++];
        memset(d, 0, sizeof *d);
        d->addr = addr;
    }
    if (w > d->w || h > d->h) {
        if (d->rb) glDeleteRenderbuffers(1, &d->rb);
        d->w = w > d->w ? w : d->w;
        d->h = h > d->h ? h : d->h;
        glGenRenderbuffers(1, &d->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, d->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, d->w, d->h);
        /* a fresh buffer is "farthest" everywhere until the game clears it */
        glBindFramebuffer(GL_FRAMEBUFFER, gScratchFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->rb);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepthf(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    }
    return d;
}

/* ---- shaders ---------------------------------------------------------------------------------------------------------------------- */
typedef struct ShaderKey {
    uint32_t comb_hi, comb_lo, omh, oml;
    uint8_t cycle, tex_used0, tex_used1, has_shade, copy_rgba16, pad[3];
} ShaderKey;

typedef struct Program {
    ShaderKey key;
    GLuint prog;
    GLint u_scale, u_prim, u_env, u_fog, u_blend, u_misc, u_tex[4][2], u_s0, u_s1;
} Program;
#define MAX_PROG 1024
static Program gProg[MAX_PROG];
static int gProgN;

typedef struct Str { char *p; size_t n, cap; } Str;
static void sa(Str *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (s->n + (size_t)n + 1 > s->cap) { s->cap = (s->n + (size_t)n + 1) * 2 + 1024; s->p = (char *)realloc(s->p, s->cap); }
    memcpy(s->p + s->n, buf, (size_t)n + 1);
    s->n += (size_t)n;
}

/* The blender plan of the last (or only) cycle. */
typedef struct BlendPlan { int mode; int psel, asel, msel, bsel; } BlendPlan;
enum { BM_NONE = 0, BM_SHADER, BM_ALPHA, BM_ALPHA_REV, BM_KEEP, BM_ALPHA_SQ };

static BlendPlan blend_plan(uint32_t oml, int cycle, int *unsupported)
{
    BlendPlan b;
    int fc = cycle == 1, force = (oml >> 14) & 1;
    memset(&b, 0, sizeof b);
    b.psel = (int)((oml >> (fc ? 28 : 30)) & 3);
    b.asel = (int)((oml >> (fc ? 24 : 26)) & 3);
    b.msel = (int)((oml >> (fc ? 20 : 22)) & 3);
    b.bsel = (int)((oml >> (fc ? 16 : 18)) & 3);
    b.mode = BM_NONE;
    if (!force || cycle > 1) return b;
    if (cycle == 1 && (((oml >> 30) & 3) == 1 || ((oml >> 22) & 3) == 1)) {
        /* Frame buffer in the first cycle.  The one form that is used (Oak's Lab screens): both cycles are
           "pixel over frame buffer" with the same alpha, R1 = (P*A + M*(1-A))*A + M*(1-A) = P*A^2 + M*(1-A^2),
           which is an ordinary alpha blend with the alpha squared. */
        int p0 = (int)((oml >> 30) & 3), a0 = (int)((oml >> 26) & 3), m0 = (int)((oml >> 22) & 3), b0 = (int)((oml >> 18) & 3);
        if (p0 == 0 && m0 == 1 && b0 == 0 && b.psel == 0 && b.msel == 1 && b.bsel == 0 && a0 == b.asel) { b.mode = BM_ALPHA_SQ; return b; }
        *unsupported = 1; b.mode = BM_NONE; return b;
    }
    if (b.psel != 1 && b.msel != 1) { b.mode = BM_SHADER; return b; }
    if (b.psel == 1 && b.msel == 1) { *unsupported = 1; b.mode = BM_NONE; return b; }
    if (b.msel == 1) {
        if ((b.asel == 3 && b.bsel == 2) || (b.asel == 3 && b.bsel == 0)) { b.mode = BM_KEEP; return b; }
        if (b.bsel == 0) { b.mode = BM_ALPHA; return b; }
    } else {
        if (b.bsel == 0) { b.mode = BM_ALPHA_REV; return b; }
    }
    *unsupported = 1;
    b.mode = BM_NONE;
    return b;
}

static const char *kVs =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec4 aCol;\n"
    "layout(location=2) in vec3 aTc;\n"
    "uniform vec2 uScale;\n"
    "out vec4 vCol;\n"
    "out vec3 vTc;\n"
    "void main() {\n"
    "  vCol = aCol; vTc = aTc;\n"
    "  gl_Position = vec4(aPos.x * uScale.x - 1.0, aPos.y * uScale.y - 1.0, aPos.z * 2.0 - 1.0, 1.0);\n"
    "}\n";

static const char *rgb_sel(int sel, const char *cc)       /* blender colour selector (P or M): 0 pixel/combiner, 1 frame buffer, 2 blend colour, 3 fog colour */
{
    static char buf[4][64];
    static int k;
    char *o = buf[k++ & 3];
    switch (sel) {
    case 0: snprintf(o, 64, "%s.rgb", cc); break;
    case 2: snprintf(o, 64, "uBlend.rgb"); break;
    case 3: snprintf(o, 64, "uFog.rgb"); break;
    default: snprintf(o, 64, "ivec3(0)"); break;
    }
    return o;
}
static const char *a_sel(int sel)        /* blender A: 0 pixel alpha, 1 fog alpha, 2 shade alpha, 3 zero */
{
    switch (sel) { case 0: return "apix"; case 1: return "uFog.a"; case 2: return "shade.a"; default: return "0"; }
}
static const char *b_sel(int sel)        /* blender B: 0 one minus A, 1 frame-buffer alpha (always 255 here), 2 one, 3 zero */
{
    switch (sel) { case 0: return "(255 - A_)"; case 1: return "255"; case 2: return "255"; default: return "0"; }
}

static char *gen_fs(const ShaderKey *k)
{
    Str s = {0};
    uint8_t idx[2][4][4];
    int cyc = k->cycle, ncyc = cyc == 1 ? 2 : 1, c, op, ch, dummy = 0;
    BlendPlan bp = blend_plan(k->oml, cyc, &dummy);
    int cvg_x_alpha = (k->oml >> 12) & 1, alpha_cvg_sel = (k->oml >> 13) & 1, alpha_cmp = k->oml & 1, dither_alpha = (k->oml >> 1) & 1;
    int bilerp = (k->omh >> 13) & 1;
    static const char ch_name[4] = { 'x', 'y', 'z', 'w' };

    rdp_comb_plan(k->comb_hi, k->comb_lo, idx);
    sa(&s, "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n");
    sa(&s, "in vec4 vCol;\nin vec3 vTc;\nout vec4 oCol;\n");
    sa(&s, "uniform ivec4 uPrim, uEnv, uFog, uBlend, uMisc;\n");           /* uMisc: k4, k5, prim lod frac, noise seed */
    sa(&s, "uniform ivec4 uT[8];\n");                                        /* per texture i: uT[4*i+0] = (w, h, sl, tl), +1 = (sw, th, shift_s, shift_t), +2 = (clamp_s, clamp_t, mirror_s, mirror_t), +3 = (mask_s, mask_t, 0, 0) */
    sa(&s, "uniform sampler2D uTex0;\nuniform sampler2D uTex1;\n");
    sa(&s,
       "int wrapc(int c, int cl, int mi, int mask, int size) {\n"
       "  if (cl != 0) c = clamp(c, 0, size - 1);\n"
       "  if (mask != 0) { int m = (1 << mask) - 1; if (mi != 0 && (c & (1 << mask)) != 0) c = ~c; c &= m; }\n"
       "  return c;\n}\n"
       "ivec4 fetchT(sampler2D tx, int i, int s, int t) {\n"
       "  ivec2 sz = ivec2(uT[4 * i].x, uT[4 * i].y);\n"
       "  return ivec4(texelFetch(tx, ivec2(clamp(s, 0, sz.x - 1), clamp(t, 0, sz.y - 1)), 0) * 255.0 + 0.5);\n}\n"
       "ivec4 sampleT(sampler2D tx, int i, float s, float t) {\n"
       "  int sf = int(floor(s * 32.0)), tf = int(floor(t * 32.0));\n"
       "  ivec4 a = uT[4 * i], b = uT[4 * i + 1], c = uT[4 * i + 2], d = uT[4 * i + 3];\n"
       "  if (b.z != 0) sf = (b.z <= 10) ? (sf >> b.z) : (sf << (16 - b.z));\n"
       "  if (b.w != 0) tf = (b.w <= 10) ? (tf >> b.w) : (tf << (16 - b.w));\n"
       "  sf -= a.z << 3; tf -= a.w << 3;\n");
    if (bilerp) {
        sa(&s,
           "  int s0 = sf >> 5, t0 = tf >> 5, fs = sf & 31, ft = tf & 31, u, v;\n"
           "  int ws0 = wrapc(s0, c.x, c.z, d.x, b.x), ws1 = wrapc(s0 + 1, c.x, c.z, d.x, b.x);\n"
           "  int wt0 = wrapc(t0, c.y, c.w, d.y, b.y), wt1 = wrapc(t0 + 1, c.y, c.w, d.y, b.y);\n"
           "  ivec4 pa, pb, pc;\n"
           "  if (fs + ft < 32) { pa = fetchT(tx, i, ws0, wt0); pb = fetchT(tx, i, ws1, wt0); pc = fetchT(tx, i, ws0, wt1); u = fs; v = ft; }\n"
           "  else { pa = fetchT(tx, i, ws1, wt1); pb = fetchT(tx, i, ws0, wt1); pc = fetchT(tx, i, ws1, wt0); u = 32 - fs; v = 32 - ft; }\n"
           "  return pa + (((pb - pa) * u + (pc - pa) * v) >> 5);\n}\n");
    } else {
        sa(&s,
           "  return fetchT(tx, i, wrapc(sf >> 5, c.x, c.z, d.x, b.x), wrapc(tf >> 5, c.y, c.w, d.y, b.y));\n}\n");
    }
    sa(&s, "int eq(int a, int b, int c, int d) { return clamp((((a - b) * c + 128) >> 8) + d, 0, 255); }\n");
    sa(&s, "void main() {\n");
    sa(&s, "  ivec4 shade = %s;\n", k->has_shade ? "ivec4(clamp(floor(vCol + 0.5), 0.0, 255.0))" : "ivec4(0)");
    sa(&s, "  float iw = max(vTc.z, 1e-20);\n  float ts = vTc.x / iw, tt = vTc.y / iw;\n");
    sa(&s, "  int noise = int(fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + float(uMisc.w)) * 43758.5453) * 256.0);\n");
    sa(&s, "  ivec4 t0 = ivec4(0), t1 = ivec4(0);\n");
    if (k->tex_used0) sa(&s, "  t0 = sampleT(uTex0, 0, ts, tt);\n");
    if (k->tex_used1) sa(&s, "  t1 = sampleT(uTex1, 1, ts, tt);\n");
    if (cyc == 2) {
        /* copy mode: the texel is written as it is, after the alpha test */
        if (alpha_cmp) {
            if (k->copy_rgba16) sa(&s, "  if (t0.a == 0) discard;\n");
            else sa(&s, "  if (t0.a < uBlend.a) discard;\n");
        }
        sa(&s, "  oCol = vec4(vec3(t0.rgb) / 255.0, 1.0);\n}\n");
        return s.p;
    }
    sa(&s, "  int v[31];\n  ivec4 cmb = ivec4(0);\n");
    for (c = 0; c < 4; c++) sa(&s, "  v[%d] = 0; v[%d] = t0.%c; v[%d] = t1.%c; v[%d] = uPrim.%c; v[%d] = shade.%c; v[%d] = uEnv.%c;\n",
                               c, 4 + c, ch_name[c], 8 + c, ch_name[c], 12 + c, ch_name[c], 16 + c, ch_name[c], 20 + c, ch_name[c]);
    sa(&s, "  v[24] = 256; v[25] = 0; v[26] = noise; v[27] = uMisc.x; v[28] = uMisc.y; v[29] = 0; v[30] = uMisc.z;\n");
    for (c = 0; c < ncyc; c++) {
        if (c == 1) {
            int q;
            for (q = 0; q < 4; q++) sa(&s, "  v[%d] = cmb.%c; v[%d] = t1.%c; v[%d] = t0.%c;\n", q, ch_name[q], 4 + q, ch_name[q], 8 + q, ch_name[q]);
        }
        sa(&s, "  cmb = ivec4(");
        for (ch = 0; ch < 4; ch++) {
            sa(&s, "%seq(v[%d], v[%d], v[%d], v[%d])", ch ? ", " : "", idx[c][0][ch], idx[c][1][ch], idx[c][2][ch], idx[c][3][ch]);
        }
        sa(&s, ");\n");
        (void)op;
    }
    if (alpha_cmp) sa(&s, "  if (cmb.a < %s) discard;\n", dither_alpha ? "noise" : "uBlend.a");
    sa(&s, "  int apix = cmb.a;\n");
    if (cvg_x_alpha) sa(&s, "  if (apix < 16) discard;\n");
    if (alpha_cvg_sel) sa(&s, "  apix = %s;\n", cvg_x_alpha ? "cmb.a" : "255");
    sa(&s, "  vec4 outc = vec4(vec3(cmb.rgb) / 255.0, 1.0);\n");
    if (cyc == 1 && ((k->oml >> 14) & 1)) {
        int p0 = (int)((k->oml >> 30) & 3), a0 = (int)((k->oml >> 26) & 3), m0 = (int)((k->oml >> 22) & 3), b0 = (int)((k->oml >> 18) & 3);
        if (p0 != 1 && m0 != 1) {
            sa(&s, "  ivec3 mid;\n  {\n    ivec3 pr = %s, mr = %s;\n    int A_ = clamp(%s, 0, 255);\n    int B_ = clamp(%s, 0, 255);\n", rgb_sel(p0, "cmb"), rgb_sel(m0, "cmb"), a_sel(a0), b_sel(b0));
            sa(&s, "    mid = (A_ + B_ == 0) ? pr : clamp((pr * A_ + mr * B_ + (A_ + B_) / 2) / (A_ + B_), ivec3(0), ivec3(255));\n  }\n");
            sa(&s, "  ivec4 cmb2 = ivec4(mid, cmb.a);\n");
        } else {
            sa(&s, "  ivec4 cmb2 = cmb;\n");
        }
    } else {
        sa(&s, "  ivec4 cmb2 = cmb;\n");
    }
    switch (bp.mode) {
    case BM_SHADER:
        sa(&s, "  {\n    ivec3 pr = %s, mr = %s;\n    int A_ = clamp(%s, 0, 255);\n    int B_ = clamp(%s, 0, 255);\n", rgb_sel(bp.psel, "cmb2"), rgb_sel(bp.msel, "cmb2"), a_sel(bp.asel), b_sel(bp.bsel));
        sa(&s, "    ivec3 r = (A_ + B_ == 0) ? pr : clamp((pr * A_ + mr * B_ + (A_ + B_) / 2) / (A_ + B_), ivec3(0), ivec3(255));\n    outc = vec4(vec3(r) / 255.0, 1.0);\n  }\n");
        break;
    case BM_ALPHA:
        sa(&s, "  outc = vec4(vec3(%s) / 255.0, float(clamp(%s, 0, 255)) / 255.0);\n", rgb_sel(bp.psel, "cmb2"), a_sel(bp.asel));
        break;
    case BM_ALPHA_REV:
        sa(&s, "  outc = vec4(vec3(%s) / 255.0, float(clamp(%s, 0, 255)) / 255.0);\n", rgb_sel(bp.msel, "cmb2"), a_sel(bp.asel));
        break;
    case BM_ALPHA_SQ:
        sa(&s, "  outc = vec4(vec3(%s) / 255.0, float(clamp(%s, 0, 255) * clamp(%s, 0, 255)) / 65025.0);\n", rgb_sel(bp.psel, "cmb2"), a_sel(bp.asel), a_sel(bp.asel));
        break;
    case BM_KEEP:
        sa(&s, "  outc = vec4(0.0);\n");
        break;
    default:
        sa(&s, "  outc = vec4(vec3(cmb2.rgb) / 255.0, 1.0);\n");
        break;
    }
    sa(&s, "  oCol = outc;\n}\n");
    return s.p;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "gles: shader compile error:\n%s\n--- source ---\n%s\n", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Builds (compiles and links) the program for a key. Called on the render thread, and on the warm-up thread with a second context that shares objects with
 * the render thread's. Returns the program name, or 0 (the error is logged). */
static GLuint build_program(const ShaderKey *k, const char *who)
{
    char *fs;
    GLuint v, f, prog;
    GLint ok = 0;
    uint64_t t0 = now_ms();
    fs = gen_fs(k);
    if (getenv("KYUJO_GPU_DUMPFS")) fprintf(stderr, "gles-fs: comb=%06X/%08X oml=%08X cycle=%d\n%s\n", k->comb_hi, k->comb_lo, k->oml, k->cycle, fs);
    v = compile(GL_VERTEX_SHADER, kVs);
    f = compile(GL_FRAGMENT_SHADER, fs);
    free(fs);
    if (!v || !f) return 0;
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "gles: link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    fprintf(stderr, "gles: shader built in %u ms%s (comb=%06X/%08X oml=%08X cycle=%d)\n", (unsigned)(now_ms() - t0), who, k->comb_hi, k->comb_lo, k->oml, k->cycle);
    return prog;
}

/* ---- warm-up: shaders built before the game asks for them ----------------------------------------------------------------------------------------------
 * Building one program takes 15-170 ms on the Switch's driver, and the render thread did it the first time a draw needed it: every new effect in a battle was a
 * hitch (a fraction of a second of stale or black picture, and the game thread waits for the render thread meanwhile). The keys of all programs the game has
 * needed are therefore saved (shader-keys.bin in the game's folder, appended as they appear) and the next run builds them all right after the render thread
 * starts, while the main thread is still loading the ROMs and cartridges, before the game's first picture.
 * (A first version built them on a second thread with a second GL context that shared objects with this one. On the Switch's Mesa 20.1 that crashed at start-up
 * and the programs drew distorted pictures when used from the other context, so everything stays on this thread.) */
static ShaderKey gKnown[MAX_PROG];
static int gKnownN;
static char gKeyPath[1100];

static void keys_load(void)
{
    FILE *f;
    ShaderKey k;
    if (!gKeyPath[0] || !(f = fopen(gKeyPath, "rb"))) return;
    while (gKnownN < MAX_PROG && fread(&k, sizeof k, 1, f) == 1) {
        int i, dup = 0;
        for (i = 0; i < gKnownN; i++) if (!memcmp(&gKnown[i], &k, sizeof k)) { dup = 1; break; }
        if (!dup) gKnown[gKnownN++] = k;
    }
    fclose(f);
}
static void keys_append(const ShaderKey *k)
{
    FILE *f;
    if (gKnownN >= MAX_PROG) return;
    gKnown[gKnownN++] = *k;
    if (!gKeyPath[0] || !(f = fopen(gKeyPath, "ab"))) return;
    fwrite(k, sizeof *k, 1, f);
    fclose(f);
}
static int key_known(const ShaderKey *k)
{
    int i;
    for (i = 0; i < gKnownN; i++) if (!memcmp(&gKnown[i], k, sizeof *k)) return 1;
    return 0;
}

static Program *program_get(const ShaderKey *k)
{
    int i;
    Program *p;
    GLuint prog;
    for (i = 0; i < gProgN; i++) if (memcmp(&gProg[i].key, k, sizeof *k) == 0) return gProg[i].prog ? &gProg[i] : NULL;   /* a failed program stays failed: no new compile on every draw */
    if (gProgN == MAX_PROG) { fprintf(stderr, "gles: too many shaders\n"); return NULL; }
    phase("shader compile");
    p = &gProg[gProgN];
    memset(p, 0, sizeof *p);
    p->key = *k;
    if (!key_known(k)) keys_append(k);                           /* a program the game needed for the first time: the next run builds it in advance */
    prog = build_program(k, "");
    if (!prog) { gProgN++; return NULL; }
    p->prog = prog;
    p->u_scale = glGetUniformLocation(p->prog, "uScale");
    p->u_prim = glGetUniformLocation(p->prog, "uPrim");
    p->u_env = glGetUniformLocation(p->prog, "uEnv");
    p->u_fog = glGetUniformLocation(p->prog, "uFog");
    p->u_blend = glGetUniformLocation(p->prog, "uBlend");
    p->u_misc = glGetUniformLocation(p->prog, "uMisc");
    p->u_s0 = glGetUniformLocation(p->prog, "uTex0");
    p->u_s1 = glGetUniformLocation(p->prog, "uTex1");
    p->u_tex[0][0] = glGetUniformLocation(p->prog, "uT");
    gProgN++;
    gStats.shaders++;
    return p;
}

/* ---- render thread: drawing ----------------------------------------------------------------------------------------------------------- */
static GLuint tex_name(uint32_t id) { return id < gTexNameCap ? gTexName[id] : 0; }

static void bind_surface(const GpuState *st, Surf **out_s)
{
    Surf *s = surf_get(st);
    *out_s = s;
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, (GLsizei)s->width, s->h);
}

static int gTrace = -1;
static void do_draw(Cmd *c)
{
    const GpuState *st = &c->st;
    Surf *s;
    ShaderKey k;
    Program *p;
    int unsupported = 0, z_compare, z_update;
    BlendPlan bp;
    GLint ivals[32];
    int i, x0, y0, x1, y1;

    static int dbgNoDepth = -1, dbgNoBlend = -1;
    if (dbgNoDepth < 0) { dbgNoDepth = getenv("KYUJO_GPU_NODEPTH") != NULL; dbgNoBlend = getenv("KYUJO_GPU_NOBLEND") != NULL; }
    if (gTrace < 0) gTrace = getenv("KYUJO_GPU_TRACE") != NULL;
    if (gTrace > 0) {
        fprintf(stderr, "gpu-draw: n=%d cyc=%d comb=%06X/%08X omh=%06X oml=%08X shade=%d z=%d zp=%d tex0=%u(%dx%d fmt%d siz%d sl%d tl%d sw%d th%d sh%d,%d cl%d%d mi%d%d mk%d,%d) tex1=%u cimg=%06X/%d/%d zimg=%06X\n",
                c->count, st->cycle, st->comb_hi, st->comb_lo, st->omh, st->oml, st->has_shade, st->has_z, st->z_prim, st->tex[0].id, st->tex[0].w, st->tex[0].h,
                st->tex[0].fmt, st->tex[0].siz, st->tex[0].sl, st->tex[0].tl, st->tex[0].sw, st->tex[0].th, st->tex[0].shift_s, st->tex[0].shift_t,
                st->tex[0].clamp_s, st->tex[0].clamp_t, st->tex[0].mirror_s, st->tex[0].mirror_t, st->tex[0].mask_s, st->tex[0].mask_t, st->tex[1].id,
                st->cimg_addr, st->cimg_width, st->cimg_siz, st->zimg_addr);
    }
    if (st->cimg_siz != 2 && st->cimg_siz != 3) return;
    if (st->cimg_addr == st->zimg_addr && st->zimg_addr) return;       /* drawing into the z-buffer image itself: not modelled */
    memset(&k, 0, sizeof k);
    k.comb_hi = st->comb_hi; k.comb_lo = st->comb_lo;
    k.omh = st->omh & (1u << 13);
    k.oml = st->oml & (0xFFFF0000u | 0x7003u);
    k.cycle = st->cycle;
    k.tex_used0 = st->tex[0].id != 0; k.tex_used1 = st->tex[1].id != 0;
    k.has_shade = st->has_shade;
    k.copy_rgba16 = st->cycle == 2 && st->tex[0].fmt == 0 && st->tex[0].siz == 2;
    p = program_get(&k);
    phase("draw setup");
    if (!p) return;
    bp = blend_plan(st->oml, st->cycle, &unsupported);
    if (unsupported) {
        static int reported;
        gStats.unsupported_blend++;
        if (!reported) { reported = 1; fprintf(stderr, "gles: blender setting not supported on the GPU (drawn without blending): other modes %08X cycle %d\n", st->oml, st->cycle); }
    }
    bind_surface(st, &s);
    s->dirty = 1;

    glUseProgram(p->prog);
    glUniform2f(p->u_scale, 2.0f / (float)s->width, 2.0f / (float)s->h);
    glUniform4i(p->u_prim, st->prim[0], st->prim[1], st->prim[2], st->prim[3]);
    glUniform4i(p->u_env, st->env[0], st->env[1], st->env[2], st->env[3]);
    glUniform4i(p->u_fog, st->fog[0], st->fog[1], st->fog[2], st->fog[3]);
    glUniform4i(p->u_blend, st->blend[0], st->blend[1], st->blend[2], st->blend[3]);
    glUniform4i(p->u_misc, st->k4, st->k5, st->prim_lod_frac, (GLint)(gSerial & 0xFFFF));
    for (i = 0; i < 2; i++) {
        const GpuTex *t = &st->tex[i];
        ivals[i * 16 + 0] = t->w; ivals[i * 16 + 1] = t->h; ivals[i * 16 + 2] = t->sl; ivals[i * 16 + 3] = t->tl;
        ivals[i * 16 + 4] = t->sw; ivals[i * 16 + 5] = t->th; ivals[i * 16 + 6] = t->shift_s; ivals[i * 16 + 7] = t->shift_t;
        ivals[i * 16 + 8] = t->clamp_s; ivals[i * 16 + 9] = t->clamp_t; ivals[i * 16 + 10] = t->mirror_s; ivals[i * 16 + 11] = t->mirror_t;
        ivals[i * 16 + 12] = t->mask_s; ivals[i * 16 + 13] = t->mask_t; ivals[i * 16 + 14] = 0; ivals[i * 16 + 15] = 0;
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, t->id ? tex_name(t->id) : 0);
    }
    glUniform4iv(p->u_tex[0][0], 8, ivals);
    glUniform1i(p->u_s0, 0);
    glUniform1i(p->u_s1, 1);

    /* scissor */
    x0 = st->scis_x0 < 0 ? 0 : st->scis_x0; y0 = st->scis_y0 < 0 ? 0 : st->scis_y0;
    x1 = st->scis_x1 > (int)s->width ? (int)s->width : st->scis_x1; y1 = st->scis_y1 > s->h ? s->h : st->scis_y1;
    if (x1 <= x0 || y1 <= y0) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);

    /* depth */
    z_compare = st->has_z && ((st->oml >> 4) & 1);
    z_update = st->has_z && ((st->oml >> 5) & 1);
    if (dbgNoDepth) z_compare = z_update = 0;
    if ((z_compare || z_update) && st->zimg_addr) {
        DepthBuf *d = depth_get(st->zimg_addr, (int)s->width, s->h);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        if (s->depth_attached != d->rb) { glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->rb); s->depth_attached = d->rb; }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(z_compare ? GL_LESS : GL_ALWAYS);
        glDepthMask(z_update ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    /* blend */
    if (dbgNoBlend) bp.mode = BM_SHADER;
    switch (bp.mode) {
    case BM_ALPHA:
    case BM_ALPHA_SQ:  glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO); break;
    case BM_ALPHA_REV: glEnable(GL_BLEND); glBlendFuncSeparate(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE, GL_ZERO); break;
    case BM_KEEP:      glEnable(GL_BLEND); glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO); break;
    default:           glDisable(GL_BLEND); break;
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindVertexArray(gVao);
    glBindBuffer(GL_ARRAY_BUFFER, gVbo);
    phase("draw upload");
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c->count * sizeof(GpuVert)), c->data, GL_STREAM_DRAW);
    phase("draw call");
    glDrawArrays(GL_TRIANGLES, 0, c->count);
    gStats.batches++;
    gStats.triangles += (uint64_t)(c->count / 3);
    gl_check("draw");
}

static void do_fill(Cmd *c)
{
    const GpuState *st = &c->st;
    if (st->cimg_siz != 2 && st->cimg_siz != 3) return;
    if (st->cimg_addr == st->zimg_addr && st->zimg_addr) {
        DepthBuf *d = depth_get(st->zimg_addr, st->cimg_width, st->scis_y1 > c->y1 ? st->scis_y1 : c->y1);
        unsigned z14 = (c->color >> 16) >> 2;       /* the z-buffer clear value; rdp.c lets any fragment through where memory holds 0x3FFF ("max"), which depth 1.0 gives with LESS */
        float z = z14 >= 0x3FFFu ? 1.0f : (float)z14 / 16384.0f;
        glBindFramebuffer(GL_FRAMEBUFFER, gScratchFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->rb);
        glEnable(GL_SCISSOR_TEST);
        glScissor(c->x0, c->y0, c->x1 - c->x0, c->y1 - c->y0);
        glDepthMask(GL_TRUE);
        glClearDepthf(z);
        glClear(GL_DEPTH_BUFFER_BIT);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    } else {
        Surf *s;
        float r, g, b;
        bind_surface(st, &s);
        if (st->cimg_siz == 2) {
            unsigned v = (c->color >> 16) & 0xFFFF;
            r = c5to8((v >> 11) & 31) / 255.0f; g = c5to8((v >> 6) & 31) / 255.0f; b = c5to8((v >> 1) & 31) / 255.0f;
        } else {
            r = (float)((c->color >> 24) & 255) / 255.0f; g = (float)((c->color >> 16) & 255) / 255.0f; b = (float)((c->color >> 8) & 255) / 255.0f;
        }
        s->dirty = 1;
        glEnable(GL_SCISSOR_TEST);
        {
            int x0 = c->x0 < 0 ? 0 : c->x0, y0 = c->y0 < 0 ? 0 : c->y0, x1 = c->x1 > (int)s->width ? (int)s->width : c->x1, y1 = c->y1 > s->h ? s->h : c->y1;
            if (x1 <= x0 || y1 <= y0) return;
            glScissor(x0, y0, x1 - x0, y1 - y0);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    gStats.fills++;
    gl_check("fill");
}

static void do_texture(Cmd *c)
{
    GLuint t;
    if (c->id >= gTexNameCap) {
        uint32_t n = gTexNameCap ? gTexNameCap : 1024;
        while (n <= c->id) n *= 2;
        gTexName = (GLuint *)realloc(gTexName, n * sizeof(GLuint));
        memset(gTexName + gTexNameCap, 0, (n - gTexNameCap) * sizeof(GLuint));
        gTexNameCap = n;
    }
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gTexName[c->id] = t;
    gStats.textures++;
}

static void do_delete_all(void)
{
    uint32_t i;
    for (i = 0; i < gTexNameCap; i++) if (gTexName[i]) { glDeleteTextures(1, &gTexName[i]); gTexName[i] = 0; }
}

/* Reads the whole surface as RGBA8, rows top to bottom. */
static uint8_t *surf_read(Surf *s)
{
    uint8_t *px = (uint8_t *)malloc((size_t)s->width * (size_t)s->h * 4u);
    if (!px) return NULL;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    phase("readpixels");
    glReadPixels(0, 0, (GLsizei)s->width, s->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px;
}

static void surf_write_back(Surf *s)
{
    uint8_t *px;
    size_t n = (size_t)s->width * (size_t)s->h, i;
    uint32_t bpp = s->siz == 3 ? 4u : 2u;
    if (!gRdpMem || (uint64_t)s->addr + n * bpp > gRdpMemSize) { s->dirty = 0; return; }
    if (getenv("KYUJO_GPU_SYNCTRACE")) fprintf(stderr, "gpu: write back surface %06X width %u height %d siz %u (%u bytes)\n", s->addr, s->width, s->h, s->siz, (unsigned)(n * bpp));
    px = surf_read(s);
    if (!px) return;
    for (i = 0; i < n; i++) {
        uint8_t *d = gRdpMem + s->addr + i * bpp;
        if (bpp == 2) {
            unsigned v = ((unsigned)(px[i * 4] >> 3) << 11) | ((unsigned)(px[i * 4 + 1] >> 3) << 6) | ((unsigned)(px[i * 4 + 2] >> 3) << 1) | 1u;
            d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v;
        } else {
            d[0] = px[i * 4]; d[1] = px[i * 4 + 1]; d[2] = px[i * 4 + 2]; d[3] = 0xE0;
        }
    }
    free(px);
    s->dirty = 0;
    gStats.syncs++;
}

static void do_sync(Cmd *c)
{
    int i;
    if (getenv("KYUJO_GPU_SYNCTRACE")) fprintf(stderr, "gpu: sync request %06X + %X\n", c->addr, c->len);
    for (i = 0; i < gSurfN; i++) {
        Surf *s = &gSurf[i];
        uint32_t bytes = s->width * (uint32_t)s->h * (s->siz == 3 ? 4u : 2u);
        if (s->dirty && c->addr < s->addr + bytes && s->addr < c->addr + c->len) surf_write_back(s);
    }
}

static void do_invalidate(Cmd *c)
{
    int i;
    for (i = 0; i < gSurfN; ) {
        Surf *s = &gSurf[i];
        uint32_t bytes = s->width * (uint32_t)s->h * (s->siz == 3 ? 4u : 2u);
        if (c->addr < s->addr + bytes && s->addr < c->addr + c->len) {
            if (getenv("KYUJO_GPU_SYNCTRACE")) fprintf(stderr, "gpu: dropped dead surface %06X width %u height %d (memory %06X + %X reused)%s\n", s->addr, s->width, s->h, c->addr, c->len, s->dirty ? " - had not been copied back" : "");
            surf_free(s);
            memmove(s, s + 1, (size_t)(gSurfN - i - 1) * sizeof *s);
            gSurfN--;
        } else i++;
    }
}

static void do_read(Cmd *c)
{
    int i;
    if (gTrace > 0) fprintf(stderr, "gpu-frame origin=%06X\n", c->addr);
    Surf *best = NULL;
    c->result = 0;
    for (i = 0; i < gSurfN; i++) {
        Surf *s = &gSurf[i];
        uint32_t bytes = s->width * (uint32_t)s->h * (s->siz == 3 ? 4u : 2u);
        if (s->width != c->stride_px || (s->siz == 3 ? 4u : 2u) != c->bpp) continue;
        if (c->addr < s->addr || c->addr >= s->addr + bytes) continue;
        if ((c->addr - s->addr) % c->bpp) continue;
        if (!best || s->serial > best->serial) best = s;
    }
    if (best) {
        uint8_t *px = surf_read(best);
        uint32_t off = (c->addr - best->addr) / c->bpp;
        int y, x;
        if (!px) return;
        for (y = 0; y < c->h; y++) {
            for (x = 0; x < c->w; x++) {
                int64_t pos = (int64_t)off + (int64_t)(y + c->row_offset) * (int64_t)best->width + x;
                uint8_t *o = c->out + ((size_t)y * (size_t)c->w + (size_t)x) * 3u;
                if (pos < 0 || pos >= (int64_t)best->width * best->h) { o[0] = o[1] = o[2] = 0; continue; }
                o[0] = px[pos * 4]; o[1] = px[pos * 4 + 1]; o[2] = px[pos * 4 + 2];
            }
        }
        free(px);
        c->result = 1;
    }
}

static void do_reset(void)
{
    int i;
    for (i = 0; i < gSurfN; i++) surf_free(&gSurf[i]);
    gSurfN = 0;
}

/* ---- showing the picture in a window ---------------------------------------------------------------------------------------------- */
static GLuint gPresProg, gPresTex;
static GLint gPresLocScale = -1, gPresLocRect = -1, gPresLocClamp = -1, gPresLocPic = -1, gPresLocRectF = -1, gPresLocPicSize = -1, gPresLocOutScale = -1;
static int gPresW, gPresH;

/* Builds the presentation program on first use. Returns 0 (and turns the window output off) when it cannot. */
static int present_ready(void)
{
    if (!gHost.swap) return 0;
    if (!gPresProg) {
        static const char *vs =
            "#version 300 es\n"
            "uniform vec2 uScale;\n"
            "uniform vec4 uRect;\n"                    /* the part of the texture shown: x, y, width, height (0..1, top row first) */
            "out vec2 vUv;\n"
            "void main() {\n"
            "  vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
            "  vUv = uRect.xy + vec2(p.x, 1.0 - p.y) * uRect.zw;\n"
            "  gl_Position = vec4((p * 2.0 - 1.0) * uScale, 0.0, 1.0);\n"
            "}\n";
        static const char *fs =
            "#version 300 es\n"
            "precision highp float;\n"
            "uniform sampler2D uPic;\n"
            "uniform vec4 uClamp;\n"                   /* keeps the filter inside the shown part (min x, min y, max x, max y) */
            "uniform vec4 uRectF;\n"                   /* the part of the texture shown, as in the vertex shader */
            "uniform vec2 uPicSize;\n"                 /* the picture in texels */
            "uniform vec2 uOutScale;\n"                /* window pixels per picture texel (at least 1) */
            "in vec2 vUv;\n"
            "out vec4 oCol;\n"
            /* "Sharp bilinear": every picture texel keeps its own colour over the whole of its window pixels except for a one-window-pixel wide band along its edges,
             * where the colours blend. A plain bilinear stretch by 3-4x (the 320 x 237 battle pictures) blurs every glyph; this keeps small text crisp and the picture smooth. */
            "void main() {\n"
            "  vec2 p = (vUv - uRectF.xy) / uRectF.zw * uPicSize;\n"
            "  vec2 fl = floor(p);\n"
            "  vec2 cd = fract(p) - 0.5;\n"
            "  vec2 rr = 0.5 - 0.5 / uOutScale;\n"
            "  vec2 f = (cd - clamp(cd, -rr, rr)) * uOutScale + 0.5;\n"
            "  vec2 uv = uRectF.xy + (fl + f) / uPicSize * uRectF.zw;\n"
            "  oCol = vec4(texture(uPic, clamp(uv, uClamp.xy, uClamp.zw)).rgb, 1.0);\n"
            "}\n";
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        GLint ok = 0;
        if (!v || !f) { gHost.swap = NULL; return 0; }
        gPresProg = glCreateProgram();
        glAttachShader(gPresProg, v);
        glAttachShader(gPresProg, f);
        glLinkProgram(gPresProg);
        glGetProgramiv(gPresProg, GL_LINK_STATUS, &ok);
        glDeleteShader(v);
        glDeleteShader(f);
        if (!ok) { fprintf(stderr, "gles: the presentation shader did not link\n"); gHost.swap = NULL; return 0; }
        gPresLocScale = glGetUniformLocation(gPresProg, "uScale");
        gPresLocRect = glGetUniformLocation(gPresProg, "uRect");
        gPresLocClamp = glGetUniformLocation(gPresProg, "uClamp");
        gPresLocRectF = glGetUniformLocation(gPresProg, "uRectF");
        gPresLocPicSize = glGetUniformLocation(gPresProg, "uPicSize");
        gPresLocOutScale = glGetUniformLocation(gPresProg, "uOutScale");
        gPresLocPic = glGetUniformLocation(gPresProg, "uPic");
        glGenTextures(1, &gPresTex);
        glBindTexture(GL_TEXTURE_2D, gPresTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return 1;
}

static volatile int gStretch;
void gles_set_stretch(int on) { gStretch = on != 0; }

/* How the picture is stretched to the window: 0 smooth (plain bilinear, the default since test 22), 1 sharp (a one-window-pixel blend band between texels), 2 firm
 * (a band two window pixels wide: between the two). Test 21 had "sharp" only and the trainer-name text looked worse on the Switch, so the player can now cycle them. */
static volatile int gFilterMode;
int gles_cycle_filter(void) { gFilterMode = (gFilterMode + 1) % 3; return gFilterMode; }
int gles_filter(void) { return gFilterMode; }
void gles_set_filter(int m) { gFilterMode = m < 0 ? 0 : m > 2 ? 2 : m; }

/* Draws `tex` (the part `rect` of it, a picture of pw x ph pixels) into the window, letterboxed (or filling it: gles_set_stretch), and swaps. */
static int gPresFill;                                    /* 1 while the start-up picture is drawn: it is made at the window's own shape and fills it */
static void present_draw(GLuint tex, int pw, int ph, const float rect[4], const float clampv[4])
{
    int ww = 0, wh = 0;
    float sx = 1.0f, sy = 1.0f, aspect = (float)pw / (float)ph;
    uint64_t t0;
    if (gHost.window_size) gHost.window_size(gHost.user, &ww, &wh);
    if (ww <= 0 || wh <= 0) { ww = 1280; wh = 720; }
    if (gStretch || gPresFill) { sx = 1.0f; sy = 1.0f; }
    else if ((float)ww / (float)wh > aspect) sx = ((float)wh * aspect) / (float)ww; else sy = ((float)ww / aspect) / (float)wh;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(gPresProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform2f(gPresLocScale, sx, sy);
    glUniform4f(gPresLocRect, rect[0], rect[1], rect[2], rect[3]);
    glUniform4f(gPresLocClamp, clampv[0], clampv[1], clampv[2], clampv[3]);
    {   /* the sharp-bilinear parameters: the window pixels the picture covers, per texel */
        float ox = (float)ww * sx / (float)pw, oy = (float)wh * sy / (float)ph;
        glUniform4f(gPresLocRectF, rect[0], rect[1], rect[2], rect[3]);
        glUniform2f(gPresLocPicSize, (float)pw, (float)ph);
        int fm = gFilterMode;
        if (fm == 0) { ox = 1.0f; oy = 1.0f; }                  /* a one-to-one blend band is plain bilinear */
        else if (fm == 2) { ox *= 0.5f; oy *= 0.5f; }
        glUniform2f(gPresLocOutScale, ox < 1.0f ? 1.0f : ox, oy < 1.0f ? 1.0f : oy);
    }
    glUniform1i(gPresLocPic, 0);
    glBindVertexArray(gVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_check("present");
    {   /* development: KYUJO_GPU_SHOT=dir writes every 30th window picture as a PPM (what the player would see) */
        static int shot = -1, nshot;
        static const char *dir;
        if (shot < 0) { dir = getenv("KYUJO_GPU_SHOT"); shot = dir != NULL; }
        if (shot && (nshot++ % 30) == 0) {
            uint8_t *px = (uint8_t *)malloc((size_t)ww * (size_t)wh * 4u);
            if (px) {
                char fn[512];
                FILE *f;
                int x, y;
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                snprintf(fn, sizeof fn, "%s/window_%05d.ppm", dir, nshot / 30);
                f = fopen(fn, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", ww, wh);
                    for (y = wh - 1; y >= 0; y--) for (x = 0; x < ww; x++) fwrite(px + ((size_t)y * (size_t)ww + (size_t)x) * 4u, 1, 3, f);
                    fclose(f);
                }
                free(px);
            }
        }
    }
    phase("swap");
    t0 = mono_ns();
    gHost.swap(gHost.user);
    gBusyNs[T_SWAP] += mono_ns() - t0;
}

static void do_present(Cmd *c)
{
    static const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (!present_ready()) return;
    glActiveTexture(GL_TEXTURE0);
    phase("present upload");
    glBindTexture(GL_TEXTURE_2D, gPresTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (c->w != gPresW || c->h != gPresH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, c->w, c->h, 0, GL_RGB, GL_UNSIGNED_BYTE, c->data);
        gPresW = c->w; gPresH = c->h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c->w, c->h, GL_RGB, GL_UNSIGNED_BYTE, c->data);
    }
    present_draw(gPresTex, c->w, c->h, whole, whole);
    gPresentsPeriod++;
}

/* The picture the VI shows, taken from the colour surface that holds it (the same choice as do_read) and drawn straight into the window. */
static void do_show(Cmd *c)
{
    int i;
    Surf *best = NULL;
    if (gTrace > 0) fprintf(stderr, "gpu-frame origin=%06X (shown)\n", c->addr);
    for (i = 0; i < gSurfN; i++) {
        Surf *s = &gSurf[i];
        uint32_t bytes = s->width * (uint32_t)s->h * (s->siz == 3 ? 4u : 2u);
        if (s->width != c->stride_px || (s->siz == 3 ? 4u : 2u) != c->bpp) continue;
        if (c->addr < s->addr || c->addr >= s->addr + bytes) continue;
        if ((c->addr - s->addr) % c->bpp) continue;
        if (!best || s->serial > best->serial) best = s;
    }
    if (best && present_ready()) {
        uint32_t off = (c->addr - best->addr) / c->bpp;
        float sw = (float)best->width, sh = (float)best->h;
        int x0 = (int)(off % best->width), y0 = (int)(off / best->width) + c->row_offset;
        float rect[4], clampv[4];
        rect[0] = (float)x0 / sw; rect[1] = (float)y0 / sh; rect[2] = (float)c->w / sw; rect[3] = (float)c->h / sh;
        clampv[0] = ((float)x0 + 0.5f) / sw; clampv[1] = ((float)(y0 < 0 ? 0 : y0) + 0.5f) / sh;
        clampv[2] = ((float)(x0 + c->w) - 0.5f) / sw; clampv[3] = ((float)(y0 + c->h) - 0.5f) / sh;
        if (clampv[3] > 1.0f - 0.5f / sh) clampv[3] = 1.0f - 0.5f / sh;
        glBindTexture(GL_TEXTURE_2D, best->tex);                /* a surface is only sampled here, so its filter is free to change */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        present_draw(best->tex, c->w, c->h, rect, clampv);
        gShowsPeriod++;
        gShowTotal++;
    }
    pthread_mutex_lock(&gQm);
    gShowDone++;
    pthread_mutex_unlock(&gQm);
}

void gles_present(const unsigned char *rgb, unsigned w, unsigned h)
{
    Cmd *c;
    size_t n = (size_t)w * h * 3u;
    if (!gRunning || !gHost.swap || !w || !h) return;
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_PRESENT;
    c->w = (int)w; c->h = (int)h;
    c->data = malloc(n);
    if (!c->data) { free(c); return; }
    memcpy(c->data, rgb, n);
    push(c);
}


/* Reports a render thread that sits in one place for long: on a console the log file is all there is to see what a stall was. */
static volatile int gWdStop;
static void *watchdog_main(void *arg)
{
    const char *last = NULL;
    uint64_t lastSince = 0, lastLog = 0;
    int reports = 0;
    (void)arg;
    while (!gWdStop) {
        struct timespec t = { 0, 250000000 };
        const char *ph;
        uint64_t since, now;
        int qn;
        nanosleep(&t, NULL);
        ph = (const char *)gPhase; since = gPhaseSince; now = now_ms();
        if (!ph || !strcmp(ph, "idle") || now - since < 1000) { last = NULL; continue; }
        if (ph == last && since == lastSince && now - lastLog < 3000) continue;
        pthread_mutex_lock(&gQm); qn = gQn; pthread_mutex_unlock(&gQm);
        if (reports++ < 40) fprintf(stderr, "gles: WATCHDOG render thread has been in '%s' for %u ms (%d commands queued)\n", ph, (unsigned)(now - since), qn);
        last = ph; lastSince = since; lastLog = now;
    }
    return NULL;
}

/* ---- start-up picture ---------------------------------------------------------------------------------------------------------------------------------
 * Before the game's first picture (the shader warm-up, loading the ROM and the cartridges: several seconds on the Switch) the window would stay black, which
 * looks like a crash. The render thread draws a "please wait" picture with a progress bar instead: made on the CPU (a built-in 5x7 font, no game data) into a
 * 640 x 480 RGB buffer and shown like any picture. Refreshed after every shader built; the first game picture replaces it. */
static const struct { char c; unsigned char row[7]; } kFont[] = {
    {' ',{0,0,0,0,0,0,0}}, {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}}, {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}}, {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}}, {'I',{14,4,4,4,4,4,14}},
    {'J',{7,2,2,2,2,18,12}}, {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}}, {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}}, {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}}, {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}}, {'W',{17,17,17,21,21,27,17}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}}, {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}}, {'2',{14,17,1,2,4,8,31}},
    {'3',{30,1,1,14,1,1,30}}, {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,30,1,1,17,14}}, {'6',{6,8,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,2,12}}, {'.',{0,0,0,0,0,12,12}}, {':',{0,12,12,0,12,12,0}}, {'/',{1,2,2,4,8,8,16}},
    {'-',{0,0,0,31,0,0,0}}, {'!',{4,4,4,4,4,0,4}},
};
#define SPL_BASE_W 640                                   /* the layout below is drawn on a 640 x 480 area, centred in a canvas that has the window's shape */
#define SPL_BASE_H 480
static int SPL_W = SPL_BASE_W, SPL_H = SPL_BASE_H;      /* the canvas (never smaller than the layout area) */
static unsigned char *gSplBuf;
static unsigned char *gBackTile;                         /* the game's own menu-wallpaper tile (RGB), given by the game at start-up from the player's ROM; memory only, never stored */
static int gBackW, gBackH;
static char gSplStatus[48] = "LOADING THE GAME";         /* what the start-up picture last showed, for redraws */
static float gSplProgress = -1.0f;
static int gSplashOn = 1;                                /* cleared by the first real picture */
static char gNotice[3][64];                                /* gles_show_notice: the start-up picture shows a message instead of the progress (written under gQm, read by the render thread) */
static int gNoticeOn;

static void spl_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + h && j < SPL_H; j++)
        for (i = x < 0 ? 0 : x; i < x + w && i < SPL_W; i++) { unsigned char *p = gSplBuf + ((size_t)j * SPL_W + (size_t)i) * 3; p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; }
}
static int spl_text_w(const char *t, int sc) { return (int)strlen(t) * 6 * sc - sc; }
static void spl_text(const char *t, int cx, int y, int sc, unsigned r, unsigned g, unsigned b)
{
    int x = cx - spl_text_w(t, sc) / 2;
    for (; *t; t++, x += 6 * sc) {
        size_t k;
        for (k = 0; k < sizeof kFont / sizeof kFont[0]; k++) {
            if (kFont[k].c == *t) {
                int rr, cc;
                for (rr = 0; rr < 7; rr++) for (cc = 0; cc < 5; cc++) if (kFont[k].row[rr] & (16 >> cc)) spl_rect(x + cc * sc, y + rr * sc, sc, sc, r, g, b);
                break;
            }
        }
    }
}
/* Rounded translucent panel, drawn the way the game's own menu boxes are (corner radius 16, the colour's alpha is the box's opacity). */
static void spl_panel(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b, unsigned a)
{
    const int rad = 16;
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + h && j < SPL_H; j++) {
        for (i = x < 0 ? 0 : x; i < x + w && i < SPL_W; i++) {
            float cov = 1.0f;
            int cx = i - x < rad ? rad : (i - x >= w - rad ? w - rad - 1 : -1);
            int cy = j - y < rad ? rad : (j - y >= h - rad ? h - rad - 1 : -1);
            if (cx >= 0 && cy >= 0) {
                float dx = (float)(i - x) + 0.5f - (float)(i - x < rad ? rad : w - rad), dy = (float)(j - y) + 0.5f - (float)(j - y < rad ? rad : h - rad);
                float d = sqrtf(dx * dx + dy * dy);
                cov = (float)rad - d + 0.5f;
                cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
            }
            if (cov > 0.0f) {
                unsigned char *p = gSplBuf + ((size_t)j * SPL_W + (size_t)i) * 3;
                float k = cov * (float)a / 255.0f;
                p[0] = (unsigned char)((float)p[0] + ((float)r - (float)p[0]) * k);
                p[1] = (unsigned char)((float)p[1] + ((float)g - (float)p[1]) * k);
                p[2] = (unsigned char)((float)p[2] + ((float)b - (float)p[2]) * k);
            }
        }
    }
}
/* The start-up picture follows the game's own "Game Pak Check" screens: gold wallpaper, translucent blue rounded boxes, white text, yellow caption,
 * the same layout as the Osaka-NX start-up screen. The colours are the game's menu colours (plain numbers, nothing copied from the ROM; the
 * wallpaper's fine pattern is left out, only its middle shade is used). progress < 0: no bar. Render thread only; a no-op without a window. */
static void splash_draw(const char *status, float progress)
{
    Cmd c;
    int ww = 0, wh = 0, W = SPL_BASE_W, H = SPL_BASE_H, ox, oy;
    if (!gSplashOn || !gHost.swap || !present_ready()) return;
    if (gHost.window_size) gHost.window_size(gHost.user, &ww, &wh);
    if (ww > 0 && wh > 0) {                                            /* a canvas with the window's shape: the picture fills the window, no black bars */
        if ((long)ww * SPL_BASE_H >= (long)wh * SPL_BASE_W) W = (int)(((long)SPL_BASE_H * ww + wh / 2) / wh);
        else H = (int)(((long)SPL_BASE_W * wh + ww / 2) / ww);
        if (W < SPL_BASE_W) W = SPL_BASE_W;
        if (H < SPL_BASE_H) H = SPL_BASE_H;
    }
    if (!gSplBuf || W != SPL_W || H != SPL_H) {
        free(gSplBuf);
        SPL_W = W; SPL_H = H;
        if (!(gSplBuf = (unsigned char *)malloc((size_t)SPL_W * SPL_H * 3))) return;
    }
    ox = (SPL_W - SPL_BASE_W) / 2; oy = (SPL_H - SPL_BASE_H) / 2;
    snprintf(gSplStatus, sizeof gSplStatus, "%s", status);
    gSplProgress = progress;
    if (gBackTile && gBackW > 0 && gBackH > 0) {                       /* the same wallpaper as the game's menu pages (the Game Pak Check pattern), tiled over the whole canvas */
        int x, y;
        for (y = 0; y < SPL_H; y++) {
            const unsigned char *row = gBackTile + (size_t)(y % gBackH) * (size_t)gBackW * 3u;
            unsigned char *o = gSplBuf + (size_t)y * (size_t)SPL_W * 3u;
            for (x = 0; x < SPL_W; x++) { const unsigned char *t = row + (size_t)(x % gBackW) * 3u; o[x * 3] = t[0]; o[x * 3 + 1] = t[1]; o[x * 3 + 2] = t[2]; }
        }
    } else
    spl_rect(0, 0, SPL_W, SPL_H, 150, 140, 70);                        /* wallpaper without the game's tile: the middle of its two menu-background colours */
    spl_panel(ox + (SPL_BASE_W - 200) / 2, oy + 32, 200, 73, 30, 30, 130, 170);   /* heading box: two 21 px lines 3 px apart, 14 px above and below */
    spl_text("POKEMON", ox + SPL_BASE_W / 2, oy + 46, 3, 255, 255, 255);
    spl_text("STADIUM", ox + SPL_BASE_W / 2, oy + 70, 3, 255, 255, 255);
    spl_panel(ox + 52, oy + 130, 533, 210, 30, 30, 130, 170);          /* text box */
    if (gNoticeOn) {                                                   /* a message (gles_show_notice): a heading, up to three lines and a caption */
        int k;
        spl_text(gNotice[0], ox + SPL_BASE_W / 2, oy + 148, 3, 255, 255, 255);
        for (k = 1; k < 3; k++) spl_text(gNotice[k], ox + SPL_BASE_W / 2, oy + 200 + (k - 1) * 40, 2, 255, 255, 255);
        spl_text("RETURNING TO THE LAUNCHER", ox + SPL_BASE_W / 2, oy + 310, 2, 255, 220, 90);
        progress = -1.0f;
    } else {
    spl_text("PLEASE WAIT", ox + SPL_BASE_W / 2, oy + 148, 3, 255, 255, 255);
    spl_text(status, ox + SPL_BASE_W / 2, oy + 192, 3, 255, 255, 255);
    }
    if (progress >= 0.0f) {
        int fw;
        if (progress > 1.0f) progress = 1.0f;
        fw = (int)(453.0f * progress);
        spl_panel(ox + 89, oy + 267, 459, 32, 255, 255, 255, 255);
        spl_panel(ox + 92, oy + 270, 453, 26, 20, 20, 70, 255);
        if (fw >= 32) spl_panel(ox + 92, oy + 270, fw, 26, 0, 96, 40, 255);
        else if (fw > 0) spl_rect(ox + 94, oy + 272, fw, 22, 0, 96, 40);
    }
    if (!gNoticeOn) spl_text("THE GAME STARTS SHORTLY", ox + SPL_BASE_W / 2, oy + 310, 2, 255, 220, 90);
    {   /* development: KYUJO_SPLASH_SHOT=dir writes every start-up picture (the first 40) as a PPM */
        static const char *dir; static int tried, n;
        if (!tried) { tried = 1; dir = getenv("KYUJO_SPLASH_SHOT"); }
        if (dir && n < 40) {
            char fn[512];
            FILE *f;
            snprintf(fn, sizeof fn, "%s/splash_%02d.ppm", dir, n++);
            f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", SPL_W, SPL_H); fwrite(gSplBuf, 1, (size_t)SPL_W * SPL_H * 3u, f); fclose(f); }
        }
    }
    memset(&c, 0, sizeof c);
    c.op = OP_PRESENT; c.w = SPL_W; c.h = SPL_H; c.data = gSplBuf;
    gPresFill = 1;
    do_present(&c);
    gPresFill = 0;
}

static pthread_t gWdThread;
#define WARM_FIRST 20                                  /* programs built before the game's first picture (the start-up ones come first in the saved list); the rest are built when idle */
static int gWarmNext;                                  /* the next saved program to build while idle */

/* The shader warm-up behind the start-up picture (see "warm-up" above): build the first saved programs with a progress bar, the rest while idle. */
static void warm_up(void)
{
    if (gSplashForce) { gSplashForce = 0; gSplashOn = 1; }       /* the first-start page drew its own pictures: the start-up picture comes back */
    splash_draw(gKnownN > 0 ? "PREPARING GRAPHICS" : "LOADING THE GAME", gKnownN > 0 ? 0.0f : -1.0f);
    if (gKnownN > 0 && !getenv("KYUJO_NO_WARMUP")) {
        int i, n = gKnownN;
        uint64_t t0 = now_ms();
        phase("shader warm-up");
        if (n > WARM_FIRST) n = WARM_FIRST;                          /* the rest is built while the render thread has nothing to do (below) */
        for (i = 0; i < n; i++) {
            char line[48];
            program_get(&gKnown[i]);
            snprintf(line, sizeof line, "PREPARING GRAPHICS %d/%d", i + 1, n);
            splash_draw(line, (float)(i + 1) / (float)n);
        }
        splash_draw("STARTING THE GAME", 1.0f);
        gWarmNext = n;
        fprintf(stderr, "gles: warm-up: %d of %d programs built in %u ms, the rest while idle\n", n, gKnownN, (unsigned)(now_ms() - t0));
    } else if (gKnownN > 0) {
        gWarmNext = gKnownN;
    }
}

/* The game says its first-start page is over (or was not needed): the shader warm-up may begin, with the start-up picture. */
void gles_graphics_go(void)
{
    pthread_mutex_lock(&gQm);
    if (!gWarmGo) gWarmGo = 1;
    else if (gWarmDone) gSplashDirty = 1;                       /* a restart of the game: the start-up picture again while it boots */
    gSplashForce = 1;
    pthread_cond_signal(&gQc);
    pthread_mutex_unlock(&gQm);
}

/* The wallpaper tile of the game's menu pages (RGB, w x h; copied). The start-up picture uses it as soon as it arrives. */
void gles_set_backdrop(const unsigned char *rgb, unsigned w, unsigned h)
{
    unsigned char *t;
    if (!rgb || !w || !h || w > 1024 || h > 1024) return;
    t = (unsigned char *)malloc((size_t)w * h * 3u);
    if (!t) return;
    memcpy(t, rgb, (size_t)w * h * 3u);
    pthread_mutex_lock(&gQm);
    free(gBackTile);
    gBackTile = t; gBackW = (int)w; gBackH = (int)h;
    gSplashDirty = 1;
    pthread_cond_signal(&gQc);
    pthread_mutex_unlock(&gQm);
}

/* A message on the start-up picture (the game has stopped and the program is about to go back to the launcher): heading and two lines, upper case letters,
 * digits and . : / - ! only (the built-in font). Thread-safe; the render thread draws it. */
void gles_show_notice(const char *heading, const char *line1, const char *line2)
{
    pthread_mutex_lock(&gQm);
    snprintf(gNotice[0], sizeof gNotice[0], "%s", heading ? heading : "");
    snprintf(gNotice[1], sizeof gNotice[1], "%s", line1 ? line1 : "");
    snprintf(gNotice[2], sizeof gNotice[2], "%s", line2 ? line2 : "");
    gNoticeOn = 1;
    gSplashForce = 1;
    gSplashDirty = 1;
    pthread_cond_signal(&gQc);
    pthread_mutex_unlock(&gQm);
}

static void *render_main(void *arg)
{
    (void)arg;
    if (gHost.make_current(gHost.user) != 0) { gStartFailed = 1; gStarted = 1; return NULL; }
    {
        const char *ver = (const char *)glGetString(GL_VERSION), *ren = (const char *)glGetString(GL_RENDERER);
        fprintf(stderr, "gles: %s / %s\n", ver ? ver : "?", ren ? ren : "?");
        {   /* for the shader start-up delays: can compiled programs be saved and loaded again? */
            GLint nfmt = -1;
            const char *ext = (const char *)glGetString(GL_EXTENSIONS);
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nfmt);
            fprintf(stderr, "gles: program binary formats: %d; parallel shader compile: %s; shared contexts: see below\n", (int)nfmt,
                    ext && strstr(ext, "KHR_parallel_shader_compile") ? "yes" : "no");
        }
    }
    if (gHost.cache_dir && !getenv("KYUJO_NO_WARMUP")) snprintf(gKeyPath, sizeof gKeyPath, "%s/shader-keys.bin", gHost.cache_dir);
    keys_load();
    glGenVertexArrays(1, &gVao);
    glGenBuffers(1, &gVbo);
    glGenFramebuffers(1, &gScratchFbo);
    glBindVertexArray(gVao);
    glBindBuffer(GL_ARRAY_BUFFER, gVbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVert), (const void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GpuVert), (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVert), (const void *)(7 * sizeof(float)));
    gStarted = 1;
    splash_draw("LOADING THE GAME", -1.0f);
    for (;;) {
        Cmd *c;
        uint64_t t_op;
        int skipped;
        pthread_mutex_lock(&gQm);
        while (!gHead) {
            if (gWarmGo && !gWarmDone) {                         /* the game asked for the graphics set-up (after its first-start page, or at its first drawing) */
                gWarmDone = 1;
                pthread_mutex_unlock(&gQm);
                warm_up();
                pthread_mutex_lock(&gQm);
                continue;
            }
            if (gSplashDirty) {
                gSplashDirty = 0;
                pthread_mutex_unlock(&gQm);
                if (gSplashForce) { gSplashForce = 0; gSplashOn = 1; }
                if (gSplashOn) splash_draw(gSplStatus, gSplProgress);
                pthread_mutex_lock(&gQm);
                continue;
            }
            if (gWarmDone && gWarmNext < gKnownN) {                           /* idle: build one more of the saved programs (it may delay a command that arrives meanwhile by one build) */
                int w = gWarmNext++;
                pthread_mutex_unlock(&gQm);
                program_get(&gKnown[w]);
                if (gWarmNext >= gKnownN) fprintf(stderr, "gles: the remaining saved programs are built\n");
                pthread_mutex_lock(&gQm);
                continue;
            }
            pthread_cond_wait(&gQc, &gQm);
        }
        c = gHead;
        gHead = c->next;
        if (!gHead) gTail = NULL;
        gQn--;
        if (c == gLastShow) gLastShow = NULL;
        skipped = c->op == OP_SHOW && c->skip;
        pthread_cond_broadcast(&gQd);
        pthread_mutex_unlock(&gQm);
        {
            static const char *const names[] = { "?", "draw", "fill", "texture", "delete textures", "sync", "read", "flush", "reset", "quit", "present", "show", "invalidate" };
            phase(c->op > 0 && c->op < (int)(sizeof names / sizeof names[0]) ? names[c->op] : "op");
        }
        t_op = mono_ns();
        switch (c->op) {
        case OP_DRAW: do_draw(c); break;
        case OP_FILL: do_fill(c); break;
        case OP_TEXTURE: do_texture(c); break;
        case OP_DELETE_ALL: do_delete_all(); break;
        case OP_SYNC: do_sync(c); break;
        case OP_READ: do_read(c); break;
        case OP_FLUSH: glFinish(); break;
        case OP_RESET: do_reset(); break;
        case OP_PRESENT: gSplashOn = 0; do_present(c); break;
        case OP_INVALIDATE: do_invalidate(c); break;
        case OP_SHOW: if (skipped) { pthread_mutex_lock(&gQm); gShowDropped++; pthread_mutex_unlock(&gQm); } else { gSplashOn = 0; do_show(c); } break;
        case OP_QUIT: gHost.release(gHost.user); free(c); return NULL;
        default: break;
        }
        {
            uint64_t d = mono_ns() - t_op;
            switch (c->op) {
            case OP_DRAW: gBusyNs[T_DRAW] += d; break;
            case OP_FILL: gBusyNs[T_FILL] += d; break;
            case OP_TEXTURE: gBusyNs[T_TEXTURE] += d; break;
            case OP_SYNC: case OP_READ: gBusyNs[T_SYNC_READ] += d; break;
            case OP_SHOW: gBusyNs[T_SHOW] += d; break;
            case OP_PRESENT: gBusyNs[T_PRESENT] += d; break;
            default: gBusyNs[T_OTHER] += d; break;
            }
        }
        phase("idle");
        if (c->op == OP_SYNC || c->op == OP_READ || c->op == OP_FLUSH || c->op == OP_RESET) {
            pthread_mutex_lock(&gQm);
            c->done = 1;
            pthread_cond_broadcast(&gQd);
            pthread_mutex_unlock(&gQm);              /* the producer owns and frees these */
        } else {
            free(c->data);
            free(c);
        }
    }
}

int gles_start(const GlesHost *host)
{
    GpuBackend b;
    gHost = *host;
    phase("start");
    if (pthread_create(&gThread, NULL, render_main, NULL) != 0) return -1;
    if (pthread_create(&gWdThread, NULL, watchdog_main, NULL) != 0) gWdThread = 0;
    while (!gStarted) { struct timespec t = { 0, 1000000 }; nanosleep(&t, NULL); }
    if (gStartFailed) { pthread_join(gThread, NULL); return -1; }
    gRunning = 1;
    memset(&b, 0, sizeof b);
    b.draw = gles_draw; b.fill = gles_fill; b.texture = gles_texture; b.sync_to_ram = gles_sync_to_ram; b.read_picture = gles_read_picture; b.show_picture = gles_show_picture; b.invalidate = gles_invalidate;
    b.flush = gles_flush; b.reset = gles_reset; b.tex_epoch = gles_tex_epoch;
    gpu_install(&b);
    return 0;
}

/* The game's instance is about to be thrown away (restart inside the program): finish every queued command, so nothing reads the old memory, and drop the caches. */
void gles_quiesce(void)
{
    if (!gRunning) return;
    gles_flush();
    gles_reset();
}

void gles_stop(void)
{
    Cmd *c;
    if (!gRunning) return;
    pthread_mutex_lock(&gProd);
    flush_pending();
    c = (Cmd *)calloc(1, sizeof *c);
    c->op = OP_QUIT;
    push(c);
    pthread_mutex_unlock(&gProd);
    pthread_join(gThread, NULL);
    gWdStop = 1;
    if (gWdThread) pthread_join(gWdThread, NULL);
    gRunning = 0;
}

void gles_stats(GlesStats *out) { *out = gStats; }

uint64_t gles_pictures_shown(void) { uint64_t n; pthread_mutex_lock(&gQm); n = gShowDone; pthread_mutex_unlock(&gQm); return n; }

/* Where the render thread's time went since the last call (milliseconds), how many pictures it showed, and how long the game's thread waited for it. */
void gles_period_stats(char *buf, size_t n)
{
    uint64_t b[T_N], dropped, shows, pres;
    int k;
    for (k = 0; k < T_N; k++) { b[k] = gBusyNs[k]; gBusyNs[k] = 0; }
    pthread_mutex_lock(&gQm); dropped = gShowDropped; gShowDropped = 0; pthread_mutex_unlock(&gQm);
    shows = gShowsPeriod; gShowsPeriod = 0;
    pres = gPresentsPeriod; gPresentsPeriod = 0;
    snprintf(buf, n, "gles: render thread ms: draw %u fill %u tex %u sync/read %u show %u present %u swap %u other %u; shown %u, uploaded %u, dropped %u (GPU behind)",
             (unsigned)(b[T_DRAW] / 1000000u), (unsigned)(b[T_FILL] / 1000000u), (unsigned)(b[T_TEXTURE] / 1000000u), (unsigned)(b[T_SYNC_READ] / 1000000u),
             (unsigned)(b[T_SHOW] / 1000000u), (unsigned)(b[T_PRESENT] / 1000000u), (unsigned)(b[T_SWAP] / 1000000u), (unsigned)(b[T_OTHER] / 1000000u),
             (unsigned)shows, (unsigned)pres, (unsigned)dropped);
}

void gles_describe(char *buf, size_t n)
{
    int qn;
    pthread_mutex_lock(&gQm); qn = gQn; pthread_mutex_unlock(&gQm);
    snprintf(buf, n, "render thread: '%s' for %u ms, %d commands queued, %d shaders", (const char *)gPhase, (unsigned)(now_ms() - gPhaseSince), qn, gProgN);
}
