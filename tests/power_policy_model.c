/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>

#include "deep/clock.h"
#include "deep/deep.h"
#include "doze/doze.h"
#include "powersave/lps.h"
#include "station/power.h"
#include "wowlan/wowlan.h"

volatile struct diag_status g_diag;
volatile struct foundation_status g_foundation;
volatile struct deep_resume g_deep_resume;
volatile struct station_status g_station;
uint8_t __bss_start[1], __bss_end[1];

static uint32_t tcr;

uint32_t diag_hw_read(uint32_t address)
{
    if (address == 0xb8640100u) return 0xc0;
    assert(address == 0xb8640604u);
    return tcr;
}

/* Only an idempotent ACTIVE H2C may reach MMIO in this scenario. */
void diag_hw_write(uint32_t address, uint32_t value) { (void)address; (void)value; assert(0); }
uint32_t compat_read8(uint32_t address) { (void)address; assert(0); return 0; }
uint32_t compat_read16(uint32_t address) { (void)address; assert(0); return 0; }
void compat_write8(uint32_t address, uint32_t value) { diag_hw_write(address, value); }
void compat_write16(uint32_t address, uint32_t value) { diag_hw_write(address, value); }
uint32_t foundation_read32(uint32_t address) { (void)address; assert(0); return 0; }
void foundation_write32(uint32_t address, uint32_t value) { diag_hw_write(address, value); }
uint32_t foundation_now_us(void) { assert(0); return 0; }
int foundation_timer_start(uint32_t delay) { (void)delay; assert(0); return 0; }
void foundation_timer_stop(void) { assert(0); }
uint32_t core_irq_save(void) { assert(0); return 0; }
void core_irq_restore(uint32_t status) { (void)status; assert(0); }
uint32_t core_status(void) { assert(0); return 0; }
void core_deep_clock_gate(uint32_t ctrl) { (void)ctrl; assert(0); }
void diag_mailbox_poll(void) { assert(0); }
uint32_t station_transport_active(void) { assert(0); return 0; }
uint32_t station_transport_ready(void) { assert(0); return 0; }
uint32_t station_iqk_doze_restore(void) { assert(0); return 0; }
uint32_t ap_port_active(void) { assert(0); return 0; }
void diag_emit(uint32_t id, uint32_t arg0, uint32_t arg1)
{
    (void)id; (void)arg0; (void)arg1;
    assert(0);
}

int main(void)
{
    const uint32_t statuses[] = {0x400000c9u, 0x400020c9u, 0x400030c9u, DIAG_UNKNOWN};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        g_diag.chip.raw_sys_status1 = statuses[i];
        struct core_frame frame = {.cause = 0x2000};
        deep_init();
        assert(deep_rpwm_irq(&frame) == 0);
        assert(deep_h2c(0x000105f1, DEEP_MAGIC) == 2);
        assert(deep_h2c(0x000100f2, DEEP_MAGIC) == 2);
        assert(deep_pre() == 1);
        assert(station_wowlan_h2c(0x000101f0, WOWLAN_SIGNATURE) == 2);

        station_doze_init();
        station_doze_media(1);
        station_doze_poll();
        assert(g_doze.connected && !g_doze.enabled && g_doze.state == DOZE_DISABLED);

        station_lps_init();
        assert(station_lps_h2c(0x01000000, 0) == 1); /* Reserved page update. */
        assert(!station_lps_h2c(0x01210120, 0)); /* Sleep goes to awake-only handler. */
        assert(station_power_h2c(0x01210120, 0) == POWER_UNSUPPORTED);
        assert(!station_lps_h2c(0x01000020, 0x0c00));
        tcr = 0x80;
        assert(station_power_h2c(0x01000020, 0x0c00) == POWER_UNSUPPORTED);
        tcr = 0;
        assert(station_power_h2c(0x01000020, 0x0c00) == POWER_ALREADY_ACTIVE);
        assert(g_lps.state == LPS_ACTIVE && !g_lps.ap_ps && !g_lps.null1_submissions);
    }
    g_diag.chip.raw_sys_status1 = 0x400010c9u;
    station_doze_init();
    assert(g_doze.enabled && g_doze.state == DOZE_OBSERVING);
    return 0;
}
