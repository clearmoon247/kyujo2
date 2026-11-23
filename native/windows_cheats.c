#include "windows_cheats.h"
#include "recomp.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef KYUJO2_SHARED_MENU
#include "host_ui.h"
#define CHEAT_NOTICE(text) host_menu_notice(text)
#else
#define CHEAT_NOTICE(text) ((void)0)
#endif

static atomic_bool pending, chord_down, hp_down;
static atomic_uint last_enabled;
static _Thread_local bool player_move, rerun;

void stadium2_cheats_enabled(unsigned enabled) { atomic_store(&last_enabled, enabled); }

/* The battle overlay just loaded: say which cheats are active, as Kyujo does. */
void stadium2_begin_battle(void) {
    unsigned enabled = atomic_load(&last_enabled);
    fprintf(stderr, "Battle started (cheats %u)\n", enabled);
    if (getenv("KYUJO2_TEST_KNOCKOUT")) atomic_store(&pending, true);     /* tests: armed from the start */
    if ((enabled & 3) == 3)
        CHEAT_NOTICE("Cheats on: L + R wins the battle, Z restores HP");
    else if (enabled & 1)
        CHEAT_NOTICE("Cheat on: L + R wins the battle");
    else if (enabled & 2)
        CHEAT_NOTICE("Cheat on: Z restores your first Pokemon's HP");
}

void stadium2_end_battle(void) {
    atomic_store(&pending, false);
    atomic_store(&chord_down, false);
}

void stadium2_battle_cheats(uint8_t *rdram, unsigned enabled, uint16_t buttons) {
    bool chord = (enabled & 1) && (buttons & 0x0030) == 0x0030;
    bool held = atomic_exchange(&chord_down, chord);
    atomic_store(&last_enabled, enabled);
    if (!(enabled & 1))
        atomic_store(&pending, false);
    else if (chord && !held && !atomic_exchange(&pending, true))
        CHEAT_NOTICE("Cheat armed: you win after your next move");

    /* Mupen64Plus, US cartridge CRC 03571182-892FD06D. */
    bool hp = (enabled & 2) && (buttons & 0x2000);
    if (hp) {
        MEM_H(0, 0x8014528Eu) = 255;
        MEM_H(0, 0x80145290u) = 255;
    }
    if (hp && !atomic_exchange(&hp_down, true))
        CHEAT_NOTICE("Cheat: first Pokemon's HP set to 255");
    else if (!hp)
        atomic_store(&hp_down, false);
}

/* US fragment 79, guest PC 0x84128AC8. a1 indexes the defender (side * 4),
 * a0 points at its party record, v0 holds remaining HP and v1 holds damage.
 * Substitute damage takes a different guest branch before reaching this site. */
void stadium2_apply_knockout(uint8_t *rdram, recomp_context *ctx) {
    static int test_skip = -1;     /* KYUJO2_TEST_KNOCKOUT=2: ignore the first hit, as if the move had missed */
    if (test_skip < 0) { const char *t = getenv("KYUJO2_TEST_KNOCKOUT"); test_skip = t && t[0] == '2'; }
    if (ctx->r5 != 4 || !ctx->r2 || (int32_t)ctx->r3 <= 0 || !atomic_load(&pending))
        return;
    if (test_skip && !rerun) { test_skip = 0; return; }
    atomic_store(&pending, false);
    /* The game's following instructions cap damage at HP, write the actual
     * damage to its animation record, then subtract it and check for a faint. */
    ctx->r3 = ctx->r2 + 1;
    fprintf(stderr, "Knockout cheat applied (opponent HP %d)\n", (int)ctx->r2);
    CHEAT_NOTICE("Cheat: the opponent's team is knocked out");
    if (getenv("KYUJO2_TRACE_BATTLE")) {
        fprintf(stderr, "defender record %08x\n", (unsigned)ctx->r4);
        for (uint32_t base = 0x80145200u; base < 0x80145a00u; base += 0x58) {
            fprintf(stderr, "%08x:", base);
            for (uint32_t o = 0; o < 0x58; o += 2) fprintf(stderr, " %04x", (unsigned)(uint16_t)MEM_H(o, base));
            fputc('\n', stderr);
        }
    }
    /* The rest of the opponent's team faints too, so the battle ends. Its party records start at 0x80145748,
     * 0x58 bytes apart with HP at +0x26; the byte 0x10 before the first holds the team size (3, or 6 in a
     * six-Pokemon Free Battle). */
    const uint32_t first = 0x80145748u, stride = 0x58;
    uint32_t defender = (uint32_t)ctx->r4;
    if (defender < first || (defender - first) % stride || (defender - first) / stride >= 6)
        return;
    unsigned count = (uint8_t)MEM_BU(0, first - 0x10);
    if (count < 1 || count > 6) count = 3;
    for (unsigned slot = 0; slot < count; ++slot)
        if (first + slot * stride != defender)
            MEM_H(0x26, first + slot * stride) = 0;
}

/* Fragment 79's move-script dispatcher (func_841315C4) keeps its state next to the script buffer that s3 points to
 * (D_84195198): the command index at +0x20 (D_841951B8), the attacking side at +0x24 (D_841951BC, 0 = player) and
 * the damage word at +0x30 (D_841951C8). */

void stadium2_battle_command(uint8_t *rdram, recomp_context *ctx) {
    static int trace = -1;
    const gpr buffer = ctx->r19;
    if (trace < 0) trace = getenv("KYUJO2_TRACE_BATTLE") != NULL;
    if (MEM_W(0x20, buffer) == 0 && !rerun) player_move = MEM_BU(0x24, buffer) == 0;   /* a new move script */
    if (trace)
        fprintf(stderr, "battle cmd %02x index %d attacker %d\n", (unsigned)ctx->r16 & 0xff, (int)MEM_W(0x20, buffer),
                MEM_BU(0x24, buffer));
}

/* The player's move script ended without the armed knockout landing: it missed, did no damage or was not a damaging
 * move. Run the game's own damage, message and faint commands once more with some damage, so that the damage hook
 * above turns it into a knockout with the usual animation. */
int stadium2_knockout_rerun(uint8_t *rdram, recomp_context *ctx) {
    static const uint8_t finish[] = {0x09, 0x0a, 0x0b, 0xa2, 0x0c, 0x0d, 0xff};
    const gpr buffer = ctx->r19;
    if (rerun || !player_move || !atomic_load(&pending)) {
        rerun = false;
        player_move = false;
        return 0;
    }
    rerun = true;
    for (unsigned i = 0; i < sizeof finish; i++) MEM_B(i, buffer) = (int8_t)finish[i];
    MEM_W(0x20, buffer) = 0;
    MEM_W(0x30, buffer) = 1;
    ctx->r16 = finish[0];
    ctx->r15 = finish[0] << 2;
    return 1;
}
