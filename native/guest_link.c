#include <stdio.h>
#include <stdlib.h>
#include "guest_calls.h"

/* libultra's osDpGetCounters copies the four RDP performance counters (clock, buffer, pipe and
 * TMEM busy cycles) to a0. The software renderer has no such counters. The Game Boy core in
 * fragment 89 only samples them for its own timing statistics. */
void osDpGetCounters_recomp(uint8_t *rdram, recomp_context *ctx) {
    for (unsigned i = 0; i < 4; i++) MEM_W(4 * i, ctx->r4) = 0;
}

/* The Game Boy core uses $ra as a general register, so N64Recomp's C returns are not enough. A call writes
 * the return address it would have placed in $ra and remembers it for the duration of the C call. A jump
 * through $ra is a plain return when it matches that address. Another address inside the fragment is a code
 * address that the routine stored itself; an address outside it is the stale value of an entry from resident
 * code and also ends the routine. */
#define LINK_DEPTH 1024
static _Thread_local uint32_t link_stack[LINK_DEPTH];
static _Thread_local unsigned link_depth;
/* The core also leaves a routine from inside nested calls: it saves $ra in its state, returns to the scene and
 * later jumps back through the saved value. A jump to the return address of a call further down the stack unwinds
 * to that call: every C frame above it returns as soon as its own call comes back. */
static _Thread_local int unwinding;
static _Thread_local uint32_t unwind_target;

void stadium2_ra_push(gpr *ra, gpr value) {
    *ra = value;
    if (link_depth < LINK_DEPTH) link_stack[link_depth] = (uint32_t)value;
    link_depth++;
}

/* After a call returns. Nonzero: keep returning, an outer call is the destination. */
int stadium2_ra_pop(void) {
    if (!link_depth) return unwinding = 0;
    link_depth--;
    if (unwinding && link_depth < LINK_DEPTH && link_stack[link_depth] == unwind_target) unwinding = 0;
    return unwinding;
}

/* A jump through `value` that returns to a pending call (the latest or a deeper one). */
int stadium2_ra_returns_to(gpr value) {
    const uint32_t target = (uint32_t)value;
    unsigned top = link_depth < LINK_DEPTH ? link_depth : LINK_DEPTH;
    for (unsigned i = top; i-- > 0;) {
        if (link_stack[i] != target) continue;
        if (i + 1 != link_depth) { unwinding = 1; unwind_target = target; }
        return 1;
    }
    return 0;
}

int stadium2_ra_is_return(gpr ra, int32_t base, uint32_t size) {
    if (stadium2_ra_returns_to(ra)) return 1;
    /* Not what a call left: a code address of this fragment that the routine chose itself is a jump. Anything
     * else (the stale value of an entry from resident code) is the end of the routine. */
    const uint32_t target = (uint32_t)ra;
    return !(target >= (uint32_t)base && target - (uint32_t)base < size);
}

/* SP_STATUS signal 3 for the Game Boy core: its RSP program sets the signal as its last action. The runtime runs
 * RSP tasks on its own thread, so the core waits here, yielding to the other guest threads, until the host has
 * finished the task. A missing task does not stall the core for more than a moment. */
static _Atomic int gb_rsp_signal;
unsigned long long host_now_ns(void);

void stadium2_gb_rsp_finished(void) { gb_rsp_signal = 1; }

uint32_t stadium2_gb_sp_status(uint8_t *rdram) {
    const unsigned long long until = host_now_ns() + 250000000ULL;
    while (!gb_rsp_signal && host_now_ns() < until) yield_to_ready(rdram);
    static unsigned waits, timeouts;
    waits++;
    if (!gb_rsp_signal) timeouts++;
    if (getenv("KYUJO2_TRACE_GB") && (waits % 64) == 0)
        fprintf(stderr, "GB core SP_STATUS waits %u, timed out %u\n", waits, timeouts);
    return 0x400;
}

void stadium2_gb_sp_clear(void) { gb_rsp_signal = 0; }

/* Called on every instruction the Game Boy core dispatches: lets the other guest threads (the Transfer Pak loader,
 * audio, the scheduler) run about every few milliseconds of emulation. */
void stadium2_gb_core_tick(uint8_t *rdram, recomp_context *ctx) {
    static _Thread_local unsigned count, total;
    static int trace = -1;
    if (trace < 0) trace = getenv("KYUJO2_TRACE_GB") != NULL;
    if (trace && (++total & (total < (40u << 20) ? 0xFFFFF : 0x3FF)) == 0 && total < (40u << 20) + 200000)
        fprintf(stderr, "GB core: %u M instructions, PC %04x\n", total >> 20, (unsigned)ctx->r22 & 0xFFFF);
    if (++count >= 65536) {
        count = 0;
        yield_to_ready(rdram);
    }
}
