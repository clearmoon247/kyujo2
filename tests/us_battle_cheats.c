#include "windows_cheats.h"
#include "recomp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void stadium2_apply_knockout(uint8_t *rdram, recomp_context *ctx);
static uint8_t memory[0x146000], original[sizeof(memory)];
int main(void) {
    memset(memory, 0x55, sizeof(memory));
    memcpy(original, memory, sizeof(memory));
    recomp_context ctx = {0};
    ctx.r2 = 117;
    ctx.r3 = 14;
    ctx.r4 = 0x80145748;
    ctx.r5 = 4;
    stadium2_battle_cheats(memory, 0, 0x2030);
    stadium2_apply_knockout(memory, &ctx);
    assert(ctx.r3 == 14);
    stadium2_battle_cheats(memory, 1, 0x0030);
    assert(!memcmp(original, memory, sizeof(memory)));
    ctx.r5 = 0;
    stadium2_apply_knockout(memory, &ctx);
    assert(ctx.r3 == 14 && !memcmp(original, memory, sizeof(memory)));
    ctx.r5 = 4;
    ctx.r3 = 0;
    stadium2_apply_knockout(memory, &ctx);
    assert(!ctx.r3 && !memcmp(original, memory, sizeof(memory)));
    ctx.r3 = 14;
    stadium2_apply_knockout(memory, &ctx);
    assert(ctx.r3 == 118);
    uint16_t zero = 0;
    memcpy(original + (0x1457c6 ^ 2), &zero, 2);
    memcpy(original + (0x14581e ^ 2), &zero, 2);
    assert(!memcmp(original, memory, sizeof(memory)));
    ctx.r3 = 14;
    stadium2_battle_cheats(memory, 1, 0x0030);
    stadium2_apply_knockout(memory, &ctx);
    assert(ctx.r3 == 14);
    stadium2_battle_cheats(memory, 1, 0);
    stadium2_battle_cheats(memory, 1, 0x30);
    stadium2_end_battle();
    stadium2_apply_knockout(memory, &ctx);
    assert(ctx.r3 == 14);
    stadium2_battle_cheats(memory, 2, 0x2000);
    uint16_t hp = 255;
    memcpy(original + (0x14528e ^ 2), &hp, 2);
    memcpy(original + (0x145290 ^ 2), &hp, 2);
    assert(!memcmp(original, memory, sizeof(memory)));
    puts("PASS: input leaves party and model state intact, next opponent hit uses the guest faint path, bench "
         "and first-slot HP are isolated");
}
