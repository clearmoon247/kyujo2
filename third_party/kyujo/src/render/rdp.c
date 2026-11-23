/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdp.c - a software model of the console's RDP (Reality Display Processor), HOST-side.
 *
 * Sources used (all of them are said where they matter; nothing here comes from the leaked/unpublished hardware sources):
 *   - command layouts, other-modes bits, combiner/blender/z-compare rules: n64brew "Reality Display Processor/Commands" and the
 *     fast64 "Graphics Pipeline Details" pages (Color Combiner, Blender), see docs/rdp-notes.md;
 *   - the encodings the GAME uses (combiner input numbers, blender input numbers, render-mode bits, image formats): the decomp's
 *     lib/ultralib/include/PR/gbi.h. tests/native/render/rdp_gbi_check.c builds display lists with that header's macros and checks the
 *     resulting pixels (colour combiner modes and vertex lighting; NOT every command: see the test's header for what is covered);
 *   - texture memory (TMEM) layout and filtering: the parts that are not in a document I could read are marked "FROM KNOWLEDGE" and are
 *     listed in docs/rdp-notes.md as unverified. They are decided by looking at rendered frames.
 *
 * Deviations from the hardware (also reported at run time by rdp_report_unimplemented):
 *   1. Coverage / anti-aliasing: every pixel a primitive touches is treated as fully covered (coverage 8/8) and the "memory coverage" of
 *      the frame buffer is taken to be 7 (the value the hardware assumes for a cleared frame), so edge anti-aliasing is not drawn. With
 *      those two values the blender enable rule of the hardware (blend_en = force_blend || (!overflow && aa_en && farther)) always reduces
 *      to blend_en = force_blend, which is what is implemented.
 *   2. Triangles are rasterised from floating-point vertices at pixel centres, not by the RDP's fixed-point edge walker, so single pixels on
 *      edges can differ from the hardware.
 *   3. The z-buffer is 16 bit like the hardware's, but the 14-bit depth in it is LINEAR (screen z * 16) instead of the hardware's
 *      3-bit-exponent/11-bit-mantissa format (gbi.h calls G_MAXFBZ "3b exp, 11b mantissa"), and the 2-bit delta-z field is stored as 0.
 *      The value the game clears the buffer with (G_MAXFBZ << 2 = 0xFFFC) still means "farthest".
 *   4. Dither, chroma key, YUV textures, mip-map level selection (LOD), detail/sharpen textures, line primitives are not implemented
 *      (the first use of each is counted and reported).
 *   5. 1-cycle mode uses the first combiner/blender cycle slot; TEXEL1 is the same texel as TEXEL0 there (the hardware says undefined).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdram.h"
#include "rdp.h"
#ifdef RDP_GPU
#include "rdp_gpu.h"
#ifdef RDP_GPU
static int verify_on(void);
#endif
#endif

/* ---- small helpers -------------------------------------------------------------------------------------------------------------- */

/* SPEED: the optimiser left these tiny helpers as real calls in the per-pixel code (they showed up as functions of their own in the profile). */
#if defined(__GNUC__) || defined(__clang__)
#define PS_INLINE static inline __attribute__((always_inline))
#else
#define PS_INLINE static inline
#endif
PS_INLINE int imin(int a, int b) { return a < b ? a : b; }
PS_INLINE int imax(int a, int b) { return a > b ? a : b; }
PS_INLINE int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
PS_INLINE int ifloor(float f) { int i = (int)f; return (f < 0.0f && (float)i != f) ? i - 1 : i; }

/* Things the renderer knows it does not do. Each has a counter; the first hit is reported at once, the totals at exit. */
enum {
    UN_YUV_TEXTURE, UN_CHROMA_KEY, UN_LOD, UN_DETAIL_SHARPEN, UN_DITHER, UN_LINE, UN_BAD_ADDRESS, UN_TWO_CYCLE_SLOT_MISMATCH,
    UN_TEX_FORMAT, UN_FILL_TRIANGLE_Z_IMAGE, UN_COUNT
};
static const char *const kUnName[UN_COUNT] = {
    "YUV texture format", "chroma key", "mip-map LOD selection", "detail/sharpen textures", "colour/alpha dither", "line primitive",
    "RDRAM address outside game memory", "1-cycle mode with different settings in the two cycle slots", "texture format/size combination",
    "z-buffer image equal to colour image",
};
static uint32_t sUnCount[UN_COUNT];

/* Development switches read from the environment. getenv() is a linear search through the environment strings and was found (sampling profiler,
 * docs/known-decomp-issues.md) to cost several percent of the whole run when called for every command or rectangle; so each is looked up once. */
static int env_set(const char *name, int *cache)
{
    if (*cache < 0) *cache = getenv(name) != NULL;
    return *cache;
}
static int sEnvTrace = -1, sEnvTexels = -1, sEnvTileDump = -1, sEnvCmds = -1;
/* Tracing of texture rectangles: KYUJO_RDP_TRACE=1 traces all of them; the development control file (host_control.c) can switch it on and
 * off at run time with "rdptrace on" / "rdptrace off", which is how one picture of a long scene is captured (a whole scene prints far too much). */
static int sTraceRuntime;
void host_rdp_trace(int on) { sTraceRuntime = on; }
static int trace_active(void)
{
    return sTraceRuntime || env_set("KYUJO_RDP_TRACE", &sEnvTrace);
}

static void unimpl(int what)
{
    if (sUnCount[what]++ == 0) {
        fprintf(stderr, "kyujo: renderer: not implemented, used for the first time: %s\n", kUnName[what]);
    }
}

void rdp_report_unimplemented(void)
{
    int i;
    for (i = 0; i < UN_COUNT; i++) {
        if (sUnCount[i]) {
            fprintf(stderr, "kyujo: renderer: %s: %u times\n", kUnName[i], sUnCount[i]);
        }
    }
}

/* ---- RDRAM access (the game's memory is big-endian; the address is the native pointer, see platform.h) -------------------------------- */

/* All game-visible memory is inside the program image (tools/native/imagelimit.py keeps it below 16 MiB). An address outside it is garbage:
 * on the console the RDP would read some wrapped-around RDRAM, here dereferencing it would crash the program (found by the bounds-checking
 * build: LoadBlock with texture addresses 0xFFDB4B and 0x6150, outside the image). So it is refused, reported once, and the command skipped. */
static uint32_t sRamLo, sRamHi;          /* the program image's address range, fetched once: the pixel loops check every access */
static uint8_t *ram(uint32_t addr, uint32_t len)
{
    static int reported;
    if (!sRamHi) host_game_range_bounds(&sRamLo, &sRamHi);
    if (!(len <= sRamHi - sRamLo && addr >= sRamLo && addr <= sRamHi - len)) {      /* = !host_game_range_ok(addr, len), inline */
        if (!reported) {
            reported = 1;
            fprintf(stderr, "kyujo: renderer: first bad RDRAM access: address 0x%08X, length %u\n", addr, len);
        }
        unimpl(UN_BAD_ADDRESS);
        return 0;
    }
    return RDRAM_PTR(addr);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- state ------------------------------------------------------------------------------------------------------------------------ */

enum { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };       /* Set_Other_Modes cycle_type */
enum { FMT_RGBA = 0, FMT_YUV = 1, FMT_CI = 2, FMT_IA = 3, FMT_I = 4 };    /* gbi.h G_IM_FMT_* */
enum { SIZ_4 = 0, SIZ_8 = 1, SIZ_16 = 2, SIZ_32 = 3 };                    /* gbi.h G_IM_SIZ_* */

typedef struct Tile {
    uint8_t fmt, siz, palette;
    uint16_t line;                  /* TMEM row length in 64-bit words */
    uint16_t tmem;                  /* TMEM start in 64-bit words */
    uint8_t clamp_s, clamp_t, mirror_s, mirror_t, mask_s, mask_t, shift_s, shift_t;
    uint16_t sl, tl, sh, th;        /* tile size, 10.2 fixed point texels */
} Tile;

typedef struct Image {
    uint8_t fmt, siz;
    uint16_t width;                 /* pixels per line */
    uint32_t addr;
} Image;

/* The colour combiner's settings, resolved when Set_Combine_Mode arrives (see "colour combiner" below). */
typedef struct CombPlan {
    uint8_t idx[2][4][4];       /* [cycle][operand A, B, C, D][channel R, G, B, A] -> index into the input table v[] */
    uint8_t uses_t[2][2];       /* [cycle][0 = TEXEL0, 1 = TEXEL1]: does that cycle's combiner read any channel of that input at all?
                                   (set by comb_plan_build; lets fetch_texels skip texture samples nobody reads) */
} CombPlan;
static void comb_plan_build(CombPlan *pl, uint32_t hi, uint32_t lo);

static struct {
    uint32_t omh, oml;              /* other modes: bits 55:32 and 31:0 of Set_Other_Modes */
    uint32_t comb_hi, comb_lo;      /* Set_Combine_Mode words */
    CombPlan comb_plan;             /* the same, resolved */
    Tile tile[8];
    uint8_t tmem[4096];
    uint32_t tmem_gen;              /* incremented by every command that changes TMEM (texture cache validity, see "texture cache") */
    uint32_t tile_ver[8];           /* incremented by every command that changes a tile's description */
    uint32_t fill_color;
    uint8_t fog[4], blend[4], prim[4], env[4];      /* R, G, B, A */
    uint8_t prim_min_lod, prim_lod_frac;
    uint16_t prim_z, prim_dz;
    int scis_xh, scis_yh, scis_xl, scis_yl;         /* 10.2 */
    Image cimg, timg;
    uint32_t zimg;
    int k4, k5;                     /* convert constants used by the combiner (9-bit signed) */
    uint32_t full_syncs;
    uint32_t n_tri, n_rect;
    uint64_t n_pix;
    uint32_t noise;
} R;

static uint32_t sResets;                 /* how often rdp_reset ran: texture caches from before a reset are not valid (the counters they compare restart at 0) */

void rdp_reset(void)
{
    sResets++;
    memset(&R, 0, sizeof R);
    R.noise = 1;
    comb_plan_build(&R.comb_plan, R.comb_hi, R.comb_lo);
}

uint32_t rdp_full_syncs(void) { return R.full_syncs; }
void rdp_memory_reused(uint32_t addr, uint32_t len)
{
#ifdef RDP_GPU
    if (gpu_active()) gpu_invalidate(addr, len);
#else
    (void)addr; (void)len;
#endif
}
uint32_t rdp_color_image_addr(void) { return R.cimg.addr; }

/* ---- which colour images the running task has selected (read by the game side while the task runs on the worker) --------------------
 * The game side must know whether a task is drawing into the frame buffer it is about to show (hw_sp.c, HostHwGfxCompleteFor). Each G_SETCIMG
 * of the task is logged here: the image's start and the size of two of its lines (the odd field of an interlaced picture starts one line in).
 * Written only by the thread that runs the task; the reader may see an entry one command late, which at worst shows the picture of the
 * previous frame once more, never a crash. More than CIMG_LOG different images in one task: "unknown" (the reader then waits for the task). */
#define CIMG_LOG 8
static uint32_t sCimgStart[CIMG_LOG], sCimgSpan[CIMG_LOG];
static uint32_t sCimgN;                                            /* entries valid; CIMG_LOG + 1 = overflowed */
void rdp_cimg_log_reset(void) { __atomic_store_n(&sCimgN, 0, __ATOMIC_RELEASE); }
static void cimg_log_add(uint32_t addr, uint32_t line_bytes)
{
    uint32_t n = __atomic_load_n(&sCimgN, __ATOMIC_RELAXED), i;
    if (n > CIMG_LOG) return;
    for (i = 0; i < n; i++) if (sCimgStart[i] == addr) return;
    if (n == CIMG_LOG) { __atomic_store_n(&sCimgN, CIMG_LOG + 1, __ATOMIC_RELEASE); return; }
    sCimgStart[n] = addr;
    sCimgSpan[n] = line_bytes * 2u;
    __atomic_store_n(&sCimgN, n + 1, __ATOMIC_RELEASE);
}
/* Nonzero if the task has selected a colour image that starts at `origin` (or, for the odd field, one line later), or if that is unknown. */
int rdp_task_may_draw_to(uint32_t origin)
{
    uint32_t n = __atomic_load_n(&sCimgN, __ATOMIC_ACQUIRE), i;
    if (n > CIMG_LOG) return 1;
    origin &= 0x00FFFFFFu;
    for (i = 0; i < n; i++) if (origin - sCimgStart[i] < sCimgSpan[i]) return 1;
    return 0;
}

/* Development (KYUJO_GFX_BENCH, gfx_host.c): a 64-bit FNV-1a hash of the colour image as the last display list left it (width x scissor
 * height pixels), so two builds of the renderer can be compared on a real game frame. Reads RDRAM through the same bounds check as drawing. */
uint64_t rdp_dev_color_image_hash(void)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t bytes_px = R.cimg.siz == SIZ_32 ? 4u : 2u, rows = (uint32_t)((R.scis_yl + 3) >> 2), n, i;
    const uint8_t *p;
    if (R.cimg.siz != SIZ_16 && R.cimg.siz != SIZ_32) return 0;
    n = R.cimg.width * rows * bytes_px;
    p = ram(R.cimg.addr, n);
    if (!p) return 0;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

void rdp_stats(uint32_t *triangles, uint32_t *rects, uint64_t *pixels)
{
    *triangles = R.n_tri;
    *rects = R.n_rect;
    *pixels = R.n_pix;
}

/* Other-modes accessors. Bit numbers are those of the 64-bit command (n64brew); `omh` holds bits 55:32, so bit n of the command is bit n-32. */
#define OM_CYCLE()          ((R.omh >> 20) & 3)
#define OM_PERSP()          ((R.omh >> 19) & 1)
#define OM_DETAIL()         ((R.omh >> 18) & 1)
#define OM_SHARPEN()        ((R.omh >> 17) & 1)
#define OM_LOD_EN()         ((R.omh >> 16) & 1)
#define OM_TLUT_EN()        ((R.omh >> 15) & 1)
#define OM_TLUT_IA()        ((R.omh >> 14) & 1)                 /* tlut_type: 0 = RGBA16, 1 = IA16 */
#define OM_BILERP()         ((R.omh >> 13) & 1)                 /* sample_type */
#define OM_KEY()            ((R.omh >> 8) & 1)
#define OM_RGB_DITHER()     ((R.omh >> 6) & 3)                  /* 3 = no dither */
#define OM_ALPHA_DITHER()   ((R.omh >> 4) & 3)                  /* 3 = no dither */
#define OM_FORCE_BLEND()    ((R.oml >> 14) & 1)
#define OM_ALPHA_CVG_SEL()  ((R.oml >> 13) & 1)
#define OM_CVG_X_ALPHA()    ((R.oml >> 12) & 1)
#define OM_Z_MODE()         ((R.oml >> 10) & 3)                 /* 0 opaque, 1 interpenetrating, 2 translucent, 3 decal */
#define OM_IMAGE_READ()     ((R.oml >> 6) & 1)
#define OM_Z_UPDATE()       ((R.oml >> 5) & 1)
#define OM_Z_COMPARE()      ((R.oml >> 4) & 1)
#define OM_AA()             ((R.oml >> 3) & 1)
#define OM_Z_SOURCE_PRIM()  ((R.oml >> 2) & 1)
#define OM_DITHER_ALPHA()   ((R.oml >> 1) & 1)
#define OM_ALPHA_COMPARE()  ((R.oml >> 0) & 1)

/* ---- texture memory --------------------------------------------------------------------------------------------------------------- */
/*
 * TMEM is 4096 bytes organised as 512 64-bit words. A tile names a start word and a row length in words. FROM KNOWLEDGE (n64brew's TMEM
 * page was not reachable): in every second row the two 32-bit halves of each 64-bit word are swapped (byte address ^ 4), so that a
 * bilinear fetch of two adjacent rows reads two different memory banks. RGBA32 texels are stored as red/green in the lower 2 KiB and
 * blue/alpha at the same address + 0x800. Palettes (TLUT) live in the upper 2 KiB, every entry replicated 4 times (8 bytes per entry).
 * This file loads and reads TMEM through the same address function, so the layout is consistent even if the hardware's differs in
 * details that the game cannot observe.
 */
static const int kBitsOfSize[4] = {4, 8, 16, 32};

static uint32_t tmem_row_addr(const Tile *tl, int row)      /* byte address of the start of tile row `row` (tile-relative) */
{
    return ((uint32_t)tl->tmem * 8u + (uint32_t)row * tl->line * 8u) & 0xFFFu;
}

static uint32_t tmem_swz(int row) { return (row & 1) ? 4u : 0u; }

/* G_SETTILESIZE etc. arrive as (sl, tl) and (sh, th) in 10.2. */

/* Loads `nbytes` source bytes per row into tile rows (Load_Tile for 4/8/16-bit data). Every second row has its 32-bit halves swapped. */
static void load_rows_bytes(const Tile *tl, const uint8_t *src, uint32_t src_stride, uint32_t nbytes, int rows)
{
    int r;
    for (r = 0; r < rows; r++) {
        uint32_t base = tmem_row_addr(tl, r), swz = tmem_swz(r), i;
        for (i = 0; i < nbytes; i++) {
            R.tmem[((base + i) ^ swz) & 0xFFF] = src[(uint32_t)r * src_stride + i];
        }
    }
}

/* Load_Tile (0x34): copies a rectangle of the texture image into TMEM. */
static void cmd_load_tile(uint32_t hi, uint32_t lo)
{
    R.tmem_gen++;
    Tile *tl = &R.tile[(lo >> 24) & 7];
    int sl = (hi >> 12) & 0xFFF, t_l = hi & 0xFFF, sh = (lo >> 12) & 0xFFF, th = lo & 0xFFF;
    int s0 = sl >> 2, t0 = t_l >> 2, s1 = sh >> 2, t1 = th >> 2;        /* texel coordinates, inclusive */
    int bits = kBitsOfSize[R.timg.siz], rows = t1 - t0 + 1, cols = s1 - s0 + 1;
    uint32_t row_bytes, src_stride = ((uint32_t)R.timg.width * (uint32_t)bits + 7u) / 8u, src_off;
    const uint8_t *src;
    int r, c;

    tl->sl = (uint16_t)sl; tl->tl = (uint16_t)t_l; tl->sh = (uint16_t)sh; tl->th = (uint16_t)th;      /* the command also sets the tile size */
    R.tile_ver[tl - R.tile]++;
    if (rows <= 0 || cols <= 0 || rows > 512 || cols > 2048) {
        return;
    }
    src_off = (uint32_t)t0 * src_stride + ((uint32_t)s0 * (uint32_t)bits) / 8u;
    row_bytes = ((uint32_t)cols * (uint32_t)bits + 7u) / 8u;
#ifdef RDP_GPU
    if (gpu_active() && !verify_on()) gpu_sync_to_ram(R.timg.addr + src_off, (uint32_t)(rows - 1) * src_stride + row_bytes);
#endif
    src = ram(R.timg.addr + src_off, (uint32_t)(rows - 1) * src_stride + row_bytes);
    if (!src) {
        return;
    }
    if (R.timg.siz != SIZ_32) {
        load_rows_bytes(tl, src, src_stride, row_bytes, rows);
        return;
    }
    for (r = 0; r < rows; r++) {                    /* RGBA32: red/green low half, blue/alpha high half */
        uint32_t base = tmem_row_addr(tl, r), swz = tmem_swz(r);
        for (c = 0; c < cols; c++) {
            const uint8_t *p = src + (uint32_t)r * src_stride + (uint32_t)c * 4u;
            uint32_t a = (base + (uint32_t)c * 2u) ;
            R.tmem[(a ^ swz) & 0xFFF] = p[0];
            R.tmem[((a + 1) ^ swz) & 0xFFF] = p[1];
            R.tmem[((a ^ swz) + 0x800) & 0xFFF] = p[2];
            R.tmem[(((a + 1) ^ swz) + 0x800) & 0xFFF] = p[3];
        }
    }
}

/* Load_Block (0x33): copies a run of texels; `dxt` (1.11 fixed point) says how many 64-bit words make a texture row, which decides where
 * the every-second-row swap happens. */
static void cmd_load_block(uint32_t hi, uint32_t lo)
{
    R.tmem_gen++;
    Tile *tl = &R.tile[(lo >> 24) & 7];
    int sl = (hi >> 12) & 0xFFF, t_l = hi & 0xFFF, sh = (lo >> 12) & 0xFFF;
    uint32_t dxt = lo & 0xFFF;
    int bits = kBitsOfSize[R.timg.siz];
    uint32_t texels = (uint32_t)(sh - sl + 1), nbytes, src_off, j, nwords;
    const uint8_t *src;

    if (sh < sl) {
        return;
    }
    src_off = ((uint32_t)t_l * (uint32_t)R.timg.width + (uint32_t)sl) * (uint32_t)bits / 8u;
    nbytes = texels * (uint32_t)bits / 8u;
    if (nbytes == 0 || nbytes > 4096u * 2u) {
        return;
    }
#ifdef RDP_GPU
    if (gpu_active() && !verify_on()) gpu_sync_to_ram(R.timg.addr + src_off, nbytes);
#endif
    src = ram(R.timg.addr + src_off, nbytes);
    if (!src) {
        return;
    }
    nwords = (nbytes + 7u) / 8u;
    if (R.timg.siz == SIZ_32) {
        /* Each 64-bit source word holds two RGBA32 texels; their red/green go to the low half and blue/alpha to the high half. */
        for (j = 0; j < nwords; j++) {
            uint32_t odd = ((j * dxt) >> 11) & 1u, k;
            for (k = 0; k < 2; k++) {
                const uint8_t *p = src + j * 8u + k * 4u;
                uint32_t a = (uint32_t)tl->tmem * 8u + j * 4u + k * 2u;
                if (j * 8u + k * 4u + 4u > nbytes) break;
                R.tmem[(a ^ (odd ? 4u : 0u)) & 0xFFF] = p[0];
                R.tmem[((a + 1) ^ (odd ? 4u : 0u)) & 0xFFF] = p[1];
                R.tmem[((a ^ (odd ? 4u : 0u)) + 0x800) & 0xFFF] = p[2];
                R.tmem[(((a + 1) ^ (odd ? 4u : 0u)) + 0x800) & 0xFFF] = p[3];
            }
        }
        return;
    }
    for (j = 0; j < nwords; j++) {
        uint32_t odd = ((j * dxt) >> 11) & 1u, k;
        for (k = 0; k < 8 && j * 8u + k < nbytes; k++) {
            uint32_t a = (uint32_t)tl->tmem * 8u + j * 8u + k;
            R.tmem[(a ^ (odd ? 4u : 0u)) & 0xFFF] = src[j * 8u + k];
        }
    }
}

/* Load_TLUT (0x30): copies 16-bit palette entries; each is stored four times in a row (8 bytes per entry). */
static void cmd_load_tlut(uint32_t hi, uint32_t lo)
{
    R.tmem_gen++;
    Tile *tl = &R.tile[(lo >> 24) & 7];
    int sl = (hi >> 12) & 0xFFF, t_l = hi & 0xFFF, sh = (lo >> 12) & 0xFFF;
    int s0 = sl >> 2, s1 = sh >> 2, n = s1 - s0 + 1, i;
    const uint8_t *src;
    uint32_t base = (uint32_t)tl->tmem * 8u;

    if (n <= 0 || n > 256) {
        return;
    }
#ifdef RDP_GPU
    if (gpu_active() && !verify_on()) gpu_sync_to_ram(R.timg.addr + ((uint32_t)(t_l >> 2) * R.timg.width + (uint32_t)s0) * 2u, (uint32_t)n * 2u);
#endif
    src = ram(R.timg.addr + ((uint32_t)(t_l >> 2) * R.timg.width + (uint32_t)s0) * 2u, (uint32_t)n * 2u);
    if (!src) {
        return;
    }
    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; k < 4; k++) {
            R.tmem[(base + (uint32_t)i * 8u + (uint32_t)k * 2u) & 0xFFF] = src[i * 2];
            R.tmem[(base + (uint32_t)i * 8u + (uint32_t)k * 2u + 1u) & 0xFFF] = src[i * 2 + 1];
        }
    }
}

