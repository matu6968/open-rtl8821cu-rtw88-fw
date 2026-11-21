/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8821C_AP_H
#define RTL8821C_AP_H

#include <stdint.h>

uint32_t ap_port_active(void);
uint32_t ap_poll(void);
uint32_t ap_h2c(uint32_t lo, uint32_t hi);

#endif
