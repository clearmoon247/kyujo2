#include "recomp.h"
#include "funcs.h"
#include <stdio.h>

static unsigned status, word, offset_read, reads, mask, mask_calls;

void osSetIntMask_recomp(uint8_t *rdram, recomp_context *ctx) {
    unsigned previous = mask;
    mask = ctx->r4;
    ctx->r2 = previous;
    ++mask_calls;
}

gpr stadium2_pi_status(uint8_t *rdram, recomp_context *ctx) { return status; }

uint32_t stadium2_read_cart_word(uint32_t offset) {
    offset_read = offset;
    ++reads;
    return word;
}

static int check_probe(uint8_t *rdram, recomp_func_t *probe, unsigned offset,
                       unsigned expected_low16, int mismatch) {
    const gpr sp = (gpr)(int32_t)0x80004000;
    for (unsigned mode = 0; mode < 3; ++mode) {
        recomp_context ctx = {0};
        ctx.r29 = sp;
        ctx.r4 = 7;
        ctx.r31 = 0x1234;
        status = mode == 0 ? 3 : 0;
        word = 0xabcd0000u | (expected_low16 ^ (mode == 2));
        reads = mask_calls = 0;
        mask = 0x3f01;
        probe(rdram, &ctx);
        if (ctx.r2 != (gpr)(int64_t)(mode == 2 ? mismatch : 7) ||
            ctx.r29 != sp || ctx.r31 != 0x1234 || mask != 0x3f01 || mask_calls != 2 ||
            reads != (mode != 0) || (reads && offset_read != offset)) return 0;
    }
    return 1;
}

int main(void) {
    uint8_t *rdram = calloc(1, 8u << 20);
    if (!rdram) return 1;
    int pass = check_probe(rdram, func_82604390, 0x800, 1, -128) &&
               check_probe(rdram, func_82700DD0, 0xd10, 0x400, -64) &&
               check_probe(rdram, func_80069750, 0x504, 0xa460, -32);
    free(rdram);
    if (!pass) return 1;
    puts("PASS: three generated US cartridge checks preserve busy, match and mismatch paths and restore the interrupt mask");
    return 0;
}
