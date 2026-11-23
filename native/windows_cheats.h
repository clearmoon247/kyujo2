#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void stadium2_end_battle(void);
void stadium2_begin_battle(void);
void stadium2_cheats_enabled(unsigned enabled);
void stadium2_battle_cheats(uint8_t *rdram, unsigned enabled, uint16_t buttons);
#ifdef __cplusplus
}
#endif
