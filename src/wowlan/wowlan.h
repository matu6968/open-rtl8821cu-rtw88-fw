/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_WOWLAN_H
#define RTL8811CU_WOWLAN_H
#include "../station/station.h"
#define WOWLAN_MAGIC 0x31574f57u /* WOW1 */
#define WOWLAN_H2C 0xf0u
#define WOWLAN_SIGNATURE 0x31574f57u
enum wowlan_state { WOWLAN_IDLE, WOWLAN_ARMING, WOWLAN_ARMED,
    WOWLAN_TRIGGERED, WOWLAN_DISARMING, WOWLAN_FAILED };
enum wowlan_error { WOWLAN_OK, WOWLAN_BAD_COMMAND, WOWLAN_BAD_CONTEXT,
    WOWLAN_BUSY, WOWLAN_READBACK, WOWLAN_DMA_TIMEOUT, WOWLAN_BAD_RING,
    WOWLAN_CONTEXT_LOST, WOWLAN_RELEASE_TIMEOUT };
struct wowlan_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t commands, rejected, arms, disarms, cancels, polls;
    uint32_t owned, cookie, ack, deadline, release_deadline;
    uint32_t rx_packets, rx_released, rx_bad, magic_matches, wake_requests;
    uint32_t wake_reason, wake_time_us, readback_errors, timeouts;
    uint32_t last_lo, last_hi, last_ctrl, last_rxpkt;
    uint32_t rx_end, rx_read, rx_write, rx_next, release_pending;
    uint32_t last_descriptor[6];
    uint32_t saved_ctrl, saved_release, mac_lo, mac_hi, bssid_lo, bssid_hi, aid;
};
extern volatile struct wowlan_status g_wowlan;
void station_wowlan_init(void);
/* Only private unified-image F0 commands. Acceptance is distinct from ACK. */
uint32_t station_wowlan_h2c(uint32_t lo, uint32_t hi);
/* Poll first; suppress normal RX FIFO/C2H/LPS producers while owned. */
uint32_t station_wowlan_pre(void);
uint32_t station_wowlan_owned(void);
#endif
