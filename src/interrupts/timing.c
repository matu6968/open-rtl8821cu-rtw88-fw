/* SPDX-License-Identifier: GPL-2.0-only */
#include "foundation.h"
#include "../deep/deep.h"
#define TIMER_CTRL 0xb8641510u
#define TIMER_A 0xb8641504u
#define FTIMR 0xb8640138u
#define FTISR 0xb864013cu

int foundation_due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

uint32_t foundation_now_us(void)
{
    return deep_now_us(foundation_read32(0xb8640568u));
}

/* Caller has interrupts disabled. Disable comparator before changing its
 * deadline; clear only our W1C status bit and use the free-running source 5.
 */
int foundation_timer_at(uint32_t deadline)
{
    uint32_t saved = core_irq_save();
    uint32_t lead = deadline - foundation_now_us();
    if (lead < TIMER_ARM_GUARD_US || lead > 0x7fffffffu) {
        core_irq_restore(saved);
        return -1;
    }
    uint32_t control = compat_read8(TIMER_CTRL);
    compat_write8(TIMER_CTRL, control & ~0x80u);
    foundation_write32(FTISR, TIMER_MASK);
    g_foundation.next_deadline = deadline;
    foundation_write32(TIMER_A, deadline);
    compat_write8(TIMER_CTRL, 0x85); /* PS timer A enable, source = free count */
    foundation_write32(FTIMR, foundation_read32(FTIMR) | TIMER_MASK);
    g_foundation.timer_enabled = 1;
    core_irq_restore(saved);
    return 0;
}

int foundation_timer_start(uint32_t delay_us)
{
    if (delay_us < TIMER_MIN_US || delay_us > 0x7fffffffu)
        return -1;
    uint32_t saved = core_irq_save();
    /* Request minimum includes room for bookkeeping before the arm guard. */
    uint32_t deadline = foundation_now_us() + delay_us;
    int result = foundation_timer_at(deadline);
    core_irq_restore(saved);
    return result;
}

void foundation_timer_stop(void)
{
    uint32_t saved = core_irq_save();
    foundation_write32(FTIMR, foundation_read32(FTIMR) & ~TIMER_MASK);
    compat_write8(TIMER_CTRL, compat_read8(TIMER_CTRL) & ~0x80u);
    foundation_write32(FTISR, TIMER_MASK);
    g_foundation.timer_enabled = 0;
    core_irq_restore(saved);
}