/* ---- texel fetch ------------------------------------------------------------------------------------------------------------------ */

static uint8_t c5to8(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }

/* Reads texel (s, t) of the tile (tile-relative, already wrapped/clamped) as RGBA 0..255. */
static void texel_at(const Tile *tl, int s, int t, uint8_t out[4])
{
    uint32_t row = tmem_row_addr(tl, t), swz = tmem_swz(t), a;
    unsigned v;

    switch (tl->siz) {
    case SIZ_4: {
        a = (row + (uint32_t)(s >> 1)) ^ swz;
        v = R.tmem[a & 0xFFF];
        v = (s & 1) ? (v & 0xF) : (v >> 4);
        if (tl->fmt == FMT_CI) {
            goto palette;
        } else if (tl->fmt == FMT_IA) {
            unsigned i3 = v >> 1, i8 = (i3 << 5) | (i3 << 2) | (i3 >> 1);
            out[0] = out[1] = out[2] = (uint8_t)i8; out[3] = (v & 1) ? 255 : 0;
        } else {                                                    /* I4 (RGBA4 does not exist; treated like I4) */
            out[0] = out[1] = out[2] = out[3] = (uint8_t)(v * 17);
        }
        return;
    }
    case SIZ_8: {
        a = (row + (uint32_t)s) ^ swz;
        v = R.tmem[a & 0xFFF];
        if (tl->fmt == FMT_CI) {
            goto palette;
        } else if (tl->fmt == FMT_IA) {
            out[0] = out[1] = out[2] = (uint8_t)((v >> 4) * 17); out[3] = (uint8_t)((v & 0xF) * 17);
        } else {                                                    /* I8 */
            out[0] = out[1] = out[2] = out[3] = (uint8_t)v;
        }
        return;
    }
    case SIZ_16: {
        a = (row + (uint32_t)s * 2u);
        v = ((unsigned)R.tmem[(a ^ swz) & 0xFFF] << 8) | R.tmem[((a + 1) ^ swz) & 0xFFF];
        if (tl->fmt == FMT_IA) {
            out[0] = out[1] = out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
        } else if (tl->fmt == FMT_YUV) {
            unimpl(UN_YUV_TEXTURE);
            out[0] = out[1] = out[2] = 0; out[3] = 255;
        } else if (tl->fmt == FMT_CI) {
            unimpl(UN_TEX_FORMAT);
            out[0] = out[1] = out[2] = out[3] = 0;
        } else {                                                    /* RGBA5551 */
            out[0] = c5to8((v >> 11) & 31); out[1] = c5to8((v >> 6) & 31); out[2] = c5to8((v >> 1) & 31); out[3] = (v & 1) ? 255 : 0;
        }
        return;
    }
    default: {                                                      /* SIZ_32: RGBA8888 */
        a = (row + (uint32_t)s * 2u);
        out[0] = R.tmem[(a ^ swz) & 0xFFF];
        out[1] = R.tmem[((a + 1) ^ swz) & 0xFFF];
        out[2] = R.tmem[(((a ^ swz)) + 0x800) & 0xFFF];
        out[3] = R.tmem[(((a + 1) ^ swz) + 0x800) & 0xFFF];
        if (tl->fmt != FMT_RGBA) {
            unimpl(UN_TEX_FORMAT);
        }
        return;
    }
    }
palette:
    {
        /* Colour-index texel: look the index up in the palette (upper half of TMEM). With the lookup switched off the hardware passes the
         * index through as intensity; that mode is not used by the game's palettised textures, so only the lookup is modelled. */
        unsigned idx = (tl->siz == SIZ_4) ? (((unsigned)tl->palette << 4) | v) : v;
        unsigned e = ((unsigned)R.tmem[(0x800u + idx * 8u) & 0xFFF] << 8) | R.tmem[(0x800u + idx * 8u + 1u) & 0xFFF];
        if (!OM_TLUT_EN()) {
            out[0] = out[1] = out[2] = out[3] = (uint8_t)idx;
        } else if (OM_TLUT_IA()) {
            out[0] = out[1] = out[2] = (uint8_t)(e >> 8); out[3] = (uint8_t)e;
        } else {
            out[0] = c5to8((e >> 11) & 31); out[1] = c5to8((e >> 6) & 31); out[2] = c5to8((e >> 1) & 31); out[3] = (e & 1) ? 255 : 0;
        }
    }
}

/* Applies the tile's clamp / mirror / mask to a tile-relative texel coordinate on one axis. `size` is the tile size in texels (sh - sl + 1). */
static int wrap_coord(int c, int clamp, int mirror, int mask, int size)
{
    if (clamp) {
        c = iclamp(c, 0, size - 1);
    }
    if (mask) {
        int m = (1 << mask) - 1;
        if (mirror && (c & (1 << mask))) {
            c = ~c;
        }
        c &= m;
    }
    return c;
}

