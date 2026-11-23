/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * f3dex2.c - what the RSP's graphics microcode (F3DEX2) does with a display list, done directly in C. HOST-side.
 *
 * The console's RSP runs a small program (the "microcode") that reads the game's display list, transforms vertices with the current
 * matrices, lights them, clips and culls triangles and hands the RDP finished triangles and RDP commands. This file does the same work with
 * floating-point numbers. The game builds its display lists with the macros of lib/ultralib/include/PR/gbi.h compiled for F3DEX2
 * numbering (F3DEX_GBI_2); the encodings below were read from that header, and tests/native/render/ checks that this file decodes what
 * the header macros encode.
 *
 * Which parts are documented and which come from knowledge (listed in docs/rdp-notes.md as unverified):
 *   documented (gbi.h and the Nintendo manual pages): command encodings, structures (Vtx, Vp, Light, Mtx), fog formula "alpha =
 *     eye-space z * fm + fo, clamped 0..255, z in -1..1", light numbering (n directional lights, then the ambient light), segment addressing.
 *   from knowledge: the matrix/vertex arithmetic (row vectors, new matrix on the left), viewport transform with flipped y, the lighting
 *     equation's overall shape (ambient + sum of max(0, normal . direction) * colour; the direction is transformed by the modelview matrix and
 *     NORMALISED, which IS documented - Nintendo manual 11.7 - but the exact fixed-point scaling, about 1%, is not checked), the clip-plane handling, which
 *     vertex gives the colour of a flat-shaded triangle, and texture-coordinate generation.
 *   not done: line primitives, texture-coordinate generation in "linear" mode, G_LOAD_UCODE, G_DMA_IO, point lights (this ucode has none).
 */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "rdp.h"
#include "gfx.h"

/* F3DEX2 command numbers (gbi.h with F3DEX_GBI_2). */
enum {
    G_NOOP = 0x00, G_VTX = 0x01, G_MODIFYVTX = 0x02, G_CULLDL = 0x03, G_BRANCH_Z = 0x04, G_TRI1 = 0x05, G_TRI2 = 0x06, G_QUAD = 0x07,
    G_LINE3D = 0x08, G_TEXTURE = 0xD7, G_POPMTX = 0xD8, G_GEOMETRYMODE = 0xD9, G_MTX = 0xDA, G_MOVEWORD = 0xDB, G_MOVEMEM = 0xDC,
    G_LOAD_UCODE = 0xDD, G_DL = 0xDE, G_ENDDL = 0xDF, G_SPNOOP = 0xE0, G_RDPHALF_1 = 0xE1, G_SETOTHERMODE_L = 0xE2, G_SETOTHERMODE_H = 0xE3,
    G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_RDPSETOTHERMODE = 0xEF, G_SETCIMG = 0xFF, G_SETZIMG = 0xFE, G_SETTIMG = 0xFD,
    G_RDPHALF_2 = 0xF1, G_DMA_IO = 0xD6
};
/* G_MOVEMEM / G_MOVEWORD indices (F3DEX2). */
enum { MV_VIEWPORT = 8, MV_LIGHT = 10, MV_MATRIX = 14, MW_NUMLIGHT = 2, MW_CLIP = 4, MW_SEGMENT = 6, MW_FOG = 8, MW_LIGHTCOL = 10, MW_FORCEMTX = 12,
       MW_PERSPNORM = 14 };
/* Geometry mode bits (F3DEX2). */
enum {
    GM_ZBUFFER = 0x00000001, GM_SHADE = 0x00000004, GM_CULL_FRONT = 0x00000200, GM_CULL_BACK = 0x00000400, GM_FOG = 0x00010000,
    GM_LIGHTING = 0x00020000, GM_TEXTURE_GEN = 0x00040000, GM_TEXTURE_GEN_LINEAR = 0x00080000, GM_SHADING_SMOOTH = 0x00200000
};
/* G_MTX parameter bits after undoing the `^ G_MTX_PUSH` the macro applies (gbi.h). */
enum { MTX_PROJECTION = 0x04, MTX_LOAD = 0x02, MTX_PUSH = 0x01 };

#define MAX_MV_STACK 18          /* the microcode keeps ten; more is a game error, but a roomier stack makes the failure visible instead of fatal */
#define VTX_CACHE 64

typedef struct Vertex {
    float cx, cy, cz, cw;        /* clip coordinates */
    unsigned clip;               /* outside-plane bits, see CLIP_* */
    RdpVert out;                 /* screen vertex (valid when cw > 0) */
    uint8_t has_screen;
} Vertex;
enum { CLIP_XN = 1, CLIP_XP = 2, CLIP_YN = 4, CLIP_YP = 8, CLIP_NEAR = 16, CLIP_FAR = 32 };

typedef struct Light { uint8_t col[3]; int8_t dir[3]; } Light;

