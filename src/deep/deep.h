/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_DEEP_H
#define RTL8811CU_DEEP_H
#include "../interrupts/foundation.h"
#define DEEP_MAGIC 0x31535044u /* DPS1 */
struct deep_status {
    uint32_t magic, abi_version, struct_bytes, state, error, mode, cookie;
    uint32_t commands, rejected, irq_events, unstable, overruns, pending;
    uint32_t last_raw, accepted, duplicates, last_toggle, requested, cpwm_acks;
    uint32_t last_cpwm, last_isr, last_cause, last_status, last_epc;
    uint32_t saved_ft1imr, saved_irq, clock_cycles, timer_epochs, timer_failures;
    uint32_t entry_progress, wake_progress, last_lo, last_hi, entry_request;
    uint32_t polled_events;
    uint32_t commits, rejected_commits;
    uint32_t time_offset, epoch_corrections, compensated_us, scheduling_declines;
    uint32_t associated_cycles, last_elapsed_us, last_missing_us;
    uint32_t rf_pre_cr, rf_pre_sys, rf_resume_state, rf_resume_error;
};
extern volatile struct deep_status g_deep;
void deep_init(void);
uint32_t deep_rpwm_irq(struct core_frame *frame);
uint32_t deep_h2c(uint32_t lo,uint32_t hi);
/* Must precede all MAC/BB/station/diagnostic foreground work. */
uint32_t deep_pre(void);
uint32_t deep_now_us(uint32_t raw);
uint32_t deep_hold_radio_awake(void);
#endif