/* ---- texture cache -----------------------------------------------------------------------------------------------------------------
 * SPEED: texel_at() decodes a texel from TMEM (format switch, row address, swizzle, palette lookup) on every fetch, and a bilinear sample
 * fetches three of them per pixel; together with sample_tile that was the biggest part of the run time. Each tile now keeps the texels it has
 * already decoded as RGBA8888, valid as long as nothing changed that could alter them: TMEM contents (R.tmem_gen), the tile's description
 * (R.tile_ver[]) and the palette mode bits of Set_Other_Modes. A texel is decoded the first time it is asked for (so a small rectangle that
 * touches 64 texels of a big tile costs 64 decodes, not the whole tile). Coordinates outside the cached rectangle fall back to texel_at().
 * The cached value is exactly what texel_at() returns, so pictures do not change.
 */
typedef struct TexCache {
    uint32_t gen, ver, tlut, epoch; /* the state the contents belong to (see above; epoch = sResets) */
    int valid;                      /* initialised for that state at all */
    int w, h;                       /* cached rectangle, texels; 0 = this tile is not cached */
    uint32_t *px;                   /* w * h decoded texels, bytes R,G,B,A in memory order */
    uint16_t *stamp;                /* px[i] is decoded when stamp[i] == cur */
    uint16_t cur;
} TexCache;
static TexCache sTc[8];
#define TC_MAX_TEXELS 16384         /* TMEM holds 4 KiB: at most 8192 texels of 4 bits; the rest is slack for masked tiles */

static void tc_reset(TexCache *tc, const Tile *tl, uint32_t tlut_bits)
{
    int sw = ((int)tl->sh - (int)tl->sl) / 4 + 1, th = ((int)tl->th - (int)tl->tl) / 4 + 1;
    /* The texels a sample can reach (see wrap_coord): with a mask, 0 .. 2^mask - 1 (fewer if the clamp comes first and the tile is smaller); without one, the tile's
     * own size. A scrolling texture moves the tile window (sl, tl) while its lower right corner moves faster, so the window can grow far beyond what the mask lets
     * anyone reach: using the window size here made such a tile too big for the cache (and for the GPU's copy of it) after a few frames. */
    int ew = tl->mask_s ? ((tl->clamp_s && sw < (1 << tl->mask_s)) ? sw : (1 << tl->mask_s)) : sw;
    int eh = tl->mask_t ? ((tl->clamp_t && th < (1 << tl->mask_t)) ? th : (1 << tl->mask_t)) : th;
    tc->gen = R.tmem_gen; tc->ver = R.tile_ver[tl - R.tile]; tc->tlut = tlut_bits; tc->epoch = sResets; tc->valid = 1;
    tc->w = tc->h = 0;
    /* (A tile whose lower right corner lies above/left of its upper left one has a negative size; with a mask and no clamp the size does not matter, and games
     * do scroll textures that way.) */
    if (ew <= 0 || eh <= 0 || ew > 2048 || eh > 2048 || ew * eh > TC_MAX_TEXELS) return;
    if (!tc->px) {
        tc->px = (uint32_t *)malloc(TC_MAX_TEXELS * sizeof(uint32_t));
        tc->stamp = (uint16_t *)calloc(TC_MAX_TEXELS, sizeof(uint16_t));
        tc->cur = 0;
        if (!tc->px || !tc->stamp) { free(tc->px); free(tc->stamp); tc->px = 0; tc->stamp = 0; return; }
    }
    if (++tc->cur == 0) {                                   /* the 16-bit stamp wrapped: forget everything */
        memset(tc->stamp, 0, TC_MAX_TEXELS * sizeof(uint16_t));
        tc->cur = 1;
    }
    tc->w = ew; tc->h = eh;
}

/* texel_at() through the cache (see tc_current). (s, t) are tile-relative texel coordinates, already wrapped/clamped. */
/* The tile's cache, brought up to date: if anything that could change the decoded texels happened since it was filled, it is emptied. A
 * bilinear sample calls this ONCE and then fetches its three texels from the result (SPEED: the check used to run for every texel). */
static TexCache *tc_current(const Tile *tl)
{
    TexCache *tc = &sTc[tl - R.tile];
    uint32_t tlut_bits = (R.omh >> 14) & 3u;                /* tlut_en, tlut_type: they change what a palette texel means */
    if (!tc->valid || tc->epoch != sResets || tc->gen != R.tmem_gen || tc->ver != R.tile_ver[tl - R.tile] || tc->tlut != tlut_bits) tc_reset(tc, tl, tlut_bits);
    return tc;
}

/* The texel fetch proper; `tc` must come from tc_current(tl) with nothing changed since. Returns the texel as 0xAABBGGRR (bytes R, G, B, A in
 * memory order, the cache's own format). */
static uint32_t texel_px(TexCache *tc, const Tile *tl, int s, int t)
{
    uint8_t d[4];
    if ((unsigned)s < (unsigned)tc->w && (unsigned)t < (unsigned)tc->h) {
        size_t i = (size_t)t * (size_t)tc->w + (size_t)s;
        if (tc->stamp[i] != tc->cur) {
            texel_at(tl, s, t, d);
            tc->px[i] = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
            tc->stamp[i] = tc->cur;
        }
#ifdef PS_RDP_VERIFY
        {   /* development check (build with -DPS_RDP_VERIFY): the cached texel must equal a fresh decode */
            uint32_t v = tc->px[i];
            texel_at(tl, s, t, d);
            if (v != ((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24))) {
                fprintf(stderr, "kyujo: TEXTURE CACHE MISMATCH tile %d s %d t %d cached %08X\n", (int)(tl - R.tile), s, t, (unsigned)v);
                abort();
            }
        }
#endif
        return tc->px[i];
    }
    texel_at(tl, s, t, d);
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* One filtered texture sample. (s, t) are in texels of the whole texture, as the RDP's interpolators produce them. Returns 0xAABBGGRR. */
static uint32_t sample_tile_px(const Tile *tl, float s, float t)
{
    int sf = ifloor(s * 32.0f), tf = ifloor(t * 32.0f);             /* s10.5 */
    int sw = ((int)tl->sh - (int)tl->sl) / 4 + 1, th = ((int)tl->th - (int)tl->tl) / 4 + 1;
    TexCache *tc;

    /* shift (Set_Tile shift_s/t): 0 none, 1..10 shift right by n, 11..15 shift left by 16-n */
    if (tl->shift_s) sf = (tl->shift_s <= 10) ? (sf >> tl->shift_s) : (sf << (16 - tl->shift_s));
    if (tl->shift_t) tf = (tl->shift_t <= 10) ? (tf >> tl->shift_t) : (tf << (16 - tl->shift_t));
    sf -= (int)tl->sl << 3;                                         /* 10.2 -> 10.5 */
    tf -= (int)tl->tl << 3;
    tc = tc_current(tl);

    if (!OM_BILERP()) {
        return texel_px(tc, tl, wrap_coord(sf >> 5, tl->clamp_s, tl->mirror_s, tl->mask_s, sw),
                        wrap_coord(tf >> 5, tl->clamp_t, tl->mirror_t, tl->mask_t, th));
    }
    {
        /* Bilinear. FROM KNOWLEDGE: the RDP filters with three texels, not four (the "3-point" filter): the triangle (s,t),(s+1,t),(s,t+1) when
         * the fractions sum to less than 1, otherwise (s+1,t+1),(s,t+1),(s+1,t).
         * NO half-texel shift: texel i sits AT coordinate i, so an integer (s,t) returns exactly texel (s,t). Basis: Nintendo N64 Programming
         * Manual 14.1 (Example 14-8): the exact four-texel average needs the start point 0.5 ("set the start point to 0.5 and then step by 1
         * texel per pixel"), i.e. the point between texels 0 and 1 is 0.5, so texel centres are the integers. An earlier version subtracted
         * half a texel first (an OpenGL habit); a 1:1 text rectangle (s0 = 0, dsdx = 1.0) was then blended 50/50 with its left/upper
         * neighbour - the "fuzzy, horizontally misaligned" battle text the Windows tester reported. The manual does not state the rule for
         * the general bilinear case in words; this follows from the average-mode statement and is still marked in docs/rdp-notes.md.
         * SPEED: no clamp of the result. Both forms are a + ((b - a) * u + (c - a) * v) >> 5 with u, v >= 0 and u + v <= 32 (first form: u = fs,
         * v = ft, fs + ft < 32; second: u = 32 - fs, v = 32 - ft, and 64 - fs - ft <= 32 because fs + ft >= 32). That is
         * floor((a * (32 - u - v) + b * u + c * v) / 32), a floor of a weighted mean of three values in 0..255, so it lies in 0..255 already. */
        int s0 = sf >> 5, t0 = tf >> 5, fs = sf & 31, ft = tf & 31, u, v, k;
        int ws0 = wrap_coord(s0, tl->clamp_s, tl->mirror_s, tl->mask_s, sw), ws1 = wrap_coord(s0 + 1, tl->clamp_s, tl->mirror_s, tl->mask_s, sw);
        int wt0 = wrap_coord(t0, tl->clamp_t, tl->mirror_t, tl->mask_t, th), wt1 = wrap_coord(t0 + 1, tl->clamp_t, tl->mirror_t, tl->mask_t, th);
        uint32_t pa, pb, pc, r = 0;
        if (fs + ft < 32) {
            pa = texel_px(tc, tl, ws0, wt0); pb = texel_px(tc, tl, ws1, wt0); pc = texel_px(tc, tl, ws0, wt1);
            u = fs; v = ft;
        } else {
            pa = texel_px(tc, tl, ws1, wt1); pb = texel_px(tc, tl, ws0, wt1); pc = texel_px(tc, tl, ws1, wt0);
            u = 32 - fs; v = 32 - ft;
        }
        for (k = 0; k < 32; k += 8) {
            int a = (int)((pa >> k) & 255u), b = (int)((pb >> k) & 255u), c = (int)((pc >> k) & 255u);
            r |= (uint32_t)(uint8_t)(a + (((b - a) * u + (c - a) * v) >> 5)) << k;
        }
        return r;
    }
}

/* The same sample as four separate bytes (R, G, B, A). */
static void sample_tile(const Tile *tl, float s, float t, uint8_t out[4])
{
    uint32_t v = sample_tile_px(tl, s, t);
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16); out[3] = (uint8_t)(v >> 24);
}

/* ---- colour combiner and blender -------------------------------------------------------------------------------------------------- */

typedef struct PixIn {
    int shade[4];               /* 0..255 */
    int texel0[4], texel1[4];   /* 0..255 */
} PixIn;

/* ---- colour combiner ---------------------------------------------------------------------------------------------------------------
 * The combiner computes (A - B) * C + D per channel for one or two cycles (selector layouts: n64brew "Set_Combine_Mode", numbering of the
 * inputs checked against gbi.h by tests/native/render/rdp_gbi_check.c).
 * SPEED: the selectors used to be decoded and switched on for every channel of every pixel (profile: about a tenth of the whole run). They are now
 * resolved ONCE, when Set_Combine_Mode arrives, into indices into one flat table of input values `v[]` that draw_pixel fills per pixel. The
 * results are the same numbers as before (the tests cover five combiner modes).
 */
enum {                          /* layout of the flat input table v[] */
    V_COMB = 0,                 /* result of the previous cycle: R G B A (4 entries each below) */
    V_T0 = 4, V_T1 = 8, V_PRIM = 12, V_SHADE = 16, V_ENV = 20,
    V_ONE = 24,                 /* the constant 1.0 (256) */
    V_ZERO = 25, V_NOISE = 26, V_K4 = 27, V_K5 = 28, V_LOD_FRAC = 29, V_PRIM_LOD_FRAC = 30,
    V_COUNT = 31
};

/* The input index for RGB selector `sel` of operand `op` (0 = A, 1 = B, 2 = C, 3 = D), channel ch. Same tables as the old rgb_a..rgb_d. */
static int plan_rgb(int op, int sel, int ch)
{
    switch (op) {
    case 0:                     /* A */
        switch (sel) { case 0: return V_COMB + ch; case 1: return V_T0 + ch; case 2: return V_T1 + ch; case 3: return V_PRIM + ch;
                       case 4: return V_SHADE + ch; case 5: return V_ENV + ch; case 6: return V_ONE; case 7: return V_NOISE; default: return V_ZERO; }
    case 1:                     /* B (6 = key centre: chroma key is not modelled, reads 0) */
        switch (sel) { case 0: return V_COMB + ch; case 1: return V_T0 + ch; case 2: return V_T1 + ch; case 3: return V_PRIM + ch;
                       case 4: return V_SHADE + ch; case 5: return V_ENV + ch; case 7: return V_K4; default: return V_ZERO; }
    case 2:                     /* C (6 = key scale: not modelled, reads 0) */
        switch (sel) { case 0: return V_COMB + ch; case 1: return V_T0 + ch; case 2: return V_T1 + ch; case 3: return V_PRIM + ch;
                       case 4: return V_SHADE + ch; case 5: return V_ENV + ch; case 7: return V_COMB + 3; case 8: return V_T0 + 3;
                       case 9: return V_T1 + 3; case 10: return V_PRIM + 3; case 11: return V_SHADE + 3; case 12: return V_ENV + 3;
                       case 13: return V_LOD_FRAC; case 14: return V_PRIM_LOD_FRAC; case 15: return V_K5; default: return V_ZERO; }
    default:                    /* D */
        switch (sel) { case 0: return V_COMB + ch; case 1: return V_T0 + ch; case 2: return V_T1 + ch; case 3: return V_PRIM + ch;
                       case 4: return V_SHADE + ch; case 5: return V_ENV + ch; case 6: return V_ONE; default: return V_ZERO; }
    }
}
/* Alpha selectors a, b, d share one list; c has its own (gbi.h G_ACMUX_*). */
static int plan_alpha(int op, int sel)
{
    if (op == 2) {
        switch (sel) { case 0: return V_LOD_FRAC; case 1: return V_T0 + 3; case 2: return V_T1 + 3; case 3: return V_PRIM + 3;
                       case 4: return V_SHADE + 3; case 5: return V_ENV + 3; case 6: return V_PRIM_LOD_FRAC; default: return V_ZERO; }
    }
    switch (sel) { case 0: return V_COMB + 3; case 1: return V_T0 + 3; case 2: return V_T1 + 3; case 3: return V_PRIM + 3;
                   case 4: return V_SHADE + 3; case 5: return V_ENV + 3; case 6: return V_ONE; default: return V_ZERO; }
}