static struct {
    uint32_t seg[16];
    float mv[MAX_MV_STACK][16];
    int mv_top;
    float proj[16], mp[16], forced[16];
    int mp_dirty, mp_forced;
    float vscale[3], vtrans[3];
    uint32_t geom, omh, oml;
    Light light[10];             /* slot 0 / 1 = look-at X / Y (direction only), 2.. = lights; the macros address them as slot = offset / 24 */
    int numlights;
    int fog_fm, fog_fo;
    int tex_s, tex_t, tex_tile, tex_level, tex_on;
    uint32_t half1;              /* last G_RDPHALF_1 word (used by G_BRANCH_Z and G_TEXRECT) */
    Vertex vtx[VTX_CACHE];
    uint32_t n_cmds, n_tris;
} S;

/* Unknown / unmodelled features are reported once. */
enum { FU_LINE, FU_TEXGEN_LINEAR, FU_UCODE, FU_BAD_ADDR, FU_UNKNOWN_OP, FU_MV_OVERFLOW, FU_COUNT };
static const char *const kFuName[FU_COUNT] = { "G_LINE3D", "linear texture-coordinate generation", "G_LOAD_UCODE", "bad display list address", "unknown display list command", "modelview stack overflow" };
static uint32_t sFuCount[FU_COUNT];
static void fu(int what, uint32_t detail)
{
    if (sFuCount[what]++ == 0) fprintf(stderr, "kyujo: display lists: not handled, first seen: %s (0x%X)\n", kFuName[what], detail);
}

/* Development trace (KYUJO_GFX_TRACE=<n>): prints the viewport, projection and the first <n> triangles of every task to stderr. */
static uint32_t sTaskNo;                 /* graphics tasks run so far */
static int trace_limit(void)
{
    static int v = -1, only = -1;
    if (v < 0) {
        const char *e = getenv("KYUJO_GFX_TRACE"), *t = getenv("KYUJO_GFX_TRACE_TASK");
        v = e ? atoi(e) : 0;
        only = t ? atoi(t) : -1;                 /* when set, only that task number is traced */
    }
    return (only < 0 || (uint32_t)only == sTaskNo) ? v : 0;
}

/* ---- memory (big-endian bytes at native addresses, see platform.h) -------------------------------------------------------------- */

