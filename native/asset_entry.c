#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>

/* Packed scene assets carry a short MIPS accessor outside the 88 code overlays.
 * Match the full accessor and its relocated entry jump before dispatching it. */
static _Thread_local int32_t asset_entry;
int stadium2_unrecompiled_fragment(uint32_t base);

static void get_asset_data(uint8_t *rdram, recomp_context *ctx) {
    gpr entry = (gpr)asset_entry;
    MEM_W(4, ctx->r29) = ctx->r5;
    ctx->r3 = 0;
    if (ctx->r4 == 0) {
        uint32_t upper = (uint32_t)MEM_W(0x2c, entry) << 16;
        ctx->r3 = ADD32(upper, (int16_t)MEM_W(0x30, entry));
    }
    ctx->r2 = ctx->r3;
}

static void get_asset_info(uint8_t *rdram, recomp_context *ctx) {
    gpr entry = (gpr)asset_entry;
    MEM_W(4, ctx->r29) = ctx->r5;
    if (ctx->r4 == 0) {
        ctx->r3 = S32((uint32_t)MEM_W(0x3c, entry) << 16);
        ctx->r2 = ADD32(ctx->r3, (int16_t)MEM_W(0x44, entry));
    } else {
        ctx->r1 = 1;
        ctx->r3 = 1;
        ctx->r2 = ctx->r4 == 1;
    }
}


/* Packed assets also carry accessors of other shapes (Free Battle loads one when the battle starts). They are
 * short leaf routines that the guest loader has already relocated in RAM, so run them with a small interpreter
 * instead of matching every variant. Anything outside the supported subset stops with a description. */
static void run_asset_accessor(uint8_t *rdram, recomp_context *ctx) {
    gpr *r = &ctx->r0;
    uint32_t pc = (uint32_t)asset_entry + 32, branch = 0;
    int pending = 0;
    for (unsigned steps = 0; steps < 100000; steps++) {
        const uint32_t w = (uint32_t)MEM_W(0, (gpr)(int32_t)pc);
        const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
        const int32_t simm = (int16_t)(w & 0xFFFF);
        const uint32_t uimm = w & 0xFFFF;
        uint32_t next = pc + 4, target = 0;
        int take = 0, likely = 0, ret = 0;
#define S32V(v) ((gpr)(int32_t)(v))
#define BR(cond) do { take = (cond); target = pc + 4 + ((uint32_t)simm << 2); } while (0)
        switch (op) {
        case 0x00:
            switch (w & 63) {
            case 0x00: if (rd) r[rd] = S32V((uint32_t)r[rt] << sa); break;
            case 0x02: if (rd) r[rd] = S32V((uint32_t)r[rt] >> sa); break;
            case 0x03: if (rd) r[rd] = S32V((int32_t)r[rt] >> sa); break;
            case 0x04: if (rd) r[rd] = S32V((uint32_t)r[rt] << (r[rs] & 31)); break;
            case 0x06: if (rd) r[rd] = S32V((uint32_t)r[rt] >> (r[rs] & 31)); break;
            case 0x07: if (rd) r[rd] = S32V((int32_t)r[rt] >> (r[rs] & 31)); break;
            case 0x08: if (rs == 31) ret = 1; else goto unsupported; break;
            case 0x21: case 0x20: if (rd) r[rd] = S32V((uint32_t)r[rs] + (uint32_t)r[rt]); break;
            case 0x23: case 0x22: if (rd) r[rd] = S32V((uint32_t)r[rs] - (uint32_t)r[rt]); break;
            case 0x24: if (rd) r[rd] = r[rs] & r[rt]; break;
            case 0x25: if (rd) r[rd] = r[rs] | r[rt]; break;
            case 0x26: if (rd) r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: if (rd) r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2A: if (rd) r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
            case 0x2B: if (rd) r[rd] = (uint32_t)r[rs] < (uint32_t)r[rt]; break;
            default: goto unsupported;
            }
            break;
        case 0x01:
            if (rt == 0 || rt == 2) BR((int32_t)r[rs] < 0);
            else if (rt == 1 || rt == 3) BR((int32_t)r[rs] >= 0);
            else goto unsupported;
            likely = rt >= 2;
            break;
        case 0x02: take = 1; target = (pc & 0xF0000000u) | ((w & 0x03FFFFFF) << 2); break;
        case 0x04: case 0x14: BR(r[rs] == r[rt]); likely = op == 0x14; break;
        case 0x05: case 0x15: BR(r[rs] != r[rt]); likely = op == 0x15; break;
        case 0x06: case 0x16: BR((int32_t)r[rs] <= 0); likely = op == 0x16; break;
        case 0x07: case 0x17: BR((int32_t)r[rs] > 0); likely = op == 0x17; break;
        case 0x08: case 0x09: if (rt) r[rt] = S32V((uint32_t)r[rs] + (uint32_t)simm); break;
        case 0x0A: if (rt) r[rt] = (int32_t)r[rs] < simm; break;
        case 0x0B: if (rt) r[rt] = (uint32_t)r[rs] < (uint32_t)simm; break;
        case 0x0C: if (rt) r[rt] = r[rs] & uimm; break;
        case 0x0D: if (rt) r[rt] = r[rs] | uimm; break;
        case 0x0E: if (rt) r[rt] = r[rs] ^ uimm; break;
        case 0x0F: if (rt) r[rt] = S32V(uimm << 16); break;
        case 0x20: if (rt) r[rt] = MEM_B(simm, r[rs]); break;
        case 0x21: if (rt) r[rt] = MEM_H(simm, r[rs]); break;
        case 0x23: if (rt) r[rt] = MEM_W(simm, r[rs]); break;
        case 0x24: if (rt) r[rt] = MEM_BU(simm, r[rs]); break;
        case 0x25: if (rt) r[rt] = MEM_HU(simm, r[rs]); break;
        case 0x28: MEM_B(simm, r[rs]) = (int8_t)r[rt]; break;
        case 0x29: MEM_H(simm, r[rs]) = (int16_t)r[rt]; break;
        case 0x2B: MEM_W(simm, r[rs]) = (int32_t)r[rt]; break;
        default:
        unsupported:
            fprintf(stderr, "Asset accessor at %08x: unsupported instruction %08x at %08x\n", (uint32_t)asset_entry, w, pc);
            abort();
        }
#undef BR
#undef S32V
        if (pending) {                          /* this instruction was a delay slot */
            if (pending == 2) return;
            pc = branch;
            pending = 0;
            continue;
        }
        if (ret) { pending = 2; pc = next; continue; }
        if (take) { pending = 1; branch = target; pc = next; continue; }
        if (likely) next += 4;                  /* untaken likely branch skips its delay slot */
        pc = next;
    }
    fprintf(stderr, "Asset accessor at %08x did not return\n", (uint32_t)asset_entry);
    abort();
}