static void comb_plan_build(CombPlan *pl, uint32_t hi, uint32_t lo)
{
    int sel[2][2][4];           /* [cycle][rgb=0 / alpha=1][A, B, C, D] */
    int cy, ch, op;
    /* Layout of Set_Combine_Mode (n64brew): hi = bits 55:32, lo = bits 31:0. Checked against gbi.h by tests/native/render/rdp_gbi_check.c. */
    sel[0][0][0] = (hi >> 20) & 0xF;  sel[0][0][2] = (hi >> 15) & 0x1F; sel[0][1][0] = (hi >> 12) & 7; sel[0][1][2] = (hi >> 9) & 7;
    sel[1][0][0] = (hi >> 5) & 0xF;   sel[1][0][2] = hi & 0x1F;
    sel[0][0][1] = (lo >> 28) & 0xF;  sel[1][0][1] = (lo >> 24) & 0xF;
    sel[1][1][0] = (lo >> 21) & 7;    sel[1][1][2] = (lo >> 18) & 7;
    sel[0][0][3] = (lo >> 15) & 7;    sel[0][1][1] = (lo >> 12) & 7; sel[0][1][3] = (lo >> 9) & 7;
    sel[1][0][3] = (lo >> 6) & 7;     sel[1][1][1] = (lo >> 3) & 7;  sel[1][1][3] = lo & 7;
    for (cy = 0; cy < 2; cy++) {
        for (op = 0; op < 4; op++) {
            for (ch = 0; ch < 3; ch++) pl->idx[cy][op][ch] = (uint8_t)plan_rgb(op, sel[cy][0][op], ch);
            pl->idx[cy][op][3] = (uint8_t)plan_alpha(op, sel[cy][1][op]);
        }
    }
    for (cy = 0; cy < 2; cy++) {
        pl->uses_t[cy][0] = pl->uses_t[cy][1] = 0;
        for (op = 0; op < 4; op++) {
            for (ch = 0; ch < 4; ch++) {
                int ix = pl->idx[cy][op][ch];
                if (ix >= V_T0 && ix < V_T0 + 4) pl->uses_t[cy][0] = 1;
                if (ix >= V_T1 && ix < V_T1 + 4) pl->uses_t[cy][1] = 1;
            }
        }
    }
}

/* (A - B) * C + D, with the hardware's rounding: the product is rounded to 8 fractional bits, the result clamped to 0..255. */
static int comb_eq(int a, int b, int c, int d)
{
    return iclamp((((a - b) * c + 0x80) >> 8) + d, 0, 255);
}

static void comb_cycle(const CombPlan *pl, int cycle, const int *v, int out[4])
{
    int ch;
    for (ch = 0; ch < 4; ch++) {
        out[ch] = comb_eq(v[pl->idx[cycle][0][ch]], v[pl->idx[cycle][1][ch]], v[pl->idx[cycle][2][ch]], v[pl->idx[cycle][3][ch]]);
    }
}

/* ---- frame buffer ------------------------------------------------------------------------------------------------------------------ */

static void fb_read(int x, int y, int rgba[4])
{
    uint8_t *p;
    if (R.cimg.siz == SIZ_16) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 2u, 2);
        if (p) {
            unsigned v = rd16(p);
            rgba[0] = c5to8((v >> 11) & 31); rgba[1] = c5to8((v >> 6) & 31); rgba[2] = c5to8((v >> 1) & 31); rgba[3] = (v & 1) ? 255 : 0;
            return;
        }
    } else if (R.cimg.siz == SIZ_32) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 4u, 4);
        if (p) {
            rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
            return;
        }
    }
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
}

static void fb_write(int x, int y, int r, int g, int b)
{
    uint8_t *p;
    if (R.cimg.siz == SIZ_16) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 2u, 2);
        if (p) {
            /* 5551: the low bit holds the coverage's top bit, which is 1 for the fully covered pixels this model produces. */
            wr16(p, (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1));
        }
    } else if (R.cimg.siz == SIZ_32) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 4u, 4);
        if (p) {
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 0xE0;       /* coverage 7 in the top 3 bits (FROM KNOWLEDGE) */
        }
    }
}

/* Depth buffer: 16 bit per pixel, depth in bits 15:2 (see "Deviations" 3). z14 is 0 (near) .. 0x3FFF (far). */
static int zb_read(int x, int y)
{
    uint8_t *p = ram(R.zimg + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 2u, 2);
    return p ? (rd16(p) >> 2) : 0x3FFF;
}
static void zb_write(int x, int y, int z14)
{
    uint8_t *p = ram(R.zimg + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 2u, 2);
    if (p) wr16(p, (uint16_t)(z14 << 2));
}

/* SPEED: the blender divides by (a + b) for every colour channel of every blended pixel. An integer division is slow (tens of cycles on the
 * 32-bit x86 this port targets), so n / d is computed as (n * m) >> 32 with m = ceil(2^32 / d). Why that is exact: write m = (2^32 + e) / d
 * with 0 <= e < d. Then n * m / 2^32 = n / d + n * e / (d * 2^32), and the extra term is smaller than n / 2^32, which is below 1 / d when
 * n * d < 2^32. A fraction n / d that is not a whole number is at least 1 / d below the next integer, so the extra term cannot carry it over,
 * and flooring gives the same result as the real division. Here n <= 255 * 510 + 255 and d <= 510, so n * d < 2^27. Not trusted on that argument alone:
 * tests/native/render/test_blend_div.c compares every (n, d) of that range with the real division. d = 1 is handled apart (2^32 does not
 * fit in 32 bits). */
static uint32_t sDivRecip[511];
static int sDivReady;
static void div_tables(void)
{
    uint32_t d;
    for (d = 2; d <= 510; d++) sDivRecip[d] = (uint32_t)(((1ULL << 32) + d - 1) / d);
    sDivReady = 1;
}
static int blend_div(int n, int d)
{
    if (d == 1) return n;
    return (int)(((uint64_t)(uint32_t)n * sDivRecip[d]) >> 32);
}

/* The blender: P, A, M, B of one cycle. Returns the blended colour; `a_in` is the pixel's alpha for the A selector. */
static void blend_cycle(int cyc, const int *p_color, int a_in, int shade_a, const int *mem, int out[3])
{
    int m1a, m1b, m2a, m2b;
    int pr[3], mr[3], a, b, k;
    /* bit positions in Set_Other_Modes' low word (n64brew): m1a 31:30 / 29:28, m1b 27:26 / 25:24, m2a 23:22 / 21:20, m2b 19:18 / 17:16 for
     * cycle 0 / cycle 1. gbi.h's GBL_c1 / GBL_c2 pack the same way. */
    m1a = (R.oml >> (cyc == 0 ? 30 : 28)) & 3;
    m1b = (R.oml >> (cyc == 0 ? 26 : 24)) & 3;
    m2a = (R.oml >> (cyc == 0 ? 22 : 20)) & 3;
    m2b = (R.oml >> (cyc == 0 ? 18 : 16)) & 3;
    /* SPEED: the selector is switched on once per cycle, not once per channel. */
    switch (m1a) {
    case 0:  pr[0] = p_color[0]; pr[1] = p_color[1]; pr[2] = p_color[2]; break;
    case 1:  pr[0] = mem[0]; pr[1] = mem[1]; pr[2] = mem[2]; break;
    case 2:  pr[0] = R.blend[0]; pr[1] = R.blend[1]; pr[2] = R.blend[2]; break;
    default: pr[0] = R.fog[0]; pr[1] = R.fog[1]; pr[2] = R.fog[2]; break;
    }
    switch (m2a) {
    case 0:  mr[0] = p_color[0]; mr[1] = p_color[1]; mr[2] = p_color[2]; break;
    case 1:  mr[0] = mem[0]; mr[1] = mem[1]; mr[2] = mem[2]; break;
    case 2:  mr[0] = R.blend[0]; mr[1] = R.blend[1]; mr[2] = R.blend[2]; break;
    default: mr[0] = R.fog[0]; mr[1] = R.fog[1]; mr[2] = R.fog[2]; break;
    }
    switch (m1b) { case 0: a = a_in; break; case 1: a = R.fog[3]; break; case 2: a = shade_a; break; default: a = 0; break; }
    switch (m2b) { case 0: b = 255 - a; break; case 1: b = mem[3]; break; case 2: b = 255; break; default: b = 0; break; }
    a = iclamp(a, 0, 255);
    b = iclamp(b, 0, 255);
    if (a + b == 0) {
        out[0] = pr[0]; out[1] = pr[1]; out[2] = pr[2];
        return;
    }
    if (!sDivReady) div_tables();
    for (k = 0; k < 3; k++) {
        out[k] = iclamp(blend_div(pr[k] * a + mr[k] * b + (a + b) / 2, a + b), 0, 255);
    }
}

/* Everything that happens to one pixel after the rasteriser has found it: texture, combiner, alpha test, z test, blender, write. */
typedef struct PixelArgs {
    int x, y;
    int z14, dz14;              /* depth (0..0x3FFF) and its screen-space slope */
    PixIn in;
    int have_z;
} PixelArgs;

static void draw_pixel(const PixelArgs *pa)
{
    int v[V_COUNT];              /* the combiner's input values for this pixel (layout: V_* above) */
    int comb[4] = {0, 0, 0, 0};
    int cyc = (int)OM_CYCLE();
    int rgb[3], a_pix, k;

    R.noise = R.noise * 1103515245u + 12345u;                /* a plain generator; the hardware's noise is not documented */

    for (k = 0; k < 4; k++) {
        v[V_COMB + k] = 0;
        v[V_T0 + k] = pa->in.texel0[k];
        v[V_T1 + k] = pa->in.texel1[k];
        v[V_PRIM + k] = R.prim[k];
        v[V_SHADE + k] = pa->in.shade[k];
        v[V_ENV + k] = R.env[k];
    }
    v[V_ONE] = 256; v[V_ZERO] = 0; v[V_NOISE] = (int)((R.noise >> 16) & 0xFF); v[V_K4] = R.k4; v[V_K5] = R.k5;
    v[V_LOD_FRAC] = 0; v[V_PRIM_LOD_FRAC] = R.prim_lod_frac;

    comb_cycle(&R.comb_plan, 0, v, comb);
    if (cyc == CYC_2) {
        /* Second cycle: TEXEL0 is what the first cycle read as TEXEL1 and vice versa (fast64: "2-cycle, 2nd cycle"); COMBINED is cycle 1's
         * result. */
        for (k = 0; k < 4; k++) {
            v[V_COMB + k] = comb[k];
            v[V_T0 + k] = pa->in.texel1[k];
            v[V_T1 + k] = pa->in.texel0[k];
        }
        comb_cycle(&R.comb_plan, 1, v, comb);
    }

    /* alpha compare */
    if (OM_ALPHA_COMPARE()) {
        int thr = OM_DITHER_ALPHA() ? (int)((R.noise >> 8) & 0xFF) : R.blend[3];
        if (comb[3] < thr) {
            return;
        }
    }
    /* Coverage: with a full pixel it is 8 (of 8); cvg_x_alpha scales it by the alpha, and a pixel without any coverage is not drawn. */
    a_pix = comb[3];
    if (OM_CVG_X_ALPHA() && a_pix < 16) {
        return;                                              /* alpha < 1/16 of full: coverage (alpha * 8 / 256) rounds to 0 */
    }
    if (OM_ALPHA_CVG_SEL()) {
        a_pix = OM_CVG_X_ALPHA() ? comb[3] : 255;            /* the blender's alpha is the coverage */
    }

    /* z test */
    if (pa->have_z && OM_Z_COMPARE()) {
        int mem_z = zb_read(pa->x, pa->y), z = pa->z14, ok, dz = pa->dz14;
        int infront = z < mem_z, farther = (z + dz) >= mem_z, nearer = (z - dz) <= mem_z, max = (mem_z == 0x3FFF);
        switch (OM_Z_MODE()) {
        case 0: case 1: ok = max || infront; break;           /* opaque, interpenetrating (overflow case: memory coverage 7 + pixel coverage >= 1) */
        case 2: ok = infront || max; break;                   /* translucent */
        default: ok = farther && nearer && !max; break;       /* decal */
        }
        if (!ok) {
            return;
        }
    }

    /* blender */
    {
        int mem[4];
        if (OM_FORCE_BLEND()) {
            fb_read(pa->x, pa->y, mem);
            if (cyc == CYC_2) {
                int mid[3];
                blend_cycle(0, comb, a_pix, pa->in.shade[3], mem, mid);
                blend_cycle(1, mid, a_pix, pa->in.shade[3], mem, rgb);
            } else {
                blend_cycle(0, comb, a_pix, pa->in.shade[3], mem, rgb);
            }
        } else if (cyc == CYC_2) {
            /* no blending: the pipeline outputs the (second-cycle) combiner colour unchanged */
            rgb[0] = comb[0]; rgb[1] = comb[1]; rgb[2] = comb[2];
        } else {
            rgb[0] = comb[0]; rgb[1] = comb[1]; rgb[2] = comb[2];
        }
    }
    fb_write(pa->x, pa->y, rgb[0], rgb[1], rgb[2]);
    if (pa->have_z && OM_Z_UPDATE()) {
        zb_write(pa->x, pa->y, pa->z14);
    }
    R.n_pix++;
}

/* ---- development profile of the primitives (KYUJO_RDP_PROFILE=<min ms>) -----------------------------------------------------------
 * WHY: a menu frame took 31 ms on the Windows tester's PC; the sampling profiler says WHERE in the code, this says WHICH drawing commands
 * (triangles / texture rectangles / fill rectangles, in which cycle mode, with or without bilinear filter and texture) cost the time.
 * At the end of every display list that took at least <min ms> a table is written to stderr: one line per kind with the number of
 * commands, the pixels written, and the time. Off (one flag test per command) unless the variable is set.
 */
