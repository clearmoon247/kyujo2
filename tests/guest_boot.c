#include "recomp.h"
#include "funcs.h"
#include <stdio.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

static const unsigned bss_begin = 0xA7E80;
static const unsigned bss_end = 0x134470;
static unsigned main_calls;
static int target_ok;

static void record_main(uint8_t *rdram, recomp_context *ctx) {
    main_calls++;
    target_ok = ctx->r29 == (gpr)(int32_t)0x800A8280;
}
recomp_func_t *get_function(int32_t vram) {
    if ((uint32_t)vram != 0x80001530) {
        fprintf(stderr, "Unexpected guest target %08x\n", (unsigned)vram);
        abort();
    }
    return record_main;
}
void switch_error(const char *function, uint32_t pc, uint32_t table) {
    fprintf(stderr, "%s: invalid dispatch at %08x through %08x\n", function, pc, table);
    abort();
}

int test_guest(FILE *log) {
    uint8_t *rdram = malloc(8u << 20);
    if (!rdram) return 1;
    memset(rdram, 0xA5, 8u << 20);
    recomp_context ctx = {0};
    ctx.f_odd = &ctx.f0.u32h;
    recomp_entrypoint(rdram, &ctx);
    fprintf(log, "Guest PC %08x   SP %08x\n", (unsigned)ctx.r10, (unsigned)ctx.r29);
    int ok = main_calls == 1 && target_ok;
    for (unsigned i = 0; i < (8u << 20); i++) {
        unsigned expected = i >= bss_begin && i < bss_end ? 0 : 0xA5;
        if (rdram[i] != expected) {
            fprintf(log, "Unexpected guest RAM byte at %08x\n", 0x80000000u + i);
            ok = 0;
            break;
        }
    }
    ctx.f12.fl = -2.75f;
    func_8000B770(rdram, &ctx);
    ok &= ctx.f0.fl == 2.75f;
    /* Default, first case, interleaved default, final case, out-of-range. */
    const unsigned inputs[] = {0, 0x33, 0x34, 0x5F, 0x60};
    const unsigned results[] = {0, 2, 1, 3, 1};
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); i++) {
        ctx.r4 = inputs[i];
        func_80064980(rdram, &ctx);
        if (ctx.r2 != results[i]) {
            fprintf(log, "Dispatch input %02x returned %u\n", inputs[i], (unsigned)ctx.r2);
            ok = 0;
        }
    }
    /* Replay the halfword copy from the first full-game fault at 0x800117E0. */
    for (int i = 0; i < 8; i++) MEM_H(2*i, (gpr)(int32_t)0x8008D660) = 0x1200 + i;
    ctx.r4 = (gpr)(int32_t)0xA00E9160;
    ctx.r5 = 0;
    func_800117B8(rdram, &ctx);
    for (int i = 0; i < 8; i++)
        ok &= MEM_H(2*i, (gpr)(int32_t)0x800E9160) == 0x1200 + i;
    free(rdram);
    fprintf(log, "%s\n", ok ? "PASS: BSS, Main handoff, floating-point entry, jump table, uncached RDRAM." : "FAIL");
    return ok ? 0 : 1;
}

#ifndef __ANDROID__
int main(void) {
#ifdef __SWITCH__
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
#endif
    puts("Kyujo2 US - native boot regression r2");
    int status = test_guest(stdout);
#ifdef __SWITCH__
    puts("Press + to exit.");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
#endif
    return status;
}
#endif