static const uint8_t *mem(uint32_t phys, uint32_t len)
{
    if (!host_game_range_ok(phys, len)) { fu(FU_BAD_ADDR, phys); return 0; }       /* outside the program image: garbage pointer in the display list */
    return (const uint8_t *)(uintptr_t)phys;
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int be16s(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

/* A display list address: top nibble = segment number (0 = plain), low 24 bits = offset. Returns the physical address. */
static uint32_t resolve(uint32_t addr)
{
    return (S.seg[(addr >> 24) & 0xF] + (addr & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

/* ---- matrices -------------------------------------------------------------------------------------------------------------------- */

static void mat_ident(float *m) { int i; for (i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }

/* out = a * b for 4x4 row-major matrices used with row vectors (v' = v * M). `out` may not alias the inputs. */
static void mat_mul(float *out, const float *a, const float *b)
{
    int r, c, k;
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) {
        float sum = 0.0f;
        for (k = 0; k < 4; k++) sum += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = sum;
    }
}

/* Reads the console's fixed-point matrix (gbi.h Mtx): 16 signed 16-bit integer parts (row-major) then 16 unsigned 16-bit fractions. */
static int mat_load(float *m, uint32_t phys)
{
    const uint8_t *p = mem(phys, 64);
    int i;
    if (!p) return 0;
    for (i = 0; i < 16; i++) {
        m[i] = (float)be16s(p + i * 2) + (float)((p[32 + i * 2] << 8) | p[33 + i * 2]) / 65536.0f;
    }
    return 1;
}

static void mp_update(void)
{
    if (S.mp_dirty && !S.mp_forced) {
        mat_mul(S.mp, S.mv[S.mv_top], S.proj);
    }
    S.mp_dirty = 0;
}

/* ---- vertices -------------------------------------------------------------------------------------------------------------------- */

static float fclampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void vertex_screen(Vertex *v)
{
    v->has_screen = 0;
    if (v->cw > 1e-6f) {
        float inv = 1.0f / v->cw;
        v->out.x = S.vtrans[0] + v->cx * inv * S.vscale[0];
        v->out.y = S.vtrans[1] - v->cy * inv * S.vscale[1];       /* FROM KNOWLEDGE: the microcode flips y; checked against rendered frames */
        v->out.z = S.vtrans[2] + v->cz * inv * S.vscale[2];
        v->out.invw = inv;
        v->has_screen = 1;
    }
}

static void load_vertices(uint32_t addr, int n, int v0)
{
    const uint8_t *p = mem(resolve(addr), (uint32_t)n * 16u);
    int i;
    /*
     * Lighting set-up shared by all vertices of this command: ONE direction vector per light (and per look-at axis), in the vertex normals'
     * own (model) space, scaled to length 1.  FROM DOCUMENTATION, in two parts:
     *   - Nintendo's programming manual (chapter 11.7 "Vertex Lighting State"): "The Light direction is multiplied times the Modelview Matrix"
     *     and, because of the normalisation that follows, the light direction need not be normalised and a scale in the matrix may not push
     *     the product out of range (it quotes 1 < L*S < 23040) - i.e. the microcode normalises the transformed light direction.
     *   - the F3DEX3 porting guide (hackern64.github.io/F3DEX3/porting.html): "F3D through F3DEX2 normalize light directions in the
     *     microcode". Vertex normals are NOT renormalised; they are used as stored (127 = 1.0).
     * Earlier versions of this file rotated the NORMAL by the modelview matrix instead and never normalised. That is the same dot product, but
     * without the normalisation it is multiplied by the matrix scale - and this game draws the Pokemon with a 0.1 scale, so their directional light
     * came out at a tenth of its strength and only the ambient light showed: the "dark / shadowed Pokemon" the Windows tester reported.
     * Row vectors: normal' = normal * M, so normal' . L = normal . (M * L); coefficient i = sum over j of M[i][j] * L[j] (M = upper 3x3 of mv).
     * NOT checked: the exact fixed-point scaling the real microcode uses (it may differ by about 1%, e.g. 127/128).
     */
    float light_dir[9][3];                  /* [0], [1] = look-at X / Y (texture generation), [2 + k] = directional light k (at most 7) */
    if (S.geom & GM_LIGHTING) {
        const float *mv = S.mv[S.mv_top];
        int li;
        for (li = 0; li < 9; li++) {
            const Light *l = &S.light[li];
            float lx = (float)l->dir[0], ly = (float)l->dir[1], lz = (float)l->dir[2];
            float cx = mv[0] * lx + mv[1] * ly + mv[2] * lz;
            float cy = mv[4] * lx + mv[5] * ly + mv[6] * lz;
            float cz = mv[8] * lx + mv[9] * ly + mv[10] * lz;
            float len = sqrtf(cx * cx + cy * cy + cz * cz);
            float inv = (len > 1e-6f) ? 1.0f / len : 0.0f;     /* a zero direction lights nothing */
            light_dir[li][0] = cx * inv; light_dir[li][1] = cy * inv; light_dir[li][2] = cz * inv;
        }
    }
    if (!p) return;
    mp_update();
    if (trace_limit() >= 100 && S.n_tris < 40) {
        const float *m = S.mp, *mv = S.mv[S.mv_top], *pr = S.proj;
        fprintf(stderr, "gfx: vtx n=%d from 0x%X first=(%d,%d,%d) mv_top=%d\n  MV: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n  P:  %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                n, resolve(addr), be16s(p), be16s(p + 2), be16s(p + 4), S.mv_top,
                mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15],
                pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], pr[6], pr[7], pr[8], pr[9], pr[10], pr[11], pr[12], pr[13], pr[14], pr[15]);
        (void)m;
    }
    for (i = 0; i < n && v0 + i < VTX_CACHE; i++, p += 16) {
        Vertex *v = &S.vtx[v0 + i];
        float x = (float)be16s(p), y = (float)be16s(p + 2), z = (float)be16s(p + 4);
        int tcs = be16s(p + 8), tct = be16s(p + 10);
        float r = p[12], g = p[13], b = p[14], a = p[15];
        float sfp, tfp;

        v->cx = x * S.mp[0] + y * S.mp[4] + z * S.mp[8] + S.mp[12];
        v->cy = x * S.mp[1] + y * S.mp[5] + z * S.mp[9] + S.mp[13];
        v->cz = x * S.mp[2] + y * S.mp[6] + z * S.mp[10] + S.mp[14];
        v->cw = x * S.mp[3] + y * S.mp[7] + z * S.mp[11] + S.mp[15];
        v->clip = 0;
        if (v->cx < -v->cw) v->clip |= CLIP_XN;
        if (v->cx > v->cw) v->clip |= CLIP_XP;
        if (v->cy < -v->cw) v->clip |= CLIP_YN;
        if (v->cy > v->cw) v->clip |= CLIP_YP;
        if (v->cz < -v->cw) v->clip |= CLIP_NEAR;
        if (v->cz > v->cw) v->clip |= CLIP_FAR;

        sfp = (float)((tcs * S.tex_s) >> 16);           /* s10.5 texture coordinate times the gSPTexture scale (0xFFFF ~ 1.0) */
        tfp = (float)((tct * S.tex_t) >> 16);

        if (S.geom & GM_LIGHTING) {
            /* The "colour" bytes are a normal (signed, 127 = 1.0). Dot it with each light's direction (set up above) and sum the lights. */
            float nx = (float)(int8_t)p[12], ny = (float)(int8_t)p[13], nz = (float)(int8_t)p[14];
            const Light *amb = &S.light[2 + (S.numlights < 7 ? S.numlights : 7)];
            int li;
            r = amb->col[0]; g = amb->col[1]; b = amb->col[2];
            for (li = 0; li < S.numlights && li < 7; li++) {
                const Light *l = &S.light[2 + li];
                float d = (nx * light_dir[2 + li][0] + ny * light_dir[2 + li][1] + nz * light_dir[2 + li][2]) / 127.0f;
                if (d > 0.0f) { r += (float)l->col[0] * d; g += (float)l->col[1] * d; b += (float)l->col[2] * d; }
            }
            r = fclampf(r, 0, 255); g = fclampf(g, 0, 255); b = fclampf(b, 0, 255);
            if (i == 0) {                                  /* development: KYUJO_LIGHT_TRACE=<n> prints the light set-up of the first <n> lit vertex batches */
                static int budget = -1;
                if (budget < 0) { const char *e = getenv("KYUJO_LIGHT_TRACE"); budget = e ? atoi(e) : 0; }
                if (budget > 0) {
                    const float *mv = S.mv[S.mv_top];
                    int k;
                    budget--;
                    fprintf(stderr, "light: task %u n=%d lights=%d ambient %u,%u,%u mvscale %.3f normal %d,%d,%d -> shade %.0f,%.0f,%.0f;", (unsigned)sTaskNo, n, S.numlights, amb->col[0], amb->col[1], amb->col[2],
                            sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]), (int)(int8_t)p[12], (int)(int8_t)p[13], (int)(int8_t)p[14], r, g, b);
                    for (k = 0; k < S.numlights && k < 7; k++) fprintf(stderr, " L%d col %u,%u,%u dir %d,%d,%d;", k, S.light[2 + k].col[0], S.light[2 + k].col[1], S.light[2 + k].col[2], S.light[2 + k].dir[0], S.light[2 + k].dir[1], S.light[2 + k].dir[2]);
                    fprintf(stderr, "\n");
                }
            }
            if (S.geom & GM_TEXTURE_GEN) {
                /* FROM KNOWLEDGE: s,t = (normal . look-at vector, scaled to -1..1) mapped to 0..0x8000, times the texture scale. A scale of
                 * 0x07C0 then gives 31 texels, the usual setting for a 32-texel environment map. The look-at vectors are treated like the
                 * lights (transformed by the modelview matrix and normalised); that part is by analogy, not documented. */
                float us = (nx * light_dir[0][0] + ny * light_dir[0][1] + nz * light_dir[0][2]) / 127.0f;
                float ut = (nx * light_dir[1][0] + ny * light_dir[1][1] + nz * light_dir[1][2]) / 127.0f;
                if (S.geom & GM_TEXTURE_GEN_LINEAR) fu(FU_TEXGEN_LINEAR, 0);
                sfp = (float)((int)((us + 1.0f) * 16384.0f) * S.tex_s >> 16);
                tfp = (float)((int)((ut + 1.0f) * 16384.0f) * S.tex_t >> 16);
            }
        }
        if (S.geom & GM_FOG) {
            /* gbi.h: alpha(fog) = (eye-space z) * fm + fo, clamped to 0..255, eye-space z from -1 to 1 */
            float ez = (v->cw > 1e-6f) ? v->cz / v->cw : 0.0f;
            a = fclampf(ez * (float)S.fog_fm + (float)S.fog_fo, 0, 255);
        }
        v->out.r = r; v->out.g = g; v->out.b = b; v->out.a = a;
        v->out.s = sfp / 32.0f;                          /* to texels */
        v->out.t = tfp / 32.0f;
        vertex_screen(v);
    }
}

/* ---- triangles ------------------------------------------------------------------------------------------------------------------- */

/* A clip-space polygon vertex with all attributes, used while clipping against the near plane. */
typedef struct CV { float cx, cy, cz, cw, r, g, b, a, s, t; } CV;

static void cv_from(CV *c, const Vertex *v)
{
    c->cx = v->cx; c->cy = v->cy; c->cz = v->cz; c->cw = v->cw;
    c->r = v->out.r; c->g = v->out.g; c->b = v->out.b; c->a = v->out.a; c->s = v->out.s; c->t = v->out.t;
}
static void cv_lerp(CV *o, const CV *a, const CV *b, float t)
{
    o->cx = a->cx + (b->cx - a->cx) * t; o->cy = a->cy + (b->cy - a->cy) * t; o->cz = a->cz + (b->cz - a->cz) * t; o->cw = a->cw + (b->cw - a->cw) * t;
    o->r = a->r + (b->r - a->r) * t; o->g = a->g + (b->g - a->g) * t; o->b = a->b + (b->b - a->b) * t; o->a = a->a + (b->a - a->a) * t;
    o->s = a->s + (b->s - a->s) * t; o->t = a->t + (b->t - a->t) * t;
}
static void cv_to_screen(RdpVert *o, const CV *c)
{
    float inv = 1.0f / c->cw;
    o->x = S.vtrans[0] + c->cx * inv * S.vscale[0];
    o->y = S.vtrans[1] - c->cy * inv * S.vscale[1];
    o->z = S.vtrans[2] + c->cz * inv * S.vscale[2];
    o->invw = inv;
    o->r = c->r; o->g = c->g; o->b = c->b; o->a = c->a; o->s = c->s; o->t = c->t;
}

/* Clips a polygon against one plane (inside: dist >= 0). `which` 0 = near (z + w), 1 = w > epsilon. */
static int clip_plane(const CV *in, int n, CV *out, int which)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const CV *a = &in[i], *b = &in[(i + 1) % n];
        float da = which == 0 ? a->cz + a->cw : a->cw - 1e-4f, db = which == 0 ? b->cz + b->cw : b->cw - 1e-4f;
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            cv_lerp(&out[m++], a, b, da / (da - db));
        }
    }
    return m;
}

