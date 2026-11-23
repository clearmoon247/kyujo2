#include "guest_calls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t fallback_pc;

recomp_func_t *get_function(int32_t pc) {
    fallback_pc = (uint32_t)pc;
    return NULL;
}

int main(void) {
    uint8_t *rdram = calloc(1, 8u << 20);
    assert(rdram);
    const int32_t pc = (int32_t)0x806788e0;
    const gpr entry = (gpr)pc;
    const uint32_t header[] = {
        0x0819e240, 0, 0x46524147, 0x4d454e54,
        0, 0, 0, 0, 0xafa50004, 0x14800003, 0x00001825, 0x3c038068,
        0x24635c80, 0x03e00008, 0x00601025
    };
    memcpy(&MEM_W(0, entry), header, sizeof(header));
    recomp_context ctx = {0};
    ctx.r29 = (gpr)(int32_t)0x80004000;
    ctx.r5 = 0x12345678;
    recomp_func_t *accessor = kyujo2_lookup(rdram, pc);
    assert(accessor);
    accessor(rdram, &ctx);
    assert(ctx.r2 == (gpr)(int32_t)0x80685c80 && ctx.r3 == ctx.r2);
    assert(MEM_W(4, ctx.r29) == ctx.r5);
    MEM_W(0x30, entry) = 0x2463abcd;
    accessor(rdram, &ctx);
    assert(ctx.r2 == (gpr)(int32_t)0x8067abcd);
    ctx.r4 = 1;
    ctx.r5++;
    accessor(rdram, &ctx);
    assert(ctx.r2 == 0 && ctx.r3 == 0 && MEM_W(4, ctx.r29) == ctx.r5);
    MEM_W(0x34, entry) = 0;
    assert(!kyujo2_lookup(rdram, pc) && fallback_pc == (uint32_t)pc);
    assert(!kyujo2_lookup(rdram, pc + 1));
    assert(!kyujo2_lookup(rdram, (int32_t)0x807ffffc));
    assert(!kyujo2_lookup(rdram, (int32_t)0xa06788e0));
    const int32_t scene = (int32_t)0x80262ce0;
    const uint32_t scene_dump[] = {
        0x08098b40, 0, 0x46524147, 0x4d454e54, 0x20, 0xc890, 0xcba0, 0xc890,
        0x10800006, 0xafa50004, 0x24010001, 0x10810006, 0x24030001, 0x03e00008,
        0x00001025, 0x3c038027, 0x03e00008, 0x2462f398, 0x03e00008, 0x00601025
    };
    memcpy(&MEM_W(0, (gpr)scene), scene_dump, sizeof(scene_dump));
    accessor = kyujo2_lookup(rdram, scene);
    assert(accessor);
    for (unsigned selector = 0; selector < 3; ++selector) {
        ctx.r1 = 19;
        ctx.r4 = selector;
        ctx.r5 = selector + 2;
        accessor(rdram, &ctx);
        assert(MEM_W(4, ctx.r29) == ctx.r5);
        assert(ctx.r1 == (selector ? 1 : 19));
        assert(ctx.r3 == (selector ? 1 : (gpr)(int32_t)0x80270000));
        assert(ctx.r2 == (selector ? selector == 1 : (gpr)(int32_t)0x8026f398));
    }
    MEM_W(0x48, (gpr)scene) = 0;
    assert(!kyujo2_lookup(rdram, scene));
    free(rdram);
    puts("PASS: both packed asset routines, all selectors, relocated pointers, register and stack effects, and fallback");
}
