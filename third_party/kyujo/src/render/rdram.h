/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * rdram.h - how the renderer files (rdp.c, vi_out.c) turn a game address into a host pointer. HOST-side.
 *
 * In the native builds (Linux, Windows) the game runs in the same process and an address IS a pointer. In the sandboxed core (docs/switch-android-plan.md)
 * the renderer is compiled into the native shell (RDP_SHELL_NATIVE) and the game's memory is the WebAssembly module's linear memory: an address is an
 * offset into it. gRdpMem is the start of that memory; the shell sets it (shell/core/rdp_glue.c) before every call into the renderer.
 */
#ifndef RDRAM_H
#define RDRAM_H

#include <stdint.h>

#ifdef RDP_SHELL_NATIVE
extern uint8_t *gRdpMem;                       /* start of the core's memory */
extern uint32_t gRdpMemSize;                   /* its size in bytes */
unsigned long long host_now_ns(void);
void host_game_range_bounds(unsigned int *lo, unsigned int *hi);
int host_game_range_ok(unsigned int addr, unsigned int len);
#define RDRAM_PTR(a) (gRdpMem + (uintptr_t)(a))
#else
#include "platform.h"
#define RDRAM_PTR(a) ((uint8_t *)(uintptr_t)(a))
#endif

#endif
