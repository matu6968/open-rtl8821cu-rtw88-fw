/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8821C_FWLOG_H
#define RTL8821C_FWLOG_H

#include <stdint.h>

#define FW_LOG_ALWAYS 0u
#define FW_LOG_RA (1u << 1)

void fw_log(uint32_t component, const char *format, ...);
uint32_t fw_log_h2c(uint32_t lo, uint32_t hi);
void fw_log_poll(void);
void fw_log_transport_start(void);
void fw_log_transport_lost(void);

#endif
