/* SPDX-License-Identifier: GPL-2.0-only */
#include "foundation.h"
#include "../deep/deep.h"

volatile struct foundation_status g_foundation = {
    .magic = FOUNDATION_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct foundation_status), .period_us = TIMER_PERIOD_US
};
volatile uint32_t g_context_snapshot[16];

static void fatal(struct core_frame *frame, uint32_t error)
{
    foundation_timer_stop();
    core_clear_software();
    g_foundation.error = error;
    g_foundation.state = 2;
    ++g_foundation.unexpected;
    frame->status &= ~(CPU_IRQ_MASK | 5u); /* no re-enable on exception return */
    frame->epc = (uint32_t)(uintptr_t)foundation_fault_idle | 1;
}

void foundation_irq_dispatch(struct core_frame *f)
{
    uint32_t cause = f->cause;
    ++g_foundation.depth;
    ++g_foundation.irq_count;
    if (g_foundation.depth > g_foundation.max_depth)
        g_foundation.max_depth = g_foundation.depth;
    g_foundation.last_cause = cause;
    g_foundation.last_epc = f->epc;
    g_foundation.last_status = f->status;
    g_foundation.irq_frame_sp = (uint32_t)(uintptr_t)f;
    uint32_t sp = diag_read_sp();
    if (!g_foundation.irq_stack_low || sp < g_foundation.irq_stack_low)
        g_foundation.irq_stack_low = sp;
    if (g_foundation.depth != 1 || (f->status & 1)) {
        fatal(f, 1);
    } else if ((cause & 0x7c) == 0x28 && g_foundation.selftest_active &&
               (f->epc & ~1u) == ((uint32_t)(uintptr_t)core_trap_point & ~1u)) {
        ++g_foundation.traps;
        f->epc += 2; /* only the explicitly requested and verified FEBa trap */
    } else if (cause & 0x7c) {
        fatal(f, 2);
    } else {
        uint32_t handled = 0;
        if (cause & 0x2000u) handled |= deep_rpwm_irq(f);
        if (cause & 0x100) {
            core_clear_software();
            ++g_foundation.software_irqs;
            handled = 1;
        }
        if ((cause & 0x2000) &&
            (foundation_read32(0xb864013cu) & TIMER_MASK)) {
            /* RPWM acceptance precedes off-domain timestamp accesses. */
            uint32_t now = foundation_now_us();
            uint32_t late = now - g_foundation.next_deadline;
            if (late > g_foundation.max_lateness)
                g_foundation.max_lateness = late;
            if (late >= TIMER_PERIOD_US)
                ++g_foundation.late_events;
            g_foundation.last_timer_us = now;
            ++g_foundation.timer_irqs;
            ++g_foundation.deferred_ticks;
            uint32_t next = g_foundation.next_deadline + TIMER_PERIOD_US;
            if (foundation_timer_at(next))
                (void)foundation_timer_start(TIMER_PERIOD_US);
            handled = 1;
        }
        if (!handled || (cause & f->status & 0xff00 & ~CPU_IRQ_MASK))
            fatal(f, 3);
    }
    --g_foundation.depth;
}

static uint32_t context_ok(uint32_t timed)
{
    static const uint32_t expected[13] = {0x100,0x22334455,0x33445566,0x44556677,
        0x55667788,0x66778899,0x778899aa,0x8899aabb,0x55667788,0,
        0x11223344,0x55667788,0x44556677};
    if (g_context_snapshot[0] != (timed ? 0 : expected[0]) ||
        g_context_snapshot[1] != (timed ? 1799970000u : expected[1]))
        return 0;
    for (uint32_t i = 2; i < 13; ++i)
        if (g_context_snapshot[i] != expected[i])
            return 0;
    return g_context_snapshot[13] >= DIAG_STACK_BOTTOM + 32 &&
        g_context_snapshot[13] < DIAG_STACK_TOP && !(g_context_snapshot[13] & 7);
}

static uint32_t wait_us(uint32_t duration)
{
    uint32_t start = foundation_now_us();
    /* Clock failure is bounded by an independent iteration limit. This is a
     * boot qualification helper, not an iteration-calibrated time service.
     */
    for (uint32_t spins = 0; spins < 2000000u; ++spins)
        if ((uint32_t)(foundation_now_us() - start) >= duration)
            return 1;
    return 0;
}

static void mask_check(uint32_t ok)
{
    if (!ok) {
        ++g_foundation.mask_failures;
        g_foundation.mask_failure_bitmap |= 1u << g_foundation.mask_checks;
    }
    ++g_foundation.mask_checks;
}