enum { PK_TRI, PK_RECT, PK_FILL, PK_KINDS };
static const char *const sPkName[PK_KINDS] = { "triangle", "texrect", "fillrect" };
static struct { unsigned n; uint64_t pix, ns; } sPk[PK_KINDS][3][2][2];       /* kind, cycle type (0 = 1 cycle, 1 = 2 cycles, 2 = fill/copy), bilinear, texture on */
static int sPkMs = -1;                                                         /* -1 = not read yet, 0 = off */
static int pk_on(void)
{
    if (sPkMs < 0) { const char *e = getenv("KYUJO_RDP_PROFILE"); sPkMs = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0; }
    return sPkMs;
}
static void pk_add(int kind, int has_tex, unsigned long long t0, uint64_t pix0)
{
    int cyc = (int)OM_CYCLE(), c = cyc == CYC_1 ? 0 : (cyc == CYC_2 ? 1 : 2), b = (int)OM_BILERP();
    sPk[kind][c][b][has_tex ? 1 : 0].n++;
    sPk[kind][c][b][has_tex ? 1 : 0].pix += R.n_pix - pix0;
    sPk[kind][c][b][has_tex ? 1 : 0].ns += host_now_ns() - t0;
}
/* Called by the display-list interpreter after a task; prints and clears the table. `task_ns` = duration of the whole task. */
void rdp_profile_report(unsigned long long task_ns)
{
    int k, c, b, t;
    if (!pk_on()) return;
    if (task_ns / 1000000ULL >= (unsigned long long)sPkMs) {
        fprintf(stderr, "rdp-profile: display list %.1f ms\n", (double)task_ns / 1e6);
        for (k = 0; k < PK_KINDS; k++) for (c = 0; c < 3; c++) for (b = 0; b < 2; b++) for (t = 0; t < 2; t++) {
            if (!sPk[k][c][b][t].n) continue;
            fprintf(stderr, "rdp-profile:   %-8s %s bilerp=%d tex=%d: %5u cmds %8llu pixels %7.2f ms (%.0f ns/pixel)\n", sPkName[k],
                    c == 0 ? "1cyc" : (c == 1 ? "2cyc" : "fill"), b, t, sPk[k][c][b][t].n, (unsigned long long)sPk[k][c][b][t].pix,
                    (double)sPk[k][c][b][t].ns / 1e6, sPk[k][c][b][t].pix ? (double)sPk[k][c][b][t].ns / (double)sPk[k][c][b][t].pix : 0.0);
        }
    }
    memset(sPk, 0, sizeof sPk);
}

/* ---- fill and copy ----------------------------------------------------------------------------------------------------------------- */

static int in_scissor(int x, int y)
{
    return x >= (R.scis_xh >> 2) && x < ((R.scis_xl + 3) >> 2) && y >= (R.scis_yh >> 2) && y < ((R.scis_yl + 3) >> 2);
}

/* In fill mode a pixel is written with the fill colour: for a 16-bit image the 32-bit register holds two pixels, chosen by x parity. */
static void fill_pixel(int x, int y)
{
    uint8_t *p;
    if (R.cimg.siz == SIZ_16) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 2u, 2);
        if (p) wr16(p, (uint16_t)((x & 1) ? (R.fill_color & 0xFFFF) : (R.fill_color >> 16)));
    } else if (R.cimg.siz == SIZ_32) {
        p = ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x) * 4u, 4);
        if (p) { p[0] = (uint8_t)(R.fill_color >> 24); p[1] = (uint8_t)(R.fill_color >> 16); p[2] = (uint8_t)(R.fill_color >> 8); p[3] = (uint8_t)R.fill_color; }
    }
    R.n_pix++;
}

static void fill_rect_impl(uint32_t hi, uint32_t lo)
{
    int lrx = (hi >> 12) & 0xFFF, lry = hi & 0xFFF, ulx = (lo >> 12) & 0xFFF, uly = lo & 0xFFF;
    int x0 = ulx >> 2, y0 = uly >> 2, x1, y1, x, y, cyc = (int)OM_CYCLE();

    /* In fill and copy mode the lower-right corner is inclusive; in 1/2-cycle mode it is exclusive. FROM KNOWLEDGE (n64brew prose). */
    if (cyc == CYC_FILL || cyc == CYC_COPY) { x1 = (lrx >> 2) + 1; y1 = (lry >> 2) + 1; } else { x1 = lrx >> 2; y1 = lry >> 2; }
    R.n_rect++;
    x0 = imax(x0, R.scis_xh >> 2); y0 = imax(y0, R.scis_yh >> 2);
    x1 = imin(x1, (R.scis_xl + 3) >> 2); y1 = imin(y1, (R.scis_yl + 3) >> 2);
    if (cyc == CYC_FILL) {
        /* SPEED: a row that lies completely inside RDRAM is written in one go (one range check per row instead of one per pixel); any other row
         * takes the per-pixel path, which skips the pixels outside RDRAM exactly as before. Same bytes, same pixel count. */
        for (y = y0; y < y1; y++) {
            uint32_t bpp = R.cimg.siz == SIZ_32 ? 4u : 2u;
            uint8_t *row = (x1 > x0 && (R.cimg.siz == SIZ_16 || R.cimg.siz == SIZ_32))
                ? ram(R.cimg.addr + ((uint32_t)y * R.cimg.width + (uint32_t)x0) * bpp, (uint32_t)(x1 - x0) * bpp) : NULL;
            if (!row) { for (x = x0; x < x1; x++) fill_pixel(x, y); continue; }
            if (bpp == 2u) {
                for (x = x0; x < x1; x++, row += 2) wr16(row, (uint16_t)((x & 1) ? (R.fill_color & 0xFFFF) : (R.fill_color >> 16)));
            } else {
                for (x = x0; x < x1; x++, row += 4) { row[0] = (uint8_t)(R.fill_color >> 24); row[1] = (uint8_t)(R.fill_color >> 16); row[2] = (uint8_t)(R.fill_color >> 8); row[3] = (uint8_t)R.fill_color; }
            }
            R.n_pix += (uint64_t)(x1 - x0);
        }
    } else {
        /* A filled rectangle in 1/2-cycle mode is a flat-coloured primitive: blend colour comes from the combiner (no texture, no shade). */
        PixelArgs pa;
        memset(&pa, 0, sizeof pa);
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) { pa.x = x; pa.y = y; draw_pixel(&pa); }
        }
    }
}

/* ---- texture lookups for a pixel --------------------------------------------------------------------------------------------------- */

/* Fills TEXEL0 / TEXEL1 of `in` for texture coordinate (s, t). In 2-cycle mode TEXEL1 comes from the next tile (n64brew / fast64:
 * "TEXEL1 = tex1" in the first cycle); in 1-cycle mode the hardware's TEXEL1 is undefined, so it is made equal to TEXEL0. */
static void fetch_texels(int tile, float s, float t, PixIn *in)
{
    /* SPEED: a sample is only taken when the combiner reads it (CombPlan.uses_t). In 2-cycle mode the second cycle sees the two inputs
     * swapped (see draw_pixel), so cycle 1's TEXEL0 is the sample of tile + 1 and its TEXEL1 the sample of tile. An input nobody reads stays
     * zero (the caller clears PixIn); draw_pixel only ever reads what the plan names, so the picture is unchanged. */
    int cyc = (int)OM_CYCLE();
    const CombPlan *pl = &R.comb_plan;
    if (cyc == CYC_2) {
        if (pl->uses_t[0][0] | pl->uses_t[1][1]) {
            uint32_t a = sample_tile_px(&R.tile[tile & 7], s, t);
            in->texel0[0] = (int)(a & 255u); in->texel0[1] = (int)((a >> 8) & 255u); in->texel0[2] = (int)((a >> 16) & 255u); in->texel0[3] = (int)(a >> 24);
        }
        if (pl->uses_t[0][1] | pl->uses_t[1][0]) {
            uint32_t b = sample_tile_px(&R.tile[(tile + 1) & 7], s, t);
            in->texel1[0] = (int)(b & 255u); in->texel1[1] = (int)((b >> 8) & 255u); in->texel1[2] = (int)((b >> 16) & 255u); in->texel1[3] = (int)(b >> 24);
        }
    } else if (cyc != CYC_1 || (pl->uses_t[0][0] | pl->uses_t[0][1])) {     /* copy mode reads texel0 directly; 1 cycle: TEXEL1 = TEXEL0 */
        uint32_t a = sample_tile_px(&R.tile[tile & 7], s, t);
        in->texel0[0] = in->texel1[0] = (int)(a & 255u); in->texel0[1] = in->texel1[1] = (int)((a >> 8) & 255u);
        in->texel0[2] = in->texel1[2] = (int)((a >> 16) & 255u); in->texel0[3] = in->texel1[3] = (int)(a >> 24);
    }
    if (OM_LOD_EN()) unimpl(UN_LOD);
    if (OM_DETAIL() || OM_SHARPEN()) unimpl(UN_DETAIL_SHARPEN);
    if (OM_KEY()) unimpl(UN_CHROMA_KEY);
}

/* ---- texture rectangle ------------------------------------------------------------------------------------------------------------- */

