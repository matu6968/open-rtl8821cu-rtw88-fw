/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_FOUNDATION_H
#define RTL8811CU_FOUNDATION_H
#include "../compat/compat.h"

#define FOUNDATION_MAGIC 0x31515249u /* IRQ1, separate ABI v1 */
#define TIMER_PERIOD_US 1000u
#define TIMER_MIN_US 256u
#define TIMER_ARM_GUARD_US 128u
#define TIMER_MASK 0x20000u
#define CPU_IRQ_MASK 0x2100u /* observed software IRQ and timer-controller line */
#define CORE_FRAME_BYTES 80

struct core_frame {
    uint32_t status, epc, lo, hi, v0, v1, a0, a1, a2, a3, s0, s1;
    uint32_t t8, t9, ra, sp, cause, reserved[3];
};
_Static_assert(sizeof(struct core_frame) == CORE_FRAME_BYTES, "IRQ frame ABI");
struct foundation_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t irq_count, timer_irqs, software_irqs, traps, unexpected;
    uint32_t depth, max_depth, last_cause, last_epc, last_status;
    uint32_t time_low, time_high, last_timer_us, next_deadline, timer_enabled;
    uint32_t period_us, late_events, max_lateness, deferred_ticks, foreground_ticks;
    uint32_t context_checks, context_failures, irq_frame_sp, irq_stack_low;
    uint32_t selftest_active, test_snapshot[16];
    uint32_t mask_checks, mask_failures, pending_verified, stop_verified, restart_verified;
    uint32_t timer_context_checks, timer_context_failures;
    uint32_t mask_failure_bitmap;
};
_Static_assert(sizeof(struct foundation_status) == 216, "IRQ1 status ABI");
extern volatile struct foundation_status g_foundation;
extern volatile uint32_t g_context_snapshot[16];

uint32_t core_irq_save(void);
void core_irq_restore(uint32_t status);
void core_irq_enable(void);
void core_irq_disable(void);
uint32_t core_status(void);
void core_raise_software(void);
void core_clear_software(void);
uint32_t core_context_test(void);
uint32_t core_context_timed_test(void);
uint32_t core_trap_test(void);
void core_irq_entry(void);
void core_trap_point(void);
void foundation_init(void);
void foundation_poll(void);
void foundation_irq_dispatch(struct core_frame *frame);
_Noreturn void foundation_fault_idle(void);
int foundation_timer_start(uint32_t delay_us);
int foundation_timer_at(uint32_t deadline);
void foundation_timer_stop(void);
uint32_t foundation_now_us(void);
int foundation_due(uint32_t now, uint32_t deadline);
int foundation_heartbeat_due(void);

#ifdef DIAG_HOST_TEST
uint32_t foundation_read32(uint32_t address);
void foundation_write32(uint32_t address, uint32_t value);
#else
static inline uint32_t foundation_read32(uint32_t a) {return diag_hw_read(a);}
static inline void foundation_write32(uint32_t a,uint32_t v) {diag_hw_write(a,v);}
#endif
#endif
