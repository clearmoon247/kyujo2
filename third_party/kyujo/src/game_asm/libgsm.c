/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * libgsm.c - the GSM 06.10 speech decoder's hand-written MIPS assembly, written in C.
 *
 * Replaces rom.yaml segment `hasm libgsm` (ROM 0x49190..0x49790, 0x600 bytes, console addresses 0x80048590..0x80048B90). The decomp has no
 * source for it, so this file was written by reading the machine code (disassembled with llvm-mc) function by function; every statement
 * below names the instruction addresses it stands for, so a reader can check it against `llvm-mc --disassemble` of that ROM range.
 * tests/native/gsm/ runs the ORIGINAL machine code in a reference interpreter and compares it with this file on many inputs.
 *
 * The original is GSM 06.10 (ETSI full-rate) decoding in the form of the public "libgsm" by Jutta Degener and Carsten Bormann, but it is
 * NOT identical to that library: it does not saturate in several places (v[], the long-term sum, the interpolated coefficients) and keeps
 * 32-bit values where libgsm keeps 16-bit ones. Those differences are reproduced exactly (the game's audio depends on the console's
 * output, not on the standard's). Arithmetic therefore uses 32-bit wrap-around integers like the MIPS code (-fwrapv).
 *
 * Functions (console address, argument registers):
 *   0x80048590 Gsm_RPE_Decoding(xmaxcr, Mcr, xMcr, erp)         RPE decoding of one 40-sample sub-frame
 *   0x80048684 Gsm_Long_term_synthesis_filtering(S, Ncr, bcr, erp)
 *   0x80048740 (internal, own register convention)               the short-term lattice filter loop      -> short_term_loop()
 *   0x80048904 Gsm_Short_Term_Synthesis_Filter(S, in, out)       interpolation + filter of 160 samples
 *   0x80048A58 (internal)  interpolation of 8 coefficients       -> segment_coefficients()
 *   0x80048AFC/B14/B24/B3C coefficient formulas for the 4 segments, 0x80048B3C..B8C the LAR -> reflection coefficient conversion
 *
 * GAME-side file (compiled through tools/native/be_memory.py): reads and writes of game memory see big-endian data like the console.
 */
typedef short s16;
typedef int s32;
typedef unsigned int u32;

void host_fatal(const char *message) __attribute__((noreturn));      /* platform.h; not included: the game side has no libc headers */

/* The decoder state, as the assembly addresses it (offsets from the state pointer in $a0). Total 0x18A bytes, the same as gsm_state in
 * src/libgsm/libgsm.h, which only names the last three fields. */
struct GsmState {
    s16 dp0[160];          /* 0x000 reconstructed short-term residual: 120 samples of history, then the current 40 (0x0F0 = 120 * 2) */
    s32 v[8];              /* 0x140 lattice filter state; the assembly keeps it in registers $16..$23 and stores it with `sw` */
    s16 unused[2];         /* 0x160 not touched by these functions */
    s16 msr;               /* 0x164 de-emphasis filter state (`lh $7, 356($4)`) */
    s16 nrp;               /* 0x166 last valid long-term lag (`lh $11, 358($4)`) */
    s16 LARpp[2][8];       /* 0x168 decoded log-area ratios of the previous and the current frame (indexed by frame_index) */
    s16 frame_index;       /* 0x188 which row of LARpp is the current one (`lh $1, 392($4)`) */
};

/* mult / mflo / addiu 16384 / sra 15, repeated all over the assembly: (a * b + 0x4000) >> 15 in 32-bit arithmetic. */
static s32 mult_r(s32 a, s32 b)
{
    return (s32)(((u32)a * (u32)b + 16384u)) >> 15;
}

/* ---- Gsm_RPE_Decoding, 0x80048590..0x80048678 ---------------------------------------------------------------------------------- */
void Gsm_RPE_Decoding(s32 xmaxcr, s32 Mcr, s16 *xMcr, s16 *erp)
{
    s32 exp, mant, factor, shift, round, i;
    s16 *dst;

    if (Mcr < 0 || Mcr > 3 || xmaxcr < 0 || xmaxcr > 63) {
        host_fatal("GSM: RPE decoding parameters out of range");
    }
    if (xmaxcr > 7) {                                   /* 0x80048590: addiu $1,$4,-7 ; bgtz $1 -> 0x800485CC */
        exp = (xmaxcr - 8) >> 3;                        /* 0x800485CC..D0: addiu $8,$4,-8 ; sra $8,$8,3 */
        mant = xmaxcr & 7;                              /* 0x800485D4: andi $9,$4,7 */
    } else {                                            /* 0x8004859C..0x800485C0: byte table at 0x8004867C, signed: exp = b >> 4, mant = b & 7 */
        s32 b;
        switch (xmaxcr) {                               /* the 8 bytes C7 D7 E3 E7 F1 F3 F5 F7 at 0x8004867C, as signed bytes */
        case 0: b = -57; break;
        case 1: b = -41; break;
        case 2: b = -29; break;
        case 3: b = -25; break;
        case 4: b = -15; break;
        case 5: b = -13; break;
        case 6: b = -11; break;
        default: b = -9; break;
        }
        exp = b >> 4;                                   /* 0x800485BC: sra $8,$1,4 */
        mant = b & 7;                                   /* 0x800485C0: andi $9,$1,7 */
    }
    factor = (s32)(((u32)mant << 11) + 18431u);         /* 0x800485D8..DC: sll $12,$9,11 ; addiu $12,$12,0x47FF */
    shift = 6 - exp;                                    /* 0x800485E0..E4: $10 = 6 - exp */
    round = (s32)(1u << ((u32)(shift - 1) & 31));       /* 0x800485E8..F0: sllv by ($10 - 1), the shift count is taken modulo 32 */

    for (i = 0; i < 13; i++) {                          /* 0x800485F4..0x80048630: 13 pulses, in place in xMcr */
        s32 x = xMcr[i];                                /* lh */
        x = (s32)((u32)x << 13) - 28672;                /* sll 13 ; addiu -0x7000 */
        x = mult_r(x, factor);                       /* mult / mflo / +0x4000 / sra 15 */
        x = x + round;                                  /* addu $13,$13,$11 */
        x = x >> ((u32)shift & 31);                     /* srav $13,$13,$10 */
        xMcr[i] = (s16)x;                               /* sh */
    }

    for (i = 0; i < 40; i++) {                          /* 0x80048634..0x80048648: clear the 40 output samples */
        erp[i] = 0;
    }
    dst = erp + Mcr;                                    /* 0x8004864C..50: $25 = erp + 2 * Mcr bytes */
    for (i = 0; i < 13; i++) {                          /* 0x80048654..0x8004866C: RPE grid positioning, every third sample */
        *dst = xMcr[i];
        dst += 3;                                       /* addiu $25,$25,6 */
    }
}

/* ---- Gsm_Long_term_synthesis_filtering, 0x80048684..0x80048734 ------------------------------------------------------------------ */
void Gsm_Long_term_synthesis_filtering(struct GsmState *S, s32 Ncr, s32 bcr, s16 *erp)
{
    s32 Nr, qlb, k;

    Nr = S->nrp;                                        /* 0x80048684: lh $11,358($4) */
    if (Ncr >= 40 && Ncr <= 120) {                      /* 0x80048688..A0: Ncr - 40 < 0 skips, Ncr - 120 > 0 skips */
        Nr = Ncr;
    }
    S->nrp = (s16)Nr;                                   /* 0x800486A4: sh $11,358($4) */

    switch (bcr) {                                      /* the 4 halfwords at 0x80048738: gsm_QLB = 3277, 11469, 21299, 32767 */
    case 0: qlb = 3277; break;
    case 1: qlb = 11469; break;
    case 2: qlb = 21299; break;
    case 3: qlb = 32767; break;
    default: host_fatal("GSM: long-term gain index out of range"); return;
    }
    if (Nr < 0 || Nr > 120) {                           /* the assembly would read outside the state */
        host_fatal("GSM: long-term lag out of range");
    }

    for (k = 0; k < 120; k++) {                         /* 0x800486C8..E0: shift the history by 40 samples (before the new samples are made) */
        S->dp0[k] = S->dp0[k + 40];
    }
    for (k = 0; k < 40; k++) {                          /* 0x800486E4..0x8004872C */
        s32 past = S->dp0[120 + k - Nr];                /* lh 240($13), $13 = state - 2 * Nr, advancing by 2 */
        s32 contribution = mult_r(past, qlb);           /* mult / mflo / +0x4000 / sra 15 */
        S->dp0[120 + k] = (s16)(contribution + erp[k]); /* addu (NO saturation) ; sh 240($14) */
    }
}

/* ---- the lattice filter loop, 0x80048740..0x800488FC ---------------------------------------------------------------------------- */
/* Own register convention in the original ($9 input, $8 output, $11 count - 1, $16..$23 = v[0..7], $14/$15/$24/$25 = the 8 reflection
 * coefficients packed two per register, $7 = msr). Here: explicit arguments. rp[i] are the coefficients as signed 16-bit values. */
static void short_term_loop(s32 v[8], s32 *msr, const s32 rp[8], const s16 *in, s16 *out, s32 count)
{
    s32 k;

    for (k = 0; k < count; k++) {
        s32 sri = in[k];                                /* 0x80048740: lh $10,0($9) */
        s32 i, x;

        sri -= mult_r(v[7], rp[7]);                     /* 0x80048748..60: coefficient 7 (low half of $25) */
        for (i = 6; i >= 0; i--) {                      /* 0x80048764..0x800488A0: coefficients 6..0, two multiplications each */
            sri -= mult_r(v[i], rp[i]);
            v[i + 1] = mult_r(rp[i], sri) + v[i];       /* e.g. 0x8004878C: addu $23,$1,$22 (v[7] = ... + v[6]) */
        }
        v[0] = sri;                                     /* 0x800488A4: move $16,$10 */

        x = mult_r(28180, *msr);                        /* 0x800488A8..B8: ori $1,28180 ; mult $1,$7 ... */
        *msr = sri + x;                                 /* 0x800488BC..C0: addu $10,$10,$1 ; move $7,$10 (32-bit, no saturation) */
        x = (s32)((u32)*msr << 1);                      /* 0x800488C4: sll $10,$10,1 */
        {
            s32 overflow = ((x >> 15) + 1) >> 1;        /* 0x800488C8..D0: 0 when x fits in 16 bits */
            if (overflow != 0) {
                x = (overflow < 0) ? -32768 : 32767;    /* 0x800488DC..E4 (the delay slot at 0x800488E0 stores -32768 first) */
            }
        }
        out[k] = (s16)(x & 0xFFF8);                     /* 0x800488E8..EC: andi $10,$10,0xFFF8 ; sh */
    }
}

/* ---- coefficient interpolation, 0x80048A58..0x80048B8C ---------------------------------------------------------------------- */
/* Converts one interpolated log-area ratio into a reflection coefficient (0x80048B3C..0x80048B8C). Returns a signed 16-bit value. */
static s32 larp_to_rp(s32 v)
{
    s32 negative = 0, t;

    if (v < 0) {                                        /* 0x80048B3C..48 */
        v = -v;
        negative = 1;
    }
    if (v < 11059) {                                    /* 0x80048B4C..54: 0x2B33 */
        t = v << 1;
    } else if (v < 20070) {                             /* 0x80048B58..60: 0x4E66 */
        t = v + 11059;
    } else {                                            /* 0x80048B64..68 */
        t = (v >> 2) + 26112;
    }
    if (t > 32767) {                                    /* 0x80048B6C..78 */
        t = 32767;
    }
    return negative ? -t : t;                           /* 0x80048B7C..84; the final `andi $10,$10,0xFFFF` is undone by the sign-extension of the user */
}

/* Interpolation of the 8 coefficients for one of the four segments of a 160-sample frame, then conversion to reflection coefficients
 * (0x80048A58..0x80048AF8). `segment` selects the formula: 0 -> 0x80048AFC, 1 -> 0x80048B14, 2 -> 0x80048B24, 3 -> 0x80048B3C (none). */
static void segment_coefficients(const struct GsmState *S, s32 segment, s32 rp[8])
{
    const s16 *cur, *prev;
    s32 i;

    if (S->frame_index == 0) {                          /* 0x80048A58..6C: $8 = row of the current frame, $9 = row of the previous one */
        cur = S->LARpp[0];
        prev = S->LARpp[1];
    } else {
        cur = S->LARpp[1];
        prev = S->LARpp[0];
    }
    for (i = 0; i < 8; i++) {
        s32 c = cur[i], p = prev[i], v;
        switch (segment) {
        case 0:  v = (c >> 2) + (p >> 1) + (p >> 2); break;     /* 0x80048AFC..10 */
        case 1:  v = (c >> 1) + (p >> 1); break;                /* 0x80048B14..20 */
        case 2:  v = (c >> 2) + (p >> 2) + (c >> 1); break;     /* 0x80048B24..38 */
        default: v = c; break;                                  /* 0x80048B3C: no interpolation */
        }
        rp[i] = larp_to_rp(v);
    }
}

/* ---- Gsm_Short_Term_Synthesis_Filter, 0x80048904..0x80048A54 -------------------------------------------------------------------- */
void Gsm_Short_Term_Synthesis_Filter(struct GsmState *S, s16 *in, s16 *out)
{
    static const s32 start[4] = { 0, 13, 27, 40 };      /* 0x80048978, 0x8004899C (+26 bytes), 0x800489C0 (+54), 0x800489E4 (+80) */
    static const s32 count[4] = { 13, 14, 13, 120 };    /* 0x80048984 (ori 12), 0x800489A8 (13), 0x800489CC (12), 0x800489F0 (119) -> + 1 each */
    s32 v[8], msr, rp[8], seg, i;

    for (i = 0; i < 8; i++) {                           /* 0x80048940..5C: lw $16..$23 from state + 0x140 */
        v[i] = S->v[i];
    }
    msr = S->msr;                                       /* 0x80048960: lh $7,356($4) */

    for (seg = 0; seg < 4; seg++) {
        segment_coefficients(S, seg, rp);
        short_term_loop(v, &msr, rp, in + start[seg], out + start[seg], count[seg]);
    }

    for (i = 0; i < 8; i++) {                           /* 0x800489F4..A14: sw $16..$23 */
        S->v[i] = v[i];
    }
    S->msr = (s16)msr;                                  /* 0x80048A18: sh $7,356($4) */
}