recomp_func_t *kyujo2_lookup(uint8_t *rdram, int32_t pc) {
    uint32_t address = (uint32_t)pc;
    if (address >= 0x80134470 && address <= 0x807fffc4 && !(address & 3)) {
        gpr entry = (gpr)pc;
        if ((uint32_t)MEM_W(8, entry) != 0x46524147 || (uint32_t)MEM_W(12, entry) != 0x4d454e54 ||
            (uint32_t)MEM_W(0, entry) != (0x08000000 | (((address + 32) >> 2) & 0x03ffffff)) ||
            MEM_W(4, entry) != 0) return get_function(pc);
        static int force = -1;      /* KYUJO2_INTERPRET_ASSETS=1: test the interpreter on every accessor */
        if (force < 0) force = getenv("KYUJO2_INTERPRET_ASSETS") != NULL;
        if (force && stadium2_unrecompiled_fragment(address)) {
            asset_entry = pc;
            return run_asset_accessor;
        }
        if ((uint32_t)MEM_W(0x20, entry) == 0xafa50004 &&
            MEM_W(0x24, entry) == 0x14800003 && MEM_W(0x28, entry) == 0x00001825 &&
            ((uint32_t)MEM_W(0x2c, entry) >> 16) == 0x3c03 &&
            ((uint32_t)MEM_W(0x30, entry) >> 16) == 0x2463 &&
            MEM_W(0x34, entry) == 0x03e00008 && MEM_W(0x38, entry) == 0x00601025) {
            asset_entry = pc;
            return get_asset_data;
        }
        if (address <= 0x807fffb0 && MEM_W(0x20, entry) == 0x10800006 &&
            (uint32_t)MEM_W(0x24, entry) == 0xafa50004 && MEM_W(0x28, entry) == 0x24010001 &&
            MEM_W(0x2c, entry) == 0x10810006 && MEM_W(0x30, entry) == 0x24030001 &&
            MEM_W(0x34, entry) == 0x03e00008 && MEM_W(0x38, entry) == 0x00001025 &&
            (uint32_t)MEM_W(0x3c, entry) >> 16 == 0x3c03 && MEM_W(0x40, entry) == 0x03e00008 &&
            (uint32_t)MEM_W(0x44, entry) >> 16 == 0x2462 && MEM_W(0x48, entry) == 0x03e00008 &&
            MEM_W(0x4c, entry) == 0x00601025) {
            asset_entry = pc;
            return get_asset_info;
        }
        if (stadium2_unrecompiled_fragment(address)) {
            asset_entry = pc;
            return run_asset_accessor;
        }
    }
    return get_function(pc);
}
