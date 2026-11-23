#include "guest_calls.h"
#include <stdio.h>
#include <stddef.h>

/* Byte-identical in both games. The caller uses N64Recomp's word-swapped RAM. */
#include "libgsm.c"

void host_fatal(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}

static void read_samples(uint8_t *rdram, gpr address, short *samples, size_t count) {
    for (size_t i = 0; i < count; i++) samples[i] = MEM_H(2*i, address);
}

static void write_samples(uint8_t *rdram, gpr address, const short *samples, size_t count) {
    for (size_t i = 0; i < count; i++) MEM_H(2*i, address) = samples[i];
}

void func_800214C0(uint8_t *rdram, recomp_context *ctx) {
    short pulses[13], residual[40];
    read_samples(rdram, ctx->r6, pulses, 13);
    Gsm_RPE_Decoding((int)ctx->r4, (int)ctx->r5, pulses, residual);
    write_samples(rdram, ctx->r6, pulses, 13);
    write_samples(rdram, ctx->r7, residual, 40);
}

void func_800215B4(uint8_t *rdram, recomp_context *ctx) {
    struct GsmState state;
    short residual[40];
    gpr base = ctx->r4;
    read_samples(rdram, base, state.dp0, 160);
    state.nrp = MEM_H(0x166, base);
    read_samples(rdram, ctx->r7, residual, 40);
    Gsm_Long_term_synthesis_filtering(&state, (int)ctx->r5, (int)ctx->r6, residual);
    write_samples(rdram, base, state.dp0, 160);
    MEM_H(0x166, base) = state.nrp;
}

void func_80021834(uint8_t *rdram, recomp_context *ctx) {
    struct GsmState state;
    short input[160], output[160];
    gpr base = ctx->r4;
    for (int i = 0; i < 8; i++) state.v[i] = MEM_W(0x140 + 4*i, base);
    state.msr = MEM_H(0x164, base);
    state.frame_index = MEM_H(0x188, base);
    read_samples(rdram, base + 0x168, state.LARpp[0], 8);
    read_samples(rdram, base + 0x178, state.LARpp[1], 8);
    read_samples(rdram, ctx->r5, input, 160);
    Gsm_Short_Term_Synthesis_Filter(&state, input, output);
    write_samples(rdram, ctx->r6, output, 160);
    for (int i = 0; i < 8; i++) MEM_W(0x140 + 4*i, base) = state.v[i];
    MEM_H(0x164, base) = state.msr;
}