static void draw_triangle(int i0, int i1, int i2)
{
    Vertex *v[3];
    unsigned c_and;
    int need_clip, k, shade_flat;
    RdpVert o[3];

    if (i0 >= VTX_CACHE || i1 >= VTX_CACHE || i2 >= VTX_CACHE) return;
    v[0] = &S.vtx[i0]; v[1] = &S.vtx[i1]; v[2] = &S.vtx[i2];
    c_and = v[0]->clip & v[1]->clip & v[2]->clip;
    if (c_and) return;                                           /* all outside the same plane */
    S.n_tris++;
    if (trace_limit() && S.n_tris <= (uint32_t)trace_limit()) {
        fprintf(stderr, "gfx: tri clip(%g %g %g %g) (%g %g %g %g) (%g %g %g %g) screen (%g,%g,%g) (%g,%g,%g) (%g,%g,%g) geom=%X\n",
                v[0]->cx, v[0]->cy, v[0]->cz, v[0]->cw, v[1]->cx, v[1]->cy, v[1]->cz, v[1]->cw, v[2]->cx, v[2]->cy, v[2]->cz, v[2]->cw,
                v[0]->out.x, v[0]->out.y, v[0]->out.z, v[1]->out.x, v[1]->out.y, v[1]->out.z, v[2]->out.x, v[2]->out.y, v[2]->out.z, S.geom);
    }
    need_clip = ((v[0]->clip | v[1]->clip | v[2]->clip) & CLIP_NEAR) != 0 || !v[0]->has_screen || !v[1]->has_screen || !v[2]->has_screen;
    shade_flat = !(S.geom & GM_SHADING_SMOOTH);

    if (!need_clip) {
        float area;
        o[0] = v[0]->out; o[1] = v[1]->out; o[2] = v[2]->out;
        area = (o[1].x - o[0].x) * (o[2].y - o[0].y) - (o[2].x - o[0].x) * (o[1].y - o[0].y);
        if (area == 0.0f) return;
        /* Front faces are counter-clockwise in the y-up clip space; after the y flip they have a negative area. FROM KNOWLEDGE, checked on frames. */
        if ((S.geom & GM_CULL_BACK) && area > 0.0f) return;
        if ((S.geom & GM_CULL_FRONT) && area < 0.0f) return;
        if (shade_flat) {
            for (k = 1; k < 3; k++) { o[k].r = o[0].r; o[k].g = o[0].g; o[k].b = o[0].b; o[k].a = o[0].a; }
        }
        rdp_triangle(&o[0], &o[1], &o[2], S.tex_tile, S.tex_level, (S.geom & GM_SHADE) != 0, S.tex_on, (S.geom & GM_ZBUFFER) != 0);
        return;
    }
    {
        /* Cull first (using the unclipped winding is not possible with w <= 0), so clip, then test the winding of the pieces. */
        CV poly[8], tmp[8];
        int n = 3, m, i;
        cv_from(&poly[0], v[0]); cv_from(&poly[1], v[1]); cv_from(&poly[2], v[2]);
        if (shade_flat) {
            for (k = 1; k < 3; k++) { poly[k].r = poly[0].r; poly[k].g = poly[0].g; poly[k].b = poly[0].b; poly[k].a = poly[0].a; }
        }
        m = clip_plane(poly, n, tmp, 0);
        if (m < 3) return;
        n = clip_plane(tmp, m, poly, 1);
        if (n < 3) return;
        for (i = 1; i + 1 < n; i++) {
            RdpVert a, b, c;
            float area;
            cv_to_screen(&a, &poly[0]); cv_to_screen(&b, &poly[i]); cv_to_screen(&c, &poly[i + 1]);
            area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (area == 0.0f) continue;
            if ((S.geom & GM_CULL_BACK) && area > 0.0f) continue;
            if ((S.geom & GM_CULL_FRONT) && area < 0.0f) continue;
            rdp_triangle(&a, &b, &c, S.tex_tile, S.tex_level, (S.geom & GM_SHADE) != 0, S.tex_on, (S.geom & GM_ZBUFFER) != 0);
        }
    }
}

