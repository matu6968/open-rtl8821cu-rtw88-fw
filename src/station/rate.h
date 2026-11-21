/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_RATE_H
#define RTL8811CU_RATE_H
#include "station.h"

#define RATE_MAGIC 0x31415452u /* RTA1 */
#define RATE_MACIDS 128u

/* Observational ABI v1. Host commands, actual table readback and hardware
 * feedback are separate; an RA report is not a transmit ACK indication.
 */
struct rate_status {
    uint32_t magic, abi_version, struct_bytes, state;
    uint32_t commands, mask_updates, rssi_updates, media_updates;
    uint32_t bad_commands, unsupported_configs, last_macid, last_mask;
    uint32_t last_config, last_rssi, last_rate, last_bw;
    uint32_t programs, readback_errors, report_samples, report_ignored;
    uint32_t rate_up, rate_down, c2h_reports, c2h_busy;
    uint32_t last_successes, last_failures, last_retry, last_count;
    uint32_t last_h2c_lo, last_h2c_hi;
};
extern volatile struct rate_status g_rate;

void station_rate_init(void);
/* 0: unknown command; 1: applied; 2: recognized but rejected. */
uint32_t station_rate_h2c(uint32_t lo, uint32_t hi);
void station_rate_tx_report(const uint32_t report[4]);
void station_rate_poll(void);

/* Complete C2H frame including ID and sequence. 0 means defer, never block. */
uint32_t station_c2h_send(const uint8_t *frame, uint32_t length);
#endif