static void masking_tests(void)
{
    uint32_t saved = core_irq_save(), before = g_foundation.software_irqs;
    uint32_t nested = core_irq_save();
    core_raise_software();
    mask_check(g_foundation.software_irqs == before && !(core_status() & 1));
    core_irq_restore(nested);
    mask_check(g_foundation.software_irqs == before && !(core_status() & 1));
    core_irq_restore(saved);
    mask_check(g_foundation.software_irqs == before + 1 && (core_status() & 1));

    (void)foundation_timer_start(TIMER_PERIOD_US);
    saved = core_irq_save(); before = g_foundation.timer_irqs;
    mask_check(wait_us(5000));
    mask_check(g_foundation.timer_irqs == before);
    g_foundation.pending_verified = !!(foundation_read32(0xb864013cu) & TIMER_MASK);
    mask_check(g_foundation.pending_verified);
    core_irq_restore(saved);
    mask_check(g_foundation.timer_irqs == before + 1);
    mask_check(!(foundation_read32(0xb864013cu) & TIMER_MASK));

    /* Verify the controller source mask separately from the global IE bit. */
    saved = core_irq_save();
    foundation_write32(0xb8640138u, 0);
    before = g_foundation.timer_irqs;
    core_irq_restore(saved);
    mask_check(wait_us(3000) && g_foundation.timer_irqs == before);
    mask_check(!!(foundation_read32(0xb864013cu) & TIMER_MASK));
    foundation_write32(0xb8640138u, TIMER_MASK);
    (void)wait_us(16); /* allow the controller's edge to reach the CPU */
    mask_check(g_foundation.timer_irqs == before + 1);

    foundation_timer_stop(); before = g_foundation.timer_irqs;
    g_foundation.stop_verified = wait_us(3000) && g_foundation.timer_irqs == before &&
        !(foundation_read32(0xb864013cu) & TIMER_MASK);
    mask_check(g_foundation.stop_verified);
    (void)foundation_timer_start(TIMER_PERIOD_US);
    g_foundation.restart_verified = wait_us(2500) && g_foundation.timer_irqs > before;
    mask_check(g_foundation.restart_verified);
}

void foundation_init(void)
{
    core_irq_disable();
    /* Own the two ROM dispatch slots before enabling any source. Keep the
     * existing cold-start entry pointer at 17C intact.
     */
    diag_hw_write(0x80200180, (uint32_t)(uintptr_t)core_irq_entry | 1);
    diag_hw_write(0x80200184, (uint32_t)(uintptr_t)core_irq_entry | 1);
    static const uint32_t masks[] = {0x130,0x120,0x1120,0x1128,0x1130,0x138,0x1138,0x11e0};
    for (uint32_t i = 0; i < sizeof(masks)/sizeof(masks[0]); ++i)
        foundation_write32(0xb8640000u + masks[i], 0);
    core_clear_software();
    /* Linux rtw88 mac.c programs the same 80-MHz MAC timestamp divider.
     * This is a MAC clock setting, independent of the CPU delay-loop clock.
     */
    compat_write8(0xb864055cu, 80);
    compat_write8(0xb8640577u, compat_read8(0xb8640577u) | 8);
    core_irq_restore(CPU_IRQ_MASK | 1);
    g_foundation.selftest_active = 1;
    for (uint32_t i = 0; i < 8; ++i) {
        (void)core_context_test();
        ++g_foundation.context_checks;
        if (!context_ok(0))
            ++g_foundation.context_failures;
    }
    if (core_trap_test() != 0x5a)
        ++g_foundation.context_failures;
    masking_tests();
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t before = g_foundation.timer_irqs;
        (void)core_context_timed_test();
        ++g_foundation.timer_context_checks;
        if (!context_ok(1) || g_foundation.timer_irqs == before)
            ++g_foundation.timer_context_failures;
    }
    g_foundation.selftest_active = 0;
    for (uint32_t i = 0; i < 16; ++i)
        g_foundation.test_snapshot[i] = g_context_snapshot[i];
    if (g_foundation.context_failures || g_foundation.mask_failures ||
        g_foundation.timer_context_failures) {
        g_foundation.error = 4;
        g_foundation.state = 2;
        core_irq_disable();
        return;
    }
    g_foundation.state = 1;
    (void)foundation_timer_start(TIMER_PERIOD_US);
    diag_emit(32, g_foundation.software_irqs, g_foundation.traps);
}

int foundation_heartbeat_due(void)
{
    static uint32_t next;
    uint32_t now = g_foundation.time_low;
    if (!next) { next = now + 500000u; return 0; }
    if (!foundation_due(now, next)) return 0;
    next = now + 500000u;
    return 1;
}

void foundation_poll(void)
{
    uint32_t now = foundation_now_us();
    if (now < g_foundation.time_low)
        ++g_foundation.time_high;
    g_foundation.time_low = now;
    uint32_t pending, saved = core_irq_save();
    pending = g_foundation.deferred_ticks;
    g_foundation.deferred_ticks = 0;
    core_irq_restore(saved);
    g_foundation.foreground_ticks += pending;
    /* Only foreground writes the diagnostic ring. ISR work never logs or
     * waits for USB; accumulated ticks are transferred under irq_save.
     */
}

_Noreturn void foundation_fault_idle(void)
{
    core_irq_disable();
    g_diag.state = DIAG_FAILED;
    g_diag.last_error = ERROR_FOUNDATION;
    diag_hw_write(0xb86010f8u,0xe6000000u|(g_foundation.error<<16)|
                  (g_foundation.last_cause&0xfffcu));
    diag_hw_write(0xb86010fcu, 0x11111111u);
    for (;;) {
        ++g_diag.progress;
        diag_mailbox_poll();
    }
}
