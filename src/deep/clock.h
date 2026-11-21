/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_DEEP_CLOCK_H
#define RTL8811CU_DEEP_CLOCK_H
#include "../interrupts/foundation.h"
#define CLOCK_MAGIC 0x314b4c43u /* CLK1 */
enum clock_state { CLOCK_AWAKE, CLOCK_WAIT_SLEEP, CLOCK_HOLD,
    CLOCK_WAIT_WAKE, CLOCK_FAILED };
enum clock_mode { CLOCK_MACSLP=2, CLOCK_RETAIN=3, CLOCK_TIMER=4, CLOCK_STATION=5 };
enum clock_error { CLOCK_OK, CLOCK_BUSY, CLOCK_READBACK,
    CLOCK_SLEEP_TIMEOUT, CLOCK_WAKE_TIMEOUT, CLOCK_ENGINE_STUCK, CLOCK_RETENTION };
struct clock_status {
    uint32_t magic, abi_version, struct_bytes, state, error, mode, owned;
    uint32_t requests, declines, engine_requests, sleep_stable, wakes;
    uint32_t polls, hold_polls, transition_polls, wake_requested;
    uint32_t saved_sys, saved_ctrl, saved_options, options_owned;
    uint32_t last_sys, last_ctrl, last_tsf, free_before, free_after;
    uint32_t raw_before, raw_after;
    uint32_t saved_cr1, saved_cal1, saved_cal3, saved_osc, source_polls;
    uint32_t saved_cal, last_cal;
    uint32_t saved_bcn, saved_timer_lo, saved_timer_hi, saved_timer_ctrl;
    uint32_t timer_owned, timer_deadline, timer_pending, timer_pending_wakes;
    /* Pending PS0 observed after wake is not proof of autonomous CPU wake. */
    uint32_t tsf_hi_before, tsf_after, rpwm_at_wake, timer_arms;
    uint32_t timer_irq_owned, timer_irqs;
    uint32_t wake_at_us;
    uint32_t ram_canary_low, ram_lost_mask, bss_before0, bss_before1;
    uint32_t bss_after0, bss_after1, radio_owner_before, radio_owner_after;
};
extern volatile struct clock_status g_clock;
/* Assembly-owned warm-return record. Offsets are shared with entry.S. */
struct deep_resume { uint32_t magic, active, sp, pc, warm_entries, saved_boot; };
extern volatile struct deep_resume g_deep_resume;
void core_deep_clock_gate(uint32_t ctrl);
struct deep_iqk_status {
    uint32_t magic, abi_version, struct_bytes, attempts, rf18, adc, frontend;
    uint32_t completed, key0, key1, fail0, fail1, result;
};
extern volatile struct deep_iqk_status g_deep_iqk;
void clock_init(void);
/* Caller has stopped timer A and disabled CPU IE. The explicit engine probe
 * uses the stock USB upper0092=9360 domain policy and owned warm return. It must
 * never be enabled as an ordinary advertised LCLK feature from ACK alone.
 */
uint32_t clock_begin(uint32_t mode);
void clock_poll(void);
void clock_wake(void);
uint32_t clock_owned(void);
uint32_t clock_awake(void);
uint32_t clock_timer_irq(void);
#endif
