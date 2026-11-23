#include "recomp.h"
#include "funcs.h"
#include <stdio.h>

/* English fragment 50 uses its own BSS and a list in fragment 64 in the
 * same function. Move the two independently to detect the wrong base. */
static int32_t bases[256];
int32_t *section_addresses = bases;
static uint32_t parent;
static unsigned count;
void func_8004DDF4(uint8_t *rdram, recomp_context *ctx) {}
void func_80002B24(uint8_t *rdram, recomp_context *ctx) {}
void func_800030D0(uint8_t *rdram, recomp_context *ctx) {}
void func_8004DF18(uint8_t *rdram, recomp_context *ctx) {}
void func_80003120(uint8_t *rdram, recomp_context *ctx) {}
void func_87F0252C(uint8_t *rdram, recomp_context *ctx) {
    parent = ctx->r4;
    count = ctx->r7;
}

static int check_parent(uint8_t *rdram, uint32_t caller, uint32_t owner) {
    recomp_context ctx = {0};
    bases[155] = (int32_t)caller;
    bases[197] = (int32_t)owner;
    MEM_W(0xE198, caller) = (int32_t)0x80210000;
    ctx.r29 = (gpr)(int32_t)0x80008000;
    parent = count = 0;
    func_862075BC(rdram, &ctx);
    return parent == owner + 0x15B70 && count == 8 &&
           (uint32_t)ctx.r29 == 0x80008000;
}

int main(void) {
    uint8_t *rdram = calloc(1, 8u << 20);
    if (!rdram) return 1;
    int pass = check_parent(rdram, 0x80167780, 0x80145B20) &&
               check_parent(rdram, 0x80300200, 0x80403000);
    free(rdram);
    if (!pass) return 1;
    puts("PASS: fragment 50 passes the list address from the loaded fragment 64 at both bases");
    return 0;
}