/* ---- the display list walker ----------------------------------------------------------------------------------------------------- */

static void set_other_mode(uint32_t w0, uint32_t w1, int high)
{
    uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
    uint32_t mask = (len >= 32) ? 0xFFFFFFFFu : (((1u << len) - 1u) << shift);
    uint32_t *field = high ? &S.omh : &S.oml;
    *field = (*field & ~mask) | (w1 & mask);
    rdp_set_other_modes(S.omh, S.oml);
}

static void do_movemem(uint32_t w0, uint32_t w1)
{
    uint32_t idx = w0 & 0xFF, ofs = ((w0 >> 8) & 0xFF) * 8u, len = (((w0 >> 19) & 0x1F) * 8u) + 8u;
    const uint8_t *p = mem(resolve(w1), len);
    if (!p) return;
    switch (idx) {
    case MV_VIEWPORT: {
        int i;
        for (i = 0; i < 3; i++) {
            S.vscale[i] = (float)be16s(p + i * 2) / (i < 2 ? 4.0f : 1.0f);      /* x, y have 2 fraction bits (gbi.h Vp comment); z does not */
            S.vtrans[i] = (float)be16s(p + 8 + i * 2) / (i < 2 ? 4.0f : 1.0f);
        }
        if (trace_limit()) fprintf(stderr, "gfx: viewport scale %g %g %g trans %g %g %g\n", S.vscale[0], S.vscale[1], S.vscale[2], S.vtrans[0], S.vtrans[1], S.vtrans[2]);
        break;
    }
    case MV_LIGHT: {
        uint32_t slot = ofs / 24u;                  /* gSPLight / gSPLookAt: byte offset = slot * 24 (gbi.h G_MVO_*) */
        if (slot < 10 && len >= 16) {
            Light *l = &S.light[slot];
            l->col[0] = p[0]; l->col[1] = p[1]; l->col[2] = p[2];
            l->dir[0] = (int8_t)p[8]; l->dir[1] = (int8_t)p[9]; l->dir[2] = (int8_t)p[10];
        }
        break;
    }
    case MV_MATRIX:
        mat_load(S.forced, resolve(w1));
        break;
    default:
        break;                                      /* G_MV_MMTX / PMTX / POINT: not produced by games */
    }
}