static void texture_rectangle_impl(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip)
{
    int lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF, tile = (w1 >> 24) & 7, ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
    int s0 = (int16_t)(half1 >> 16), t0 = (int16_t)half1, dsdx = (int16_t)(half2 >> 16), dtdy = (int16_t)half2;
    int cyc = (int)OM_CYCLE(), xs = ulx >> 2, ys = uly >> 2, x0 = xs, y0 = ys, x1, y1, x, y;
    float ds = (float)dsdx / 1024.0f, dt = (float)dtdy / 1024.0f, sbase = (float)s0 / 32.0f, tbase = (float)t0 / 32.0f;
    uint32_t omh_saved = R.omh;

    R.n_rect++;
    if (trace_active()) {
        const Tile *t = &R.tile[tile];
        fprintf(stderr, "rdp: texrect ul=(%d,%d) lr=(%d,%d) tile %d s0=%d t0=%d dsdx=%d dtdy=%d cyc=%d bilerp=%d omh=%08X | tile fmt%d siz%d line%d tmem%d clamp%d%d mask%d,%d sl%d tl%d sh%d th%d | cimg %08X w%d siz%d scis %d %d %d %d\n",
                ulx >> 2, uly >> 2, lrx >> 2, lry >> 2, tile, s0, t0, dsdx, dtdy, cyc, (int)OM_BILERP(), (unsigned)R.omh, t->fmt, t->siz, t->line, t->tmem, t->clamp_s, t->clamp_t, t->mask_s, t->mask_t,
                t->sl, t->tl, t->sh, t->th, R.cimg.addr, R.cimg.width, R.cimg.siz, R.scis_xh, R.scis_yh, R.scis_xl, R.scis_yl);
    }
    if (trace_active() && env_set("KYUJO_RDP_TEXELS", &sEnvTexels)) {
        int ty, tx;
        for (ty = 0; ty < 16; ty += 5) {
            for (tx = 0; tx < 128; tx += 8) {
                uint8_t px[4];
                sample_tile(&R.tile[tile], (float)tx, (float)ty, px);
                fprintf(stderr, "%02X%02X%02X%02X ", px[0], px[1], px[2], px[3]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (env_set("KYUJO_TILE_DUMP", &sEnvTileDump)) {                       /* development: writes the sampler's view of the first IA8 tiles as .pgm (I | A) */
        static int shown;
        if (shown < 4 && R.tile[tile].fmt == 3 && R.tile[tile].siz == 1 && R.tile[tile].line == 3) {
            char name[512];
            FILE *f;
            int ty, tx, w = (R.tile[tile].sh >> 2) + 1, h = (R.tile[tile].th >> 2) + 1;
            {   /* the texels as they lie in memory (the load source), 24 per row, alpha nibble */
                int rr, cc;
                const uint8_t *raw = ram(R.timg.addr, 24 * 24);
                fprintf(stderr, "source at %08X (timg width %u):\n", R.timg.addr, R.timg.width);
                for (rr = 0; raw && rr < h; rr++) {
                    for (cc = 0; cc < w; cc++) fputc(" .:-=+*#%@"[(raw[rr * w + cc] & 15) * 9 / 15], stderr);
                    fputc('\n', stderr);
                }
            }
            {
                const uint8_t *big = ram(R.timg.addr - 576, 576 * 3);
                FILE *g;
                snprintf(name, sizeof name, "%s/src%d.bin", getenv("KYUJO_TILE_DUMP"), shown);
                g = big ? fopen(name, "wb") : NULL;
                if (g) { fwrite(big, 1, 576 * 3, g); fclose(g); }
            }
            snprintf(name, sizeof name, "%s/tile%d.pgm", getenv("KYUJO_TILE_DUMP"), shown++);
            f = fopen(name, "wb");
            if (f) {
                fprintf(f, "P5 %d %d 255\n", w * 2, h);
                for (ty = 0; ty < h; ty++) {
                    for (tx = 0; tx < w * 2; tx++) {
                        uint8_t px[4];
                        sample_tile(&R.tile[tile], (float)(tx % w), (float)ty, px);
                        fputc(tx < w ? px[0] : px[3], f);
                    }
                }
                fclose(f);
            }
        }
    }
    if (cyc == CYC_FILL) {
        return;                                                  /* the hardware does not draw texture rectangles in fill mode */
    }
    /* Lower-right corner: inclusive in copy mode (FROM KNOWLEDGE), exclusive otherwise. */
    if (cyc == CYC_COPY) { x1 = (lrx >> 2) + 1; y1 = (lry >> 2) + 1; } else { x1 = lrx >> 2; y1 = lry >> 2; }
    /* s, t are s10.5 texel coordinates of the upper-left corner; dsdx, dtdy are s5.10 texels per pixel. In copy mode the hardware moves four
     * pixels per step, so the game gives dsdx four times too large (FROM KNOWLEDGE, libultra man page of gSPTextureRectangle). */
    if (cyc == CYC_COPY) {
        ds /= 4.0f;
        R.omh &= ~(1u << 13);                                    /* copy mode always point-samples */
    }
    x0 = imax(x0, R.scis_xh >> 2); y0 = imax(y0, R.scis_yh >> 2);
    x1 = imin(x1, (R.scis_xl + 3) >> 2); y1 = imin(y1, (R.scis_yl + 3) >> 2);

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float u, v;
            PixelArgs pa;
            memset(&pa, 0, sizeof pa);
            if (!flip) { u = sbase + ds * (float)(x - xs); v = tbase + dt * (float)(y - ys); }
            else       { u = sbase + ds * (float)(y - ys); v = tbase + dt * (float)(x - xs); }   /* flipped: s runs down, t runs across */
            pa.x = x; pa.y = y;
            fetch_texels(tile, u, v, &pa.in);
            if (cyc == CYC_COPY) {
                /* Copy mode bypasses the combiner and blender: the texel is written as it is, after the alpha test. */
                /* Alpha test in copy mode. On the hardware (as paraLLEl-RDP/ares implement it) a 16-bit RGBA texel is skipped when its single
                 * alpha bit is 0 and alpha compare is on; the blend colour's alpha is NOT consulted. Comparing with blend alpha instead let
                 * transparent texels through whenever the game left that at 0 (white boxes behind icons and the pointer). Other formats keep
                 * the compare against the blend alpha (not verified against hardware). */
                if (OM_ALPHA_COMPARE()) {
                    if (R.tile[tile & 7].fmt == FMT_RGBA && R.tile[tile & 7].siz == SIZ_16) { if (pa.in.texel0[3] == 0) continue; }
                    else if (pa.in.texel0[3] < R.blend[3]) continue;
                }
                fb_write(x, y, pa.in.texel0[0], pa.in.texel0[1], pa.in.texel0[2]);
                R.n_pix++;
            } else {
                if (OM_Z_SOURCE_PRIM()) { pa.have_z = 1; pa.z14 = R.prim_z >> 2; pa.dz14 = 0; }
                draw_pixel(&pa);
            }
        }
    }
    R.omh = omh_saved;
}

/* ---- triangles ---------------------------------------------------------------------------------------------------------------------- */

typedef struct FxVert { int64_t x, y; } FxVert;          /* 24.8 fixed point pixel coordinates */

static int64_t edge_fn(const FxVert *a, const FxVert *b, int64_t px, int64_t py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Fill rule: a pixel centre exactly on an edge belongs to the triangle only for a top edge or a left edge (y grows downwards, the
 * triangle's vertices are ordered so that the interior is on the positive side of every edge function). */
static int edge_owns_tie(const FxVert *a, const FxVert *b)
{
    int64_t dx = b->x - a->x, dy = b->y - a->y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static int fx_ok(float v) { return v > -100000.0f && v < 100000.0f; }       /* also false for NaN */

static void triangle_impl(const RdpVert *va, const RdpVert *vb, const RdpVert *vc, int tile, int level, int has_shade, int has_tex, int has_z)
{
    const RdpVert *v[3];
    FxVert f[3];
    int64_t area;
    int x0, x1, y0, y1, x, y, cyc = (int)OM_CYCLE(), i, use_persp = OM_PERSP();
    float inv_area, dzdx, dzdy;
    (void)level;

    R.n_tri++;
    v[0] = va; v[1] = vb; v[2] = vc;
    for (i = 0; i < 3; i++) {
        if (!fx_ok(v[i]->x) || !fx_ok(v[i]->y)) return;
        f[i].x = (int64_t)(v[i]->x * 256.0f + (v[i]->x < 0 ? -0.5f : 0.5f));
        f[i].y = (int64_t)(v[i]->y * 256.0f + (v[i]->y < 0 ? -0.5f : 0.5f));
    }
    area = edge_fn(&f[0], &f[1], f[2].x, f[2].y);
    if (area == 0) return;
    if (area < 0) {                                              /* make the interior the positive side of every edge */
        const RdpVert *tv = v[1]; FxVert tf = f[1];
        v[1] = v[2]; f[1] = f[2]; v[2] = tv; f[2] = tf;
        area = -area;
    }
    inv_area = 1.0f / (float)area;

    x0 = imax((int)((imin((int)(f[0].x >> 8), imin((int)(f[1].x >> 8), (int)(f[2].x >> 8)))) - 1), R.scis_xh >> 2);
    x1 = imin(imax((int)(f[0].x >> 8), imax((int)(f[1].x >> 8), (int)(f[2].x >> 8))) + 2, (R.scis_xl + 3) >> 2);
    y0 = imax((int)((imin((int)(f[0].y >> 8), imin((int)(f[1].y >> 8), (int)(f[2].y >> 8)))) - 1), R.scis_yh >> 2);
    y1 = imin(imax((int)(f[0].y >> 8), imax((int)(f[1].y >> 8), (int)(f[2].y >> 8))) + 2, (R.scis_yl + 3) >> 2);
    if (x0 >= x1 || y0 >= y1) return;
    {
        /* depth gradient (z14 units per pixel), used for the decal comparison */
        float z0 = v[0]->z * 16.0f, z1 = v[1]->z * 16.0f, z2 = v[2]->z * 16.0f;
        float ax = v[1]->x - v[0]->x, ay = v[1]->y - v[0]->y, bx = v[2]->x - v[0]->x, by = v[2]->y - v[0]->y;
        float det = ax * by - bx * ay;
        if (det != 0.0f) {
            dzdx = ((z1 - z0) * by - (z2 - z0) * ay) / det;
            dzdy = ((z2 - z0) * ax - (z1 - z0) * bx) / det;
        } else {
            dzdx = dzdy = 0.0f;
        }
    }
    if (OM_CYCLE() == CYC_COPY) {
        return;                                                  /* the hardware cannot draw triangles in copy mode */
    }

    /* The three edge functions are evaluated exactly (integers) at the first pixel of each row and then stepped by a constant per pixel:
     * edge_fn(px + 256, py) - edge_fn(px, py) = -(b.y - a.y) * 256. Same values as evaluating each pixel from scratch, without the 64-bit
     * multiplications (they are slow on a 32-bit x86). */
    const int64_t step0 = -(f[2].y - f[1].y) * 256, step1 = -(f[0].y - f[2].y) * 256, step2 = -(f[1].y - f[0].y) * 256;
    for (y = y0; y < y1; y++) {
        int64_t row_px = ((int64_t)x0 << 8) + 128, py = ((int64_t)y << 8) + 128;
        int64_t e0 = edge_fn(&f[1], &f[2], row_px, py), e1 = edge_fn(&f[2], &f[0], row_px, py), e2 = edge_fn(&f[0], &f[1], row_px, py);
        int xa = x0, xb = x1;                                    /* the pixels of this row that can be inside: [xa, xb) */
        {
            /* SPEED: the three edge values change by a constant per pixel, so the pixels where one of them is NEGATIVE (certainly outside) form
             * a prefix and/or a suffix of the row and can be skipped without testing. Only pixels with a strictly negative edge value are
             * skipped, so the pixels that remain, and what happens to them (including the tie rules below), are exactly as before. */
            const int64_t ev[3] = { e0, e1, e2 }, st[3] = { step0, step1, step2 };
            int64_t jlo = 0, jhi = (int64_t)(x1 - x0) - 1;       /* offsets from x0 */
            int i3;
            for (i3 = 0; i3 < 3; i3++) {
                if (st[i3] > 0) {
                    if (ev[i3] < 0) { int64_t need = (-ev[i3] + st[i3] - 1) / st[i3]; if (need > jlo) jlo = need; }       /* first offset with e >= 0 */
                } else if (st[i3] < 0) {
                    if (ev[i3] < 0) { jhi = -1; } else { int64_t last = ev[i3] / (-st[i3]); if (last < jhi) jhi = last; }  /* last offset with e >= 0 */
                } else if (ev[i3] < 0) {
                    jhi = -1;                                    /* constant and negative: no pixel of this row */
                }
            }
            if (jlo > jhi) continue;
            xa = x0 + (int)jlo; xb = x0 + (int)jhi + 1;
            e0 += step0 * jlo; e1 += step1 * jlo; e2 += step2 * jlo;
        }
        for (x = xa; x < xb; x++, e0 += step0, e1 += step1, e2 += step2) {
            float w0, w1, w2;
            PixelArgs pa;
#ifdef PS_RDP_VERIFY
            {   /* development check (build with -DPS_RDP_VERIFY): the stepped edge values must equal a from-scratch evaluation */
                int64_t qx = ((int64_t)x << 8) + 128, qy = ((int64_t)y << 8) + 128;
                if (e0 != edge_fn(&f[1], &f[2], qx, qy) || e1 != edge_fn(&f[2], &f[0], qx, qy) || e2 != edge_fn(&f[0], &f[1], qx, qy)) {
                    fprintf(stderr, "kyujo: EDGE STEP MISMATCH at %d,%d\n", x, y);
                    abort();
                }
            }
#endif

            if (e0 < 0 || (e0 == 0 && !edge_owns_tie(&f[1], &f[2]))) continue;
            if (e1 < 0 || (e1 == 0 && !edge_owns_tie(&f[2], &f[0]))) continue;
            if (e2 < 0 || (e2 == 0 && !edge_owns_tie(&f[0], &f[1]))) continue;
            if (cyc == CYC_FILL) {
                fill_pixel(x, y);
                continue;
            }
            w0 = (float)e0 * inv_area; w1 = (float)e1 * inv_area; w2 = (float)e2 * inv_area;
            memset(&pa, 0, sizeof pa);
            pa.x = x; pa.y = y;
            if (has_shade) {
                pa.in.shade[0] = iclamp((int)(w0 * v[0]->r + w1 * v[1]->r + w2 * v[2]->r + 0.5f), 0, 255);
                pa.in.shade[1] = iclamp((int)(w0 * v[0]->g + w1 * v[1]->g + w2 * v[2]->g + 0.5f), 0, 255);
                pa.in.shade[2] = iclamp((int)(w0 * v[0]->b + w1 * v[1]->b + w2 * v[2]->b + 0.5f), 0, 255);
                pa.in.shade[3] = iclamp((int)(w0 * v[0]->a + w1 * v[1]->a + w2 * v[2]->a + 0.5f), 0, 255);
            }
            if (has_tex) {
                float s, t;
                if (use_persp) {
                    float iw0 = v[0]->invw, iw1 = v[1]->invw, iw2 = v[2]->invw;
                    float den = w0 * iw0 + w1 * iw1 + w2 * iw2;
                    if (den <= 0.0f) den = 1e-20f;
                    s = (w0 * v[0]->s * iw0 + w1 * v[1]->s * iw1 + w2 * v[2]->s * iw2) / den;
                    t = (w0 * v[0]->t * iw0 + w1 * v[1]->t * iw1 + w2 * v[2]->t * iw2) / den;
                } else {
                    s = w0 * v[0]->s + w1 * v[1]->s + w2 * v[2]->s;
                    t = w0 * v[0]->t + w1 * v[1]->t + w2 * v[2]->t;
                }
                fetch_texels(tile, s, t, &pa.in);
            }
            if (has_z) {
                float z = w0 * v[0]->z + w1 * v[1]->z + w2 * v[2]->z;
                int az = iclamp((int)(z * 16.0f + 0.5f), 0, 0x3FFF);
                pa.have_z = 1;
                if (OM_Z_SOURCE_PRIM()) { pa.z14 = R.prim_z >> 2; pa.dz14 = 0; }
                else { pa.z14 = az; pa.dz14 = (int)((dzdx < 0 ? -dzdx : dzdx) + (dzdy < 0 ? -dzdy : dzdy) + 0.5f); }
            }
            draw_pixel(&pa);
        }
    }
}


#ifdef RDP_GPU
static int fx_ok_f(float v) { return fx_ok(v); }
/* ---- GPU back end: this front end turns each drawing command into a GpuState + vertices (rdp_gpu.h, docs/switch-android-plan.md section 11) ---- */
typedef struct GpuTexMemo { uint32_t gen, ver, tlut, epoch, gpu_epoch; uint32_t id; int w, h; int valid; } GpuTexMemo;
static GpuTexMemo sGpuMemo[8];
static uint32_t *sGpuTexBuf;

/* The tile's texels decoded to RGBA8888 (exactly what sample_tile_px would fetch), handed to the back end; remembered while nothing that could change them happens. */
static void gpu_tile_tex(int tile, GpuTex *o)
{
    Tile *tl = &R.tile[tile & 7];
    TexCache *tc = tc_current(tl);
    GpuTexMemo *m = &sGpuMemo[tile & 7];
    int w = tc->w, h = tc->h, x, y;
    memset(o, 0, sizeof *o);
    o->sl = (int16_t)tl->sl; o->tl = (int16_t)tl->tl;
    o->sw = (int16_t)(((int)tl->sh - (int)tl->sl) / 4 + 1); o->th = (int16_t)(((int)tl->th - (int)tl->tl) / 4 + 1);
    o->shift_s = tl->shift_s; o->shift_t = tl->shift_t; o->clamp_s = tl->clamp_s; o->clamp_t = tl->clamp_t;
    o->mirror_s = tl->mirror_s; o->mirror_t = tl->mirror_t; o->mask_s = tl->mask_s; o->mask_t = tl->mask_t;
    o->fmt = tl->fmt; o->siz = tl->siz;
    if (!(m->valid && m->epoch == sResets && m->gpu_epoch == gpu_tex_epoch() && m->gen == tc->gen && m->ver == tc->ver && m->tlut == tc->tlut)) {
        static uint32_t one;
        if (w <= 0 || h <= 0) { one = 0; m->id = gpu_texture(&one, 1, 1); m->w = m->h = 1; }
        else {
            if (!sGpuTexBuf) sGpuTexBuf = (uint32_t *)malloc(TC_MAX_TEXELS * sizeof(uint32_t));
            if (!sGpuTexBuf) { one = 0; m->id = gpu_texture(&one, 1, 1); m->w = m->h = 1; }
            else {
                for (y = 0; y < h; y++) {
                    for (x = 0; x < w; x++) {
                        uint8_t d[4];
                        texel_at(tl, x, y, d);
                        sGpuTexBuf[y * w + x] = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
                    }
                }
                m->id = gpu_texture(sGpuTexBuf, w, h);
                m->w = w; m->h = h;
            }
        }
        m->gen = tc->gen; m->ver = tc->ver; m->tlut = tc->tlut; m->epoch = sResets; m->gpu_epoch = gpu_tex_epoch(); m->valid = 1;
    }
    o->id = m->id; o->w = (int16_t)m->w; o->h = (int16_t)m->h;
}

void rdp_comb_plan(uint32_t hi, uint32_t lo, uint8_t idx[2][4][4])
{
    CombPlan pl;
    comb_plan_build(&pl, hi, lo);
    memcpy(idx, pl.idx, sizeof pl.idx);
}

static void gpu_state(GpuState *st, int tile, int need_tex, int copy_mode)
{
    const CombPlan *pl = &R.comb_plan;
    int cyc = (int)OM_CYCLE();
    memset(st, 0, sizeof *st);
    st->comb_hi = R.comb_hi; st->comb_lo = R.comb_lo; st->omh = R.omh; st->oml = R.oml;
    if (copy_mode) st->omh &= ~(1u << 13);                       /* copy mode always point-samples */
    memcpy(st->prim, R.prim, 4); memcpy(st->env, R.env, 4); memcpy(st->fog, R.fog, 4); memcpy(st->blend, R.blend, 4);
    st->prim_lod_frac = R.prim_lod_frac; st->cycle = (uint8_t)cyc; st->k4 = (int16_t)R.k4; st->k5 = (int16_t)R.k5; st->prim_z = R.prim_z;
    st->scis_x0 = (int16_t)(R.scis_xh >> 2); st->scis_y0 = (int16_t)(R.scis_yh >> 2);
    st->scis_x1 = (int16_t)((R.scis_xl + 3) >> 2); st->scis_y1 = (int16_t)((R.scis_yl + 3) >> 2);
    st->cimg_addr = R.cimg.addr; st->zimg_addr = R.zimg; st->cimg_width = R.cimg.width; st->cimg_siz = R.cimg.siz; st->cimg_fmt = R.cimg.fmt;
    st->surf_h = (uint16_t)(st->scis_y1 > 0 ? st->scis_y1 : 0);
    if (need_tex) {
        st->has_tex = 1;
        if (cyc == CYC_2) {
            if (pl->uses_t[0][0] | pl->uses_t[1][1]) gpu_tile_tex(tile, &st->tex[0]);
            if (pl->uses_t[0][1] | pl->uses_t[1][0]) gpu_tile_tex(tile + 1, &st->tex[1]);
        } else if (cyc != CYC_1 || (pl->uses_t[0][0] | pl->uses_t[0][1])) {
            gpu_tile_tex(tile, &st->tex[0]);
            st->tex[1] = st->tex[0];
        }
    }
    if (OM_LOD_EN()) unimpl(UN_LOD);
    if (OM_DETAIL() || OM_SHARPEN()) unimpl(UN_DETAIL_SHARPEN);
    if (OM_KEY()) unimpl(UN_CHROMA_KEY);
}

static void gpu_triangle(const RdpVert *va, const RdpVert *vb, const RdpVert *vc, int tile, int has_shade, int has_tex, int has_z)
{
    const RdpVert *in[3];
    GpuVert o[3];
    GpuState st;
    int i;
    R.n_tri++;
    in[0] = va; in[1] = vb; in[2] = vc;
    for (i = 0; i < 3; i++) if (!fx_ok_f(in[i]->x) || !fx_ok_f(in[i]->y)) return;
    if (OM_CYCLE() == CYC_COPY || OM_CYCLE() == CYC_FILL) return;          /* the hardware cannot draw triangles in copy mode; fill-mode triangles are not modelled on the GPU */
    gpu_state(&st, tile, has_tex, 0);
    st.has_shade = (uint8_t)has_shade;
    st.has_z = (uint8_t)has_z; st.z_prim = (uint8_t)(has_z && OM_Z_SOURCE_PRIM());
    for (i = 0; i < 3; i++) {
        float z14 = in[i]->z * 16.0f;
        o[i].x = in[i]->x; o[i].y = in[i]->y;
        if (st.z_prim) z14 = (float)(R.prim_z >> 2);
        else { z14 = (float)(int)(z14 + 0.5f); if (z14 < 0.0f) z14 = 0.0f; if (z14 > 16383.0f) z14 = 16383.0f; }
        o[i].z = z14 / 16384.0f;
        if (has_shade) { o[i].r = in[i]->r; o[i].g = in[i]->g; o[i].b = in[i]->b; o[i].a = in[i]->a; } else { o[i].r = o[i].g = o[i].b = o[i].a = 0.0f; }
        if (has_tex) {
            if (OM_PERSP()) { float iw = in[i]->invw; o[i].sw = in[i]->s * iw; o[i].tw = in[i]->t * iw; o[i].w = iw; }
            else { o[i].sw = in[i]->s; o[i].tw = in[i]->t; o[i].w = 1.0f; }
        } else { o[i].sw = o[i].tw = 0.0f; o[i].w = 1.0f; }
    }
    gpu_draw(&st, o, 3);
}

/* A screen rectangle [x0,x1) x [y0,y1) as two triangles; texture coordinates at the pixel centres are (u0 + du*(x-x0)), (v0 + dv*(y-y0)) (rectangles: flip swaps the roles). */
static void gpu_rect(const GpuState *st, int x0, int y0, int x1, int y1, float u00, float v00, float dudx, float dudy, float dvdx, float dvdy, float z)
{
    GpuVert q[4];
    int i;
    float cx[4] = { (float)x0, (float)x1, (float)x0, (float)x1 }, cy[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    for (i = 0; i < 4; i++) {
        float px = cx[i] - (float)x0 - 0.5f, py = cy[i] - (float)y0 - 0.5f;       /* from the centre of the first pixel */
        q[i].x = cx[i]; q[i].y = cy[i]; q[i].z = z; q[i].r = q[i].g = q[i].b = q[i].a = 0.0f;
        q[i].sw = u00 + dudx * px + dudy * py; q[i].tw = v00 + dvdx * px + dvdy * py; q[i].w = 1.0f;
    }
    {   GpuVert t[6] = { q[0], q[1], q[2], q[1], q[3], q[2] };
        gpu_draw(st, t, 6); }
}

static void gpu_texrect(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip)
{
    int lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF, tile = (w1 >> 24) & 7, ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
    int s0 = (int16_t)(half1 >> 16), t0 = (int16_t)half1, dsdx = (int16_t)(half2 >> 16), dtdy = (int16_t)half2;
    int cyc = (int)OM_CYCLE(), xs = ulx >> 2, ys = uly >> 2, x0 = xs, y0 = ys, x1, y1;
    float ds = (float)dsdx / 1024.0f, dt = (float)dtdy / 1024.0f, sbase = (float)s0 / 32.0f, tbase = (float)t0 / 32.0f;
    GpuState st;
    R.n_rect++;
    if (cyc == CYC_FILL) return;
    if (cyc == CYC_COPY) { x1 = (lrx >> 2) + 1; y1 = (lry >> 2) + 1; ds /= 4.0f; } else { x1 = lrx >> 2; y1 = lry >> 2; }
    x0 = imax(x0, R.scis_xh >> 2); y0 = imax(y0, R.scis_yh >> 2);
    x1 = imin(x1, (R.scis_xl + 3) >> 2); y1 = imin(y1, (R.scis_yl + 3) >> 2);
    if (x0 >= x1 || y0 >= y1) return;
    gpu_state(&st, tile, 1, cyc == CYC_COPY);
    st.has_z = (uint8_t)OM_Z_SOURCE_PRIM(); st.z_prim = st.has_z;
    {
        /* the texture coordinate of pixel (x, y) is (sbase + ds*(x-xs), tbase + dt*(y-ys)), or with the roles of x and y swapped when flipped */
        float ux = flip ? 0.0f : ds, uy = flip ? ds : 0.0f, vx = flip ? dt : 0.0f, vy = flip ? 0.0f : dt;
        float u00 = sbase + ux * (float)(x0 - xs) + uy * (float)(y0 - ys), v00 = tbase + vx * (float)(x0 - xs) + vy * (float)(y0 - ys);
        gpu_rect(&st, x0, y0, x1, y1, u00, v00, ux, uy, vx, vy, st.z_prim ? (float)(R.prim_z >> 2) / 16384.0f : 0.0f);
    }
}

static void gpu_fill_rect(uint32_t hi, uint32_t lo)
{
    int lrx = (hi >> 12) & 0xFFF, lry = hi & 0xFFF, ulx = (lo >> 12) & 0xFFF, uly = lo & 0xFFF;
    int x0 = ulx >> 2, y0 = uly >> 2, x1, y1, cyc = (int)OM_CYCLE();
    GpuState st;
    if (cyc == CYC_FILL || cyc == CYC_COPY) { x1 = (lrx >> 2) + 1; y1 = (lry >> 2) + 1; } else { x1 = lrx >> 2; y1 = lry >> 2; }
    R.n_rect++;
    x0 = imax(x0, R.scis_xh >> 2); y0 = imax(y0, R.scis_yh >> 2);
    x1 = imin(x1, (R.scis_xl + 3) >> 2); y1 = imin(y1, (R.scis_yl + 3) >> 2);
    if (x0 >= x1 || y0 >= y1) return;
    gpu_state(&st, 0, 0, 0);
    if (cyc == CYC_FILL) { gpu_fill(&st, x0, y0, x1, y1, R.fill_color); return; }
    st.has_z = (uint8_t)OM_Z_SOURCE_PRIM(); st.z_prim = st.has_z;
    gpu_rect(&st, x0, y0, x1, y1, 0, 0, 0, 0, 0, 0, st.z_prim ? (float)(R.prim_z >> 2) / 16384.0f : 0.0f);
}
#endif /* RDP_GPU */

static int census_on(void);
static void census_prim(int kind, int tile, int flags, uint64_t pix0);
static void census_timg(uint32_t addr);
static void census_cimg(uint32_t addr, uint32_t line_bytes);


#ifdef RDP_GPU
/* ---- development check: KYUJO_GPU_VERIFY=1 runs every drawing command through the software renderer AND the GPU back end, then compares the
 * colour image (16 bit only) after each command and reports the commands that make the two pictures drift apart. Not for normal use. */
static int sVerify = -1;
static int sVerPrev;
static unsigned long sVerN;
static int verify_on(void)
{
    if (sVerify < 0) sVerify = getenv("KYUJO_GPU_VERIFY") != NULL;
    return sVerify && gpu_active();
}
static uint8_t sVerRgb[320 * 240 * 3];
static int verify_diff_pixels(int *minx, int *miny, int *maxx, int *maxy)
{
    uint8_t *rgb = sVerRgb;
    int w = R.cimg.width, h = R.scis_yl >> 2, x, y, n = 0;
    *minx = *miny = 9999; *maxx = *maxy = -1;
    if (R.cimg.siz != 2 || w <= 0 || w > 320) return -1;
    if (h > 240) h = 240;
    if (!gpu_read_picture(R.cimg.addr, (uint32_t)w, 2, (unsigned)w, (unsigned)h, 0, rgb)) return -2;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = ram(R.cimg.addr + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 2u, 2);
            unsigned v = p ? ((unsigned)p[0] << 8 | p[1]) : 0;
            int r = c5to8(v >> 11), g = c5to8((v >> 6) & 31), b = c5to8((v >> 1) & 31);
            const uint8_t *q = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
            if (abs(r - q[0]) > 24 || abs(g - q[1]) > 24 || abs(b - q[2]) > 24) {
                n++;
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
                if (y < *miny) *miny = y;
                if (y > *maxy) *maxy = y;
            }
        }
    }
    return n;
}
static void verify_after(const char *what, int reset)
{
    int x0, y0, x1, y1, n = verify_diff_pixels(&x0, &y0, &x1, &y1);
    sVerN++;
    if (n < 0) return;
    if (reset) { sVerPrev = n; return; }
    if (n > sVerPrev + 300) {
        fprintf(stderr, "verify: #%lu %s cyc=%d comb=%06X/%08X omh=%06X oml=%08X cimg=%06X zimg=%06X diff %d -> %d box %d,%d-%d,%d prim=%02X%02X%02X%02X env=%02X%02X%02X%02X\n",
                sVerN, what, (int)OM_CYCLE(), R.comb_hi, R.comb_lo, R.omh, R.oml, R.cimg.addr, R.zimg, sVerPrev, n, x0, y0, x1, y1,
                R.prim[0], R.prim[1], R.prim[2], R.prim[3], R.env[0], R.env[1], R.env[2], R.env[3]);
    }
    if (!reset && n > sVerPrev + 300 && getenv("KYUJO_GPU_VERIFY_DUMP") && sVerPrev > 5000) {
        static int ndump;
        int w = R.cimg.width, h = R.scis_yl >> 2, x, y;
        if (ndump < 6 && h <= 240 && w <= 320) {
            char fn[160];
            FILE *f;
            snprintf(fn, sizeof fn, "%s/v%02d_gpu.ppm", getenv("KYUJO_GPU_VERIFY_DUMP"), ndump);
            f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(sVerRgb, 3, (size_t)w * (size_t)h, f); fclose(f); }
            snprintf(fn, sizeof fn, "%s/v%02d_sw.ppm", getenv("KYUJO_GPU_VERIFY_DUMP"), ndump);
            f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                    uint8_t *p = ram(R.cimg.addr + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 2u, 2);
                    unsigned v = p ? ((unsigned)p[0] << 8 | p[1]) : 0;
                    fputc(c5to8(v >> 11), f); fputc(c5to8((v >> 6) & 31), f); fputc(c5to8((v >> 1) & 31), f);
                }
                fclose(f);
            }
            ndump++;
        }
    }
    sVerPrev = n;
}
#endif

void rdp_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, int flip)
{
#ifdef RDP_GPU
    if (verify_on()) { texture_rectangle_impl(w0, w1, half1, half2, flip); gpu_texrect(w0, w1, half1, half2, flip); verify_after("texrect", 0); return; }
    if (gpu_active()) { gpu_texrect(w0, w1, half1, half2, flip); return; }
#endif
    if (census_on()) {
        uint64_t p0 = R.n_pix;
        texture_rectangle_impl(w0, w1, half1, half2, flip);
        census_prim(1, (int)((w1 >> 24) & 7), 1, p0);
        return;
    }
    if (pk_on()) {
        unsigned long long t0 = host_now_ns(); uint64_t p0 = R.n_pix;
        texture_rectangle_impl(w0, w1, half1, half2, flip);
        pk_add(PK_RECT, 1, t0, p0);
    } else {
        texture_rectangle_impl(w0, w1, half1, half2, flip);
    }
}

