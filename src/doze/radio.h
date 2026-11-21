/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_DOZE_RADIO_H
#define RTL8811CU_DOZE_RADIO_H
#include "../interrupts/foundation.h"
#define RADIO_MAGIC 0x314f4452u /* RDO1 */
enum radio_state { RADIO_AWAKE, RADIO_RF_OFF_WAIT, RADIO_ASLEEP,
    RADIO_BB_ON_WAIT, RADIO_RF_ON_WAIT, RADIO_FAILED_AWAKE, RADIO_FAILED_OWNED,
    RADIO_RF1_ON_WAIT };
enum radio_error { RADIO_OK, RADIO_READBACK, RADIO_WAKE_TIMEOUT, RADIO_IQK_CACHE,
    RADIO_CLOCK_STALLED };
enum radio_wake_reason { RADIO_WAKE_TBTT=1, RADIO_WAKE_TIM, RADIO_WAKE_H2C,
    RADIO_WAKE_TX, RADIO_WAKE_POLICY, RADIO_WAKE_FAILURE };
enum radio_restore_phase { RADIO_RESTORE_PRE_IQK=1, RADIO_RESTORE_POST_IQK };
#define RADIO_RESTORE_RF0       (1u << 0)
#define RADIO_RESTORE_RF1       (1u << 1)
#define RADIO_RESTORE_CR        (1u << 2)
#define RADIO_RESTORE_SYS_FUNC  (1u << 3)
#define RADIO_RESTORE_BBC00     (1u << 4)
#define RADIO_RESTORE_BB8B4     (1u << 5)
#define RADIO_RESTORE_STATE_SEL (1u << 6)
#define RADIO_RESTORE_LED       (1u << 7)
#define RADIO_RESTORE_SDIO      (1u << 8)
struct radio_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t sleep_requests, enter_declines, enters, wakes, timed_wakes, demand_wakes;
    uint32_t cancels, readback_errors, timeouts, iqk_errors, polls;
    uint32_t wake_reason, wake_at, phase_deadline, operation_deadline;
    uint32_t last_rf0, last_rf18, last_cr, last_sys_func, last_bbc00, last_bb8b4;
    uint32_t last_state_mon, last_empty, last_rxpkt;
    uint32_t saved_rf0, saved_cr, saved_sys_func, saved_bbc00, saved_bb8b4;
    uint32_t saved_led, saved_sdio, saved_pause, saved_state_sel;
    uint32_t sleep_started, accumulated_doze_us, last_doze_us, max_doze_us;
    uint32_t iqk_duration_us, wake_duration_us;
    uint32_t last_rf1, last_led, last_sdio, last_state_sel, saved_rf1;
    uint32_t restore_failure_mask, restore_check_phase, restore_checks;
    uint32_t rf1_written, rf1_post_iqk_delta;
};
extern volatile struct radio_status g_radio;
void radio_init(void);
/* Caller requires a real acknowledged AP PM=1 state. The function checks TCR
 * bit7 and hardware quiescence, but does not invent the AP-side protocol.
 * Return1 means ownership acquired; ASLEEP requires subsequent readback.
 * wake_at uses the qualified free-running microsecond counter, <=1s ahead.
 */
uint32_t radio_sleep(uint32_t wake_at);
void radio_wake(uint32_t reason);
void radio_poll(void);
void radio_cancel(void); /* no MMIO: external MAC/BB lifecycle shutdown */
uint32_t radio_owned(void);
uint32_t radio_asleep(void);
uint32_t radio_awake(void);
/* Supplied by the source IQK layer. Cache-only restore, never full IQK. */
uint32_t station_iqk_doze_restore(void);
#endif
