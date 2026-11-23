#include "recomp.h"
#include "funcs.h"
#include <stdio.h>
#include <string.h>

static int32_t bases[128];
int32_t *section_addresses = bases;

void func_800226C0(uint8_t *rdram, recomp_context *ctx) {}
void func_8004C8C0(uint8_t *rdram, recomp_context *ctx) { ctx->r2 = 1; }
void _bzero(uint8_t *rdram, recomp_context *ctx) {
    memset(rdram + stadium2_rdram_offset(ctx->r4), 0, ctx->r5);
}

static int check_table(uint8_t *rdram, uint32_t base) {
    /* The English fragment 20 loader relocated both bounds in this array loop. */
    bases[65] = (int32_t)base;
    const uint32_t table = base + 0x163e8;
    recomp_context ctx = {0};
    ctx.r29 = (gpr)(int32_t)0x80008000;
    MEM_W(0, table + 8 * 0x814) = 0x12345678;
    func_826040D8(rdram, &ctx);
    for (int i = 0; i < 8; ++i) {
        if (MEM_H(0, table + i * 0x814) != -1) return 0;
    }
    return ctx.r2 == 1 && MEM_W(0, table + 8 * 0x814) == 0x12345678;
}

int main(void) {
    uint8_t *rdram = calloc(1, 8u << 20);
    if (!rdram) return 1;
    int pass = check_table(rdram, 0x8014ae60) && check_table(rdram, 0x80332110);
    free(rdram);
    if (!pass) return 1;
    puts("PASS: eight English cup entries initialized at both overlay bases; end guard intact");
    return 0;
}
