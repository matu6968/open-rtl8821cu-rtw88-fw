/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_LPS_H
#define RTL8811CU_LPS_H
#include "../interrupts/foundation.h"

#define LPS_MAGIC 0x3153504cu /* LPS1 */
enum lps_state { LPS_ACTIVE, LPS_WAIT_QUEUE, LPS_WAIT_ACK, LPS_AP_PS, LPS_FAILED };
enum lps_error { LPS_OK, LPS_NO_TEMPLATE, LPS_BAD_TEMPLATE, LPS_QUEUE_TIMEOUT,
    LPS_NULL_FAILED, LPS_NULL_TIMEOUT, LPS_READBACK, LPS_RADIO_OFF };

/* PM transitions only: RF, MAC and CPU remain running. In particular AP_PS
 * means that a PM=1 null was acknowledged; it is not an RF-doze assertion.
 */
struct lps_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t commands, reserved_updates, rejected, already_active;
    uint32_t requested_mode, rlbm, smart_ps, awake_interval, port;
    uint32_t null_page, qos_null_page, page_valid, tx_address;
    uint32_t null0_submissions, null1_submissions, null0_acks, null1_acks;
    uint32_t null_failures, null_timeouts, queue_timeouts, readback_errors;
    uint32_t recoveries, stale_events, last_isr, last_null_status;
    uint32_t last_tcr, last_cr, deadline, tx_pm;
    uint32_t beacon_events, tim_unicast, dtim_multicast, tim_wakes;
    uint32_t last_lo, last_hi, cancelled, ap_ps;
};
extern volatile struct lps_status g_lps;
void station_lps_init(void);
/* 0 unknown, 1 accepted (possibly asynchronous), 2 rejected. Accepts the
 * ordinary rtw88 port-0 register H2C 0x20 and reserved-page locations 0x00.
 * Host box release remains the caller's responsibility.
 */
uint32_t station_lps_h2c(uint32_t lo, uint32_t hi);
void station_lps_poll(void);
/* Optional future integration: supply exactly once per fresh associated-BSS
 * beacon. A periodic read of a stale BCN_PSR_RPT is not a beacon event.
 * With RF awake a TIM notification can recover buffered traffic by PM=0;
 * this is not the stock PS-Poll/DTIM sleep scheduler.
 */
void station_lps_beacon(uint32_t psr);
/* Reset the station state after disconnect, MAC shutdown, or an AP role
 * change after draining any outstanding station TX. It performs no MMIO.
 */
void station_lps_cancel(void);
void station_lps_request_active(void);
#endif
