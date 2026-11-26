/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8821CU_COEX_H
#define RTL8821CU_COEX_H

#include <stdint.h>

#ifdef RTL8821CU_COEX_BOARD
uint32_t coex_configure(void);
uint32_t coex_h2c(uint32_t lo, uint32_t hi);
void coex_poll(void);
void coex_transport_lost(void);
#else
static inline uint32_t coex_configure(void) { return 0; }
static inline void coex_poll(void) {}
static inline void coex_transport_lost(void) {}
#endif

#endif
