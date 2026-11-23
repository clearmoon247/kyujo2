#include "recomp.h"
#include "funcs.h"
#include <stdio.h>

static const uint32_t state_address = 0x800aa9e0;
static unsigned yields;

/* A second guest thread signals this counter in the game. */
void yield_self_1ms(uint8_t *rdram) {
    ++yields;
    MEM_W(0xa2c, state_address) = 1;
}

int main(void) {
    uint8_t *rdram = calloc(1, 8u << 20);
    if (!rdram) return 1;
    recomp_context ctx = {0};
    const gpr initial_sp = (gpr)(int32_t)0x80008000;
    ctx.r29 = initial_sp;
    ctx.r31 = 0x1234;
    MEM_W(0xaa4, state_address) = 1;
    func_800085EC(rdram, &ctx);
    if (yields != 1 || ctx.r2 != 1 || ctx.r29 != initial_sp || ctx.r31 != 0x1234) return 1;
    func_800085EC(rdram, &ctx);
    if (yields != 1 || ctx.r2 != 1) return 1;
    free(rdram);
    puts("PASS: generated US wait yields while busy and returns immediately when ready");
}