static void do_moveword(uint32_t w0, uint32_t w1)
{
    uint32_t idx = (w0 >> 16) & 0xFF, ofs = w0 & 0xFFFF;
    switch (idx) {
    case MW_NUMLIGHT: S.numlights = (int)(w1 / 24u); break;                        /* NUML(n) = n * 24 (gbi.h) */
    case MW_SEGMENT:  S.seg[(ofs >> 2) & 0xF] = w1 & 0x00FFFFFFu; break;
    case MW_FOG:      S.fog_fm = (int16_t)(w1 >> 16); S.fog_fo = (int16_t)w1; break;
    case MW_LIGHTCOL: {
        uint32_t slot = ofs / 24u + 2u;             /* G_MWO_aLIGHT_n = (n-1) * 24; the light array starts at slot 2 */
        if (slot < 10) { S.light[slot].col[0] = (uint8_t)(w1 >> 24); S.light[slot].col[1] = (uint8_t)(w1 >> 16); S.light[slot].col[2] = (uint8_t)(w1 >> 8); }
        break;
    }
    case MW_FORCEMTX: S.mp_forced = (w1 != 0); if (S.mp_forced) memcpy(S.mp, S.forced, sizeof S.mp); break;
    case MW_CLIP: case MW_PERSPNORM: break;
    default: break;
    }
}

static void do_mtx(uint32_t w0, uint32_t w1)
{
    uint32_t params = (w0 & 0xFF) ^ MTX_PUSH;      /* gSPMatrix = gDma2p(G_MTX, m, size, p ^ G_MTX_PUSH, 0): the parameter byte is the low byte of w0 and has the push bit inverted (gbi.h) */
    float m[16], r[16];
    if (!mat_load(m, resolve(w1))) return;
    S.mp_forced = 0;
    if (params & MTX_PROJECTION) {
        if (params & MTX_LOAD) memcpy(S.proj, m, sizeof m); else { mat_mul(r, m, S.proj); memcpy(S.proj, r, sizeof r); }
    } else {
        if (params & MTX_PUSH) {
            if (S.mv_top + 1 >= MAX_MV_STACK) { fu(FU_MV_OVERFLOW, (uint32_t)S.mv_top); }
            else { memcpy(S.mv[S.mv_top + 1], S.mv[S.mv_top], sizeof m); S.mv_top++; }
        }
        if (params & MTX_LOAD) memcpy(S.mv[S.mv_top], m, sizeof m); else { mat_mul(r, m, S.mv[S.mv_top]); memcpy(S.mv[S.mv_top], r, sizeof r); }
    }
    S.mp_dirty = 1;
}

/* Frustum cull of a vertex range (G_CULLDL): returns nonzero if all vertices lie outside one common clip plane. */
static int cull_range(int first, int last)
{
    unsigned c_and = 0xFFFFFFFFu;
    int i;
    if (first < 0 || last >= VTX_CACHE || first > last) return 0;
    for (i = first; i <= last; i++) c_and &= S.vtx[i].clip;
    return (c_and & 0x3F) != 0;
}

/* Development (KYUJO_GFX_BAD_CONTEXT): the last commands executed, so a bad image address can be read in context. */
static struct { uint32_t pc, w0, w1; } sRing[24];
static uint32_t sRingN;
static int ring_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("KYUJO_GFX_BAD_CONTEXT") != NULL;
    return v;
}
static void ring_dump(void)
{
    uint32_t i;
    for (i = 0; i < 24; i++) {
        uint32_t k = (sRingN + i) % 24;
        if (sRing[k].pc) fprintf(stderr, "    ring %08X: %08X %08X\n", sRing[k].pc, sRing[k].w0, sRing[k].w1);
    }
}

/* Fresh RSP state at the start of a task (the microcode's data is reloaded from the task each time). */
void gfx_reset_rsp(void)
{
    int i;
    sTaskNo++;
    memset(&S, 0, sizeof S);
    mat_ident(S.proj); mat_ident(S.mp); mat_ident(S.forced);
    for (i = 0; i < MAX_MV_STACK; i++) mat_ident(S.mv[i]);
    S.tex_s = S.tex_t = 0xFFFF;
    S.geom = 0x00800000u;                                         /* G_CLIPPING on, everything else off (gbi.h comment) */
}