/* Development: KYUJO_PRIM_TRACE=1 prints every new combination of combiner, colour registers and texture format a triangle is drawn with (once each). */
static void prim_trace(int tile, int has_shade, int has_tex)
{
    static int on = -1;
    static uint32_t (*seen)[10];                                 /* 160 KiB, allocated when the trace is on (it would otherwise be part of the program image) */
    static unsigned nseen;
    static unsigned long ntri;
    uint32_t k[10];
    unsigned i;
    if (on < 0) on = getenv("KYUJO_PRIM_TRACE") != NULL;
    if (!on) return;
    if (!seen && !(seen = calloc(4096, sizeof *seen))) return;
    ntri++;
    k[0] = R.comb_hi; k[1] = R.comb_lo; k[2] = R.omh; k[3] = R.oml;
    k[4] = (uint32_t)R.prim[0] << 24 | (uint32_t)R.prim[1] << 16 | (uint32_t)R.prim[2] << 8 | R.prim[3];
    k[5] = (uint32_t)R.env[0] << 24 | (uint32_t)R.env[1] << 16 | (uint32_t)R.env[2] << 8 | R.env[3];
    k[6] = (uint32_t)R.fog[0] << 24 | (uint32_t)R.fog[1] << 16 | (uint32_t)R.fog[2] << 8 | R.fog[3];
    k[7] = (uint32_t)R.tile[tile & 7].fmt << 8 | (uint32_t)R.tile[tile & 7].siz | (uint32_t)(has_tex ? 1 : 0) << 16 | (uint32_t)(has_shade ? 1 : 0) << 17 | (uint32_t)R.tile[tile & 7].palette << 20;
    k[8] = R.cimg.addr; k[9] = 0;
    for (i = 0; i < nseen; i++) if (!memcmp(seen[i], k, sizeof k)) return;
    if (nseen < 4096) memcpy(seen[nseen++], k, sizeof k);
    fprintf(stderr, "prim: tri#%lu comb=%06X/%08X omh=%06X oml=%08X prim=%08X env=%08X fog=%08X tile fmt %u siz %u pal %u tex %d shade %d cimg %06X\n", ntri, k[0], k[1], k[2], k[3], k[4], k[5], k[6],
            R.tile[tile & 7].fmt, R.tile[tile & 7].siz, R.tile[tile & 7].palette, has_tex, has_shade, k[8]);
}

