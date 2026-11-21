/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_STATION_H
#define RTL8811CU_STATION_H
#include "../interrupts/foundation.h"
#define STATION_MAGIC 0x31415453u /* STA1 */
#define STATION_IQK_TIMEOUT (1u << 4)
struct station_status {
    uint32_t magic, abi_version, struct_bytes, state;
    uint32_t polls, packet_count, packet_bad, packet_unsupported;
    uint32_t head, tail, read, write, stride, wraps;
    uint32_t last_id, last_seq, general_count, fw_boundary;
    uint32_t phydm_count, rfe, rf_type, cut, antenna;
    uint32_t iqk_requests, iqk_unsupported, last_iqk;
    uint32_t iqk_completed, iqk_access_errors, iqk_fail_mask, iqk_duration_us, iqk_stage;
    uint32_t tx_reports, tx_forwarded, tx_other, c2h_busy, c2h_bad_ptr, c2h_seq;
    uint32_t last_tx_report[4];
    uint32_t last_packet[8];
};
extern volatile struct station_status g_station;
void station_init(void);
void station_poll(void);
void station_packet_poll(void);
void station_packet(const uint32_t *packet);
uint32_t station_iqk(uint32_t clear);
uint32_t station_iqk_doze_restore(void);
void station_tx_report_poll(void);
void station_tx_report_start(void);
#endif