/* ---- resumable task -------------------------------------------------------------------------------------------------------------------
 * The console's RSP runs a graphics task while the CPU goes on, and the game's scheduler can stop it between two commands (SP_SET_YIELD)
 * and restart it later. The interpreter therefore keeps its position in GfxCtx between calls: gfx_task_begin starts a task, and
 * gfx_task_run executes commands until the list ends, a yield is requested, or the call's command budget is used up. Nothing runs in
 * another thread, so the state needs no locking. */
typedef struct GfxCtx {
    uint32_t stack[18];
    int depth;
    uint32_t pc, steps, syncs_before;
} GfxCtx;
static GfxCtx sCtx;

void gfx_task_begin(uint32_t start)
{
    sCtx.depth = 0;
    sCtx.pc = resolve(start);
    sCtx.steps = 0;
    sCtx.syncs_before = rdp_full_syncs();
}

int gfx_task_run(uint32_t budget, const volatile int *yield_req, uint32_t *syncs)
{
    uint32_t stack[18];
    int depth = sCtx.depth;
    uint32_t pc = sCtx.pc;
    uint32_t steps = sCtx.steps;
    uint32_t syncs_before = sCtx.syncs_before;
    uint32_t done_here = 0;
    memcpy(stack, sCtx.stack, sizeof stack);

    for (;;) {
        const uint8_t *c;
        uint32_t w0, w1, op;
        /* between two commands: a yield request ends the call, and so does the budget (the state is saved below) */
        if (yield_req && *yield_req) goto yielded;
        if (budget && done_here >= budget) goto paused;
        done_here++;
        c = mem(pc, 8);
        if (!c) break;
        w0 = be32(c); w1 = be32(c + 4);
        op = w0 >> 24;
        pc += 8;
        S.n_cmds++;
        if (trace_limit() >= 1000) fprintf(stderr, "gfx: [%05X] %08X %08X\n", pc - 8, w0, w1);
        if (ring_on()) { sRing[sRingN % 24].pc = pc - 8; sRing[sRingN % 24].w0 = w0; sRing[sRingN % 24].w1 = w1; sRingN++; }
        if (++steps > 8000000u) host_fatal("display list: more than 8,000,000 commands in one task (a loop?)");

        switch (op) {
        case G_NOOP: case G_SPNOOP: case G_DMA_IO: case 0xD3: case 0xD4: case 0xD5:
            break;
        case G_ENDDL:
            if (depth == 0) goto done;
            pc = stack[--depth];
            break;
        case G_DL: {
            uint32_t target = resolve(w1);
            if (((w0 >> 16) & 0xFF) == 0) {                        /* G_DL_PUSH: call */
                if (depth >= 18) host_fatal("display list: call stack deeper than 18");
                stack[depth++] = pc;
            }
            pc = target;
            break;
        }
        case G_VTX:
            load_vertices(w1, (int)((w0 >> 12) & 0xFF), (int)(((w0 >> 1) & 0x7F) - ((w0 >> 12) & 0xFF)));
            break;
        case G_MODIFYVTX: {
            uint32_t where = (w0 >> 16) & 0xFF, idx = (w0 & 0xFFFF) / 2u;
            if (idx < VTX_CACHE) {
                Vertex *v = &S.vtx[idx];
                if (where == 0x10) { v->out.r = (float)(w1 >> 24); v->out.g = (float)((w1 >> 16) & 0xFF); v->out.b = (float)((w1 >> 8) & 0xFF); v->out.a = (float)(w1 & 0xFF); }
                else if (where == 0x14) { v->out.s = (float)(int16_t)(w1 >> 16) / 32.0f; v->out.t = (float)(int16_t)w1 / 32.0f; }
            }
            break;
        }
        case G_CULLDL:
            if (cull_range((int)((w0 & 0xFFFF) / 2u), (int)((w1 & 0xFFFF) / 2u))) {
                if (depth == 0) goto done;
                pc = stack[--depth];
            }
            break;
        case G_BRANCH_Z: {
            uint32_t vi = (w0 & 0xFFF) / 2u;
            if (vi < VTX_CACHE && S.vtx[vi].has_screen) {
                float zfix = S.vtx[vi].out.z * 65536.0f;            /* G_DEPTOZSrg yields a 16.16 fixed-point screen depth */
                if (zfix <= (float)w1) pc = resolve(S.half1);        /* the branch target came with the preceding G_RDPHALF_1 */
            }
            break;
        }
        case G_TRI1:
            draw_triangle((int)(((w0 >> 16) & 0xFF) / 2u), (int)(((w0 >> 8) & 0xFF) / 2u), (int)((w0 & 0xFF) / 2u));
            break;
        case G_TRI2:
            draw_triangle((int)(((w0 >> 16) & 0xFF) / 2u), (int)(((w0 >> 8) & 0xFF) / 2u), (int)((w0 & 0xFF) / 2u));
            draw_triangle((int)(((w1 >> 16) & 0xFF) / 2u), (int)(((w1 >> 8) & 0xFF) / 2u), (int)((w1 & 0xFF) / 2u));
            break;
        case G_QUAD:
            draw_triangle((int)(((w0 >> 16) & 0xFF) / 2u), (int)(((w0 >> 8) & 0xFF) / 2u), (int)((w0 & 0xFF) / 2u));
            draw_triangle((int)(((w1 >> 16) & 0xFF) / 2u), (int)(((w1 >> 8) & 0xFF) / 2u), (int)((w1 & 0xFF) / 2u));
            break;
        case G_LINE3D: fu(FU_LINE, w0); break;
        case G_TEXTURE:
            S.tex_level = (int)((w0 >> 11) & 7); S.tex_tile = (int)((w0 >> 8) & 7); S.tex_on = ((w0 >> 1) & 0x7F) != 0;
            S.tex_s = (int)(w1 >> 16); S.tex_t = (int)(w1 & 0xFFFF);
            break;
        case G_POPMTX: {
            uint32_t n = w1 >> 6;
            if (n > (uint32_t)S.mv_top) n = (uint32_t)S.mv_top;
            S.mv_top -= (int)n;
            S.mp_dirty = 1;
            break;
        }
        case G_GEOMETRYMODE: S.geom = (S.geom & (w0 & 0x00FFFFFFu)) | w1; break;
        case G_MTX: do_mtx(w0, w1); break;
        case G_MOVEWORD: do_moveword(w0, w1); break;
        case G_MOVEMEM: do_movemem(w0, w1); break;
        case G_LOAD_UCODE: fu(FU_UCODE, w1); break;
        case G_RDPHALF_1: S.half1 = w1; break;
        case G_RDPHALF_2: break;
        case G_SETOTHERMODE_L: set_other_mode(w0, w1, 0); break;
        case G_SETOTHERMODE_H: set_other_mode(w0, w1, 1); break;
        case G_RDPSETOTHERMODE:
            S.omh = w0 & 0x00FFFFFFu; S.oml = w1;
            rdp_set_other_modes(S.omh, S.oml);
            break;
        case G_TEXRECT: case G_TEXRECTFLIP: {
            /* followed by G_RDPHALF_1 (s, t) and G_RDPHALF_2 (dsdx, dtdy) */
            const uint8_t *c2 = mem(pc, 16);
            if (c2 && (c2[0] == G_RDPHALF_1) && (c2[8] == G_RDPHALF_2)) {
                if (trace_limit() >= 1000) fprintf(stderr, "gfx:   texrect halves %08X %08X\n", be32(c2 + 4), be32(c2 + 12));
                rdp_texture_rectangle(w0, w1, be32(c2 + 4), be32(c2 + 12), op == G_TEXRECTFLIP);
                pc += 16;
            } else {
                fu(FU_UNKNOWN_OP, op);
            }
            break;
        }
        case G_SETCIMG: case G_SETZIMG: case G_SETTIMG: {
            uint32_t phys = resolve(w1);                           /* the microcode turns the segment address into a physical one */
            static int reported;
            if (!host_game_range_ok(phys, 1) && reported < 8) {    /* development aid: which segment gave an address outside the program? */
                reported++;
                fprintf(stderr, "kyujo: gfx: image address outside the program: command 0x%02X at 0x%08X, operand 0x%08X = segment %u offset 0x%06X, "
                        "segment base 0x%08X -> 0x%08X\n", op, (unsigned)(pc - 8), w1, (unsigned)((w1 >> 24) & 0xF), (unsigned)(w1 & 0xFFFFFFu),
                        (unsigned)S.seg[(w1 >> 24) & 0xF], phys);
                if (ring_on()) {
                    int k;
                    fprintf(stderr, "    segments:");
                    for (k = 0; k < 16; k++) fprintf(stderr, " %d=%06X", k, (unsigned)S.seg[k]);
                    fprintf(stderr, "\n");
                    ring_dump();
                }
            }
            rdp_command(w0, phys);
            break;
        }
        default:
            if (op >= 0xE6) {                                      /* RDP commands pass through unchanged */
                rdp_command(w0, w1);
            } else {
                fu(FU_UNKNOWN_OP, op);
            }
            break;
        }
    }
done:
    if (getenv("KYUJO_GFX_SUMMARY")) {
        fprintf(stderr, "gfx: task %u: %u commands, %u triangles, color image %06X\n", sTaskNo, S.n_cmds, S.n_tris, rdp_color_image_addr());
    }
    *syncs = rdp_full_syncs() - syncs_before;
    sCtx.depth = depth;                         /* the task is over: nothing to resume */
    return HOST_GFX_DONE;
yielded:
paused:
    /* save the position; the next gfx_task_run continues here */
    sCtx.depth = depth; sCtx.pc = pc; sCtx.steps = steps; sCtx.syncs_before = syncs_before;
    memcpy(sCtx.stack, stack, sizeof stack);
    *syncs = rdp_full_syncs() - syncs_before;
    return yield_req && *yield_req ? HOST_GFX_YIELDED : HOST_GFX_PAUSED;
}

/* The whole list in one call (the tests and any caller that does not need to pause). */
uint32_t gfx_run_display_list(uint32_t start)
{
    uint32_t syncs = 0;
    gfx_task_begin(start);
    gfx_task_run(0, NULL, &syncs);
    return syncs;
}