void rdp_triangle(const RdpVert *va, const RdpVert *vb, const RdpVert *vc, int tile, int level, int has_shade, int has_tex, int has_z)
{
    prim_trace(tile, has_shade, has_tex);
#ifdef RDP_GPU
    if (verify_on()) { triangle_impl(va, vb, vc, tile, level, has_shade, has_tex, has_z); gpu_triangle(va, vb, vc, tile, has_shade, has_tex, has_z); verify_after("tri", 0); return; }
    if (gpu_active()) { gpu_triangle(va, vb, vc, tile, has_shade, has_tex, has_z); return; }
#endif
    if (census_on()) {
        uint64_t p0 = R.n_pix;
        triangle_impl(va, vb, vc, tile, level, has_shade, has_tex, has_z);
        census_prim(0, tile, (has_tex ? 1 : 0) | (has_shade ? 2 : 0) | (has_z ? 4 : 0), p0);
        return;
    }
    if (pk_on()) {
        unsigned long long t0 = host_now_ns(); uint64_t p0 = R.n_pix;
        triangle_impl(va, vb, vc, tile, level, has_shade, has_tex, has_z);
        pk_add(PK_TRI, has_tex, t0, p0);
    } else {
        triangle_impl(va, vb, vc, tile, level, has_shade, has_tex, has_z);
    }
}

static void cmd_fill_rect(uint32_t hi, uint32_t lo)
{
#ifdef RDP_GPU
    if (verify_on()) { fill_rect_impl(hi, lo); gpu_fill_rect(hi, lo); verify_after("fill", 1); return; }
    if (gpu_active()) { gpu_fill_rect(hi, lo); return; }
#endif
    if (census_on()) {
        uint64_t p0 = R.n_pix;
        fill_rect_impl(hi, lo);
        census_prim(2, 0, 0, p0);
        return;
    }
    if (pk_on()) {
        unsigned long long t0 = host_now_ns(); uint64_t p0 = R.n_pix;
        fill_rect_impl(hi, lo);
        pk_add(PK_FILL, 0, t0, p0);
    } else {
        fill_rect_impl(hi, lo);
    }
}


/* ---- development: census of the drawing modes a game run uses (KYUJO_RDP_CENSUS=<file>) ---------------------------------------
 * Written for the GPU renderer's design (docs/switch-android-plan.md, "GPU renderer"): which combiner / blender / z settings does the game really
 * draw with, and does it ever use a colour image as a texture? One line per distinct set of settings, with the number of primitives and pixels
 * (the pixel counts are those of the software renderer), written when the program exits. Costs one hash lookup per primitive when on; nothing when off. */
typedef struct CensusKey { uint32_t comb_hi, comb_lo, omh, oml; uint8_t kind, flags, tfmt, tsiz, cimg_siz, pad[3]; } CensusKey;
typedef struct CensusRow { CensusKey k; uint64_t prims, pixels; int used; } CensusRow;
#define CENSUS_ROWS 8192
static CensusRow *sCensus;
static int sCensusOn = -1;
static const char *sCensusFile;
static uint32_t sCensusCimg[32][3], sCensusCimgN, sCensusCur, sCensusTimgInCimg, sCensusTimgLoads, sCensusZimgTex;
static uint64_t sCensusDropped, sCensusTotal;

static void census_write(void)
{
    FILE *f = fopen(sCensusFile, "w");
    uint32_t i;
    if (!f) return;
    fprintf(f, "# kind(0 tri,1 texrect,2 fillrect) comb_hi comb_lo omh oml flags(1 tex,2 shade,4 z) tile_fmt tile_siz cimg_siz prims pixels\n");
    for (i = 0; i < CENSUS_ROWS; i++) {
        const CensusRow *r = &sCensus[i];
        if (!r->used) continue;
        fprintf(f, "%u %06X %08X %06X %08X %u %u %u %u %llu %llu\n", r->k.kind, r->k.comb_hi, r->k.comb_lo, r->k.omh, r->k.oml, r->k.flags, r->k.tfmt,
                r->k.tsiz, r->k.cimg_siz, (unsigned long long)r->prims, (unsigned long long)r->pixels);
    }
    fprintf(f, "# colour images selected: %u distinct (address, width*bytes):", sCensusCimgN);
    for (i = 0; i < sCensusCimgN; i++) fprintf(f, " %06X/%ux%u", sCensusCimg[i][0], sCensusCimg[i][1], sCensusCimg[i][2]);
    fprintf(f, "\n# texture images loaded: %u, of them inside a colour image: %u; z-buffer used as texture: %u; table overflow: %llu\n",
            sCensusTimgLoads, sCensusTimgInCimg, sCensusZimgTex, (unsigned long long)sCensusDropped);
    fclose(f);
}

static int census_on(void)
{
    if (sCensusOn < 0) {
        sCensusFile = getenv("KYUJO_RDP_CENSUS");
        sCensusOn = sCensusFile != NULL && sCensusFile[0];
        if (sCensusOn) {
            sCensus = (CensusRow *)calloc(CENSUS_ROWS, sizeof *sCensus);
            if (!sCensus) sCensusOn = 0; else atexit(census_write);
        }
    }
    return sCensusOn;
}

static void census_prim(int kind, int tile, int flags, uint64_t pix0)
{
    CensusKey k;
    uint32_t h, n;
    if (sCensusCur < sCensusCimgN) { uint32_t rows = (uint32_t)(R.scis_yl + 3) >> 2; if (rows > sCensusCimg[sCensusCur][2]) sCensusCimg[sCensusCur][2] = rows; }
    if ((++sCensusTotal & 0x7FFFu) == 0) census_write();         /* a run that is killed (timeout) never reaches atexit */
    memset(&k, 0, sizeof k);
    k.kind = (uint8_t)kind; k.flags = (uint8_t)flags; k.comb_hi = R.comb_hi; k.comb_lo = R.comb_lo; k.omh = R.omh; k.oml = R.oml;
    k.tfmt = R.tile[tile & 7].fmt; k.tsiz = R.tile[tile & 7].siz; k.cimg_siz = R.cimg.siz;
    h = (k.comb_hi * 2654435761u) ^ (k.comb_lo * 40503u) ^ (k.omh * 97u) ^ (k.oml * 2246822519u) ^ ((uint32_t)kind << 5) ^ ((uint32_t)flags << 9) ^ ((uint32_t)k.tfmt << 13) ^ ((uint32_t)k.tsiz << 16);
    for (n = 0; n < CENSUS_ROWS; n++) {
        CensusRow *r = &sCensus[(h + n) % CENSUS_ROWS];
        if (!r->used) { r->used = 1; r->k = k; }
        if (memcmp(&r->k, &k, sizeof k) == 0) { r->prims++; r->pixels += R.n_pix - pix0; return; }
    }
    sCensusDropped++;
}

static void census_timg(uint32_t addr)
{
    uint32_t i;
    sCensusTimgLoads++;
    for (i = 0; i < sCensusCimgN; i++) if (sCensusCimg[i][2] && addr - sCensusCimg[i][0] < sCensusCimg[i][1] * sCensusCimg[i][2]) { sCensusTimgInCimg++; break; }
    if (R.zimg && addr - R.zimg < 320u * 240u * 2u && (sCensusCimgN == 0 || addr != sCensusCimg[0][0])) sCensusZimgTex++;
}

static void census_cimg(uint32_t addr, uint32_t line_bytes)
{
    uint32_t i;
    for (i = 0; i < sCensusCimgN; i++) if (sCensusCimg[i][0] == addr) { sCensusCur = i; return; }
    if (sCensusCimgN < 32) { sCensusCimg[sCensusCimgN][0] = addr; sCensusCimg[sCensusCimgN][1] = line_bytes; sCensusCimg[sCensusCimgN][2] = 0; sCensusCur = sCensusCimgN++; }
}

/* ---- command interpreter ----------------------------------------------------------------------------------------------------------- */

void rdp_set_other_modes(uint32_t hi, uint32_t lo)
{
    R.omh = hi & 0x00FFFFFFu;
    R.oml = lo;
    if (OM_RGB_DITHER() != 3 || OM_ALPHA_DITHER() != 3) unimpl(UN_DITHER);
}

static int sext9(uint32_t v) { return (int)((v & 0x1FF) ^ 0x100) - 0x100; }

void rdp_command(uint32_t hi, uint32_t lo)
{
    unsigned op = (hi >> 24) & 0x3F;
    if ((op == 0x30 || (op >= 0x32 && op <= 0x37) || op == 0x2F || (op >= 0x3C && op <= 0x3F)) && env_set("KYUJO_RDP_CMDS", &sEnvCmds)) {      /* development trace */
        fprintf(stderr, "rdpcmd %02X %08X %08X", op, hi, lo);
        if (op == 0x3D && (lo & 0x00FFFFFFu) < 0x00800000u + 0x00800000u) {             /* texture image: show its first bytes (the trace is a development tool) */
            const uint8_t *m = RDRAM_PTR(lo & 0x00FFFFFFu);
            fprintf(stderr, " data %02X%02X%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        }
        fprintf(stderr, "\n");
    }
    switch (op) {
    case 0x00: case 0x26: case 0x27: case 0x28:             /* no-op, sync load, sync pipe, sync tile: nothing to wait for in software */
        break;
    case 0x29:                                              /* sync full */
        R.full_syncs++;
        break;
    case 0x2A: case 0x2B:                                   /* set key GB / R: chroma key is not modelled */
        break;
    case 0x2C:                                              /* set convert: K0..K5, 9 bits each; only K4 and K5 reach the combiner */
        R.k4 = sext9(lo >> 9);
        R.k5 = sext9(lo);
        break;
    case 0x2D:                                              /* set scissor */
        R.scis_xh = (hi >> 12) & 0xFFF; R.scis_yh = hi & 0xFFF; R.scis_xl = (lo >> 12) & 0xFFF; R.scis_yl = lo & 0xFFF;
        break;
    case 0x2E:                                              /* set primitive depth */
        R.prim_z = (uint16_t)(lo >> 16); R.prim_dz = (uint16_t)lo;
        break;
    case 0x2F:
        rdp_set_other_modes(hi, lo);
        break;
    case 0x30: cmd_load_tlut(hi, lo); break;
    case 0x32: {                                            /* set tile size */
        Tile *tl = &R.tile[(lo >> 24) & 7];
        tl->sl = (hi >> 12) & 0xFFF; tl->tl = hi & 0xFFF; tl->sh = (lo >> 12) & 0xFFF; tl->th = lo & 0xFFF;
        R.tile_ver[tl - R.tile]++;
        break;
    }
    case 0x33: cmd_load_block(hi, lo); break;
    case 0x34: cmd_load_tile(hi, lo); break;
    case 0x35: {                                            /* set tile */
        Tile *tl = &R.tile[(lo >> 24) & 7];
        tl->fmt = (hi >> 21) & 7; tl->siz = (hi >> 19) & 3; tl->line = (hi >> 9) & 0x1FF; tl->tmem = hi & 0x1FF;
        tl->palette = (lo >> 20) & 0xF;
        tl->clamp_t = (lo >> 19) & 1; tl->mirror_t = (lo >> 18) & 1; tl->mask_t = (lo >> 14) & 0xF; tl->shift_t = (lo >> 10) & 0xF;
        tl->clamp_s = (lo >> 9) & 1;  tl->mirror_s = (lo >> 8) & 1;  tl->mask_s = (lo >> 4) & 0xF;  tl->shift_s = lo & 0xF;
        R.tile_ver[tl - R.tile]++;
        break;
    }
    case 0x36: cmd_fill_rect(hi, lo); break;
    case 0x37: R.fill_color = lo; break;
    case 0x38: R.fog[0] = lo >> 24; R.fog[1] = lo >> 16; R.fog[2] = lo >> 8; R.fog[3] = lo; break;
    case 0x39: R.blend[0] = lo >> 24; R.blend[1] = lo >> 16; R.blend[2] = lo >> 8; R.blend[3] = lo; break;
    case 0x3A:
        R.prim_min_lod = (uint8_t)(hi >> 8); R.prim_lod_frac = (uint8_t)hi;
        R.prim[0] = lo >> 24; R.prim[1] = lo >> 16; R.prim[2] = lo >> 8; R.prim[3] = lo;
        break;
    case 0x3B: R.env[0] = lo >> 24; R.env[1] = lo >> 16; R.env[2] = lo >> 8; R.env[3] = lo; break;
    case 0x3C: R.comb_hi = hi & 0x00FFFFFFu; R.comb_lo = lo; comb_plan_build(&R.comb_plan, R.comb_hi, R.comb_lo); break;
    case 0x3D:
        R.timg.fmt = (hi >> 21) & 7; R.timg.siz = (hi >> 19) & 3; R.timg.width = (uint16_t)((hi & 0x3FF) + 1); R.timg.addr = lo & 0x00FFFFFFu;
        if (census_on()) census_timg(R.timg.addr);
        break;
    case 0x3E: R.zimg = lo & 0x00FFFFFFu; break;
    case 0x3F:
        R.cimg.fmt = (hi >> 21) & 7; R.cimg.siz = (hi >> 19) & 3; R.cimg.width = (uint16_t)((hi & 0x3FF) + 1); R.cimg.addr = lo & 0x00FFFFFFu;
        cimg_log_add(R.cimg.addr, (uint32_t)R.cimg.width * (R.cimg.siz == SIZ_32 ? 4u : 2u));
        if (census_on()) census_cimg(R.cimg.addr, (uint32_t)R.cimg.width * (R.cimg.siz == SIZ_32 ? 4u : 2u));
        break;
    default:
        /* triangles (0x08..0x0F) and texture rectangles (0x24, 0x25) reach the renderer through their own entry points */
        break;
    }
}
