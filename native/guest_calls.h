#ifndef STADIUM2_GUEST_CALLS_H
#define STADIUM2_GUEST_CALLS_H
#include "recomp.h"
recomp_func_t *kyujo2_lookup(uint8_t *rdram, int32_t pc);
#undef LOOKUP_FUNC
#define LOOKUP_FUNC(addr) kyujo2_lookup(rdram, (int32_t)(addr))

/* Calls omitted from the generator's default prototype list. */
recomp_func_t osDpGetCounters_recomp, osDpSetStatus_recomp, osViRepeatLine_recomp, osPfsIsPlug_recomp;
recomp_func_t __osContRamRead_recomp, __osContRamWrite_recomp, __osPfsGetStatus_recomp;
#ifdef KYUJO2_JP
#define func_800214C0 func_800213E0
#define func_800215B4 func_800214D4
#define func_80021834 func_80021754
#endif
recomp_func_t set_watch_lohi, func_800214C0, func_800215B4, func_80021834;
gpr stadium2_pi_status(uint8_t *rdram, recomp_context *ctx);
uint32_t stadium2_read_cart_word(uint32_t offset);
recomp_func_t osAiSetNextBuffer_recomp;
void stadium2_ra_push(gpr *ra, gpr value);
int stadium2_ra_pop(void);
int stadium2_ra_is_return(gpr ra, int32_t base, uint32_t size);
int stadium2_ra_returns_to(gpr value);
uint32_t stadium2_gb_sp_status(uint8_t *rdram);
void stadium2_gb_sp_clear(void);
void stadium2_gb_core_tick(uint8_t *rdram, recomp_context *ctx);
/* A tail call in the Game Boy core (fragment 89), whose handlers chain indefinitely. Clang merges such a call with an
 * identical ordinary call in the other branch and loses the jump, so it is required there. GCC 14 (Windows) keeps the
 * jump without being told; GCC 15 and later (Switch) can be told. */
#if defined(__clang__)
#define STADIUM2_TAIL(f) do { _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wpedantic\"") \
    __attribute__((musttail)) return (f)(rdram, ctx); _Pragma("clang diagnostic pop") } while (0)
#elif defined(__GNUC__) && __GNUC__ >= 15
#define STADIUM2_TAIL(f) do { __attribute__((musttail)) return (f)(rdram, ctx); } while (0)
#else
#define STADIUM2_TAIL(f) do { (f)(rdram, ctx); return; } while (0)
#endif
static inline int stadium2_in_fragment(gpr address, int32_t base, uint32_t size) {
    return (uint32_t)address >= (uint32_t)base && (uint32_t)address - (uint32_t)base < size;
}
void yield_self_1ms(uint8_t *rdram);
void yield_to_ready(uint8_t *rdram);
recomp_func_t stadium2_fragment_load, stadium2_fragment_free, stadium2_pool_alloc;
void stadium2_battle_command(uint8_t *rdram, recomp_context *ctx);
int stadium2_knockout_rerun(uint8_t *rdram, recomp_context *ctx);
#endif
