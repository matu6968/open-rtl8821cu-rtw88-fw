/* SPDX-License-Identifier: GPL-2.0-only */
#include "power.h"

volatile struct station_power_status g_power = {
    .magic = POWER_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct station_power_status)
};

uint32_t station_power_h2c(uint32_t lo, uint32_t hi)
{
    /* Linux rtw88/fw.h SET_PWR_MODE_* describes this eight-byte register H2C.
     * A transport acknowledgement is not an acknowledgement of RF sleep.
     * Ordinary LPS requires beacon/DTIM scheduling, reserved null templates,
     * real PM=1/0 null transmission/ACK and restoration of TCR. Deep LPS also
     * requires independently established RPWM/CPWM/clock ownership. None of
     * those effects is claimed by this awake-only build.
     */
    if ((lo & 255u) != 0x20u)
        return POWER_NOT_COMMAND;
    ++g_power.requests;
    g_power.last_lo = lo; g_power.last_hi = hi;
    g_power.mode = (lo >> 8) & 127u;
    g_power.rlbm = (lo >> 16) & 15u;
    g_power.smart_ps = (lo >> 20) & 15u;
    g_power.awake_interval = lo >> 24;
    g_power.port = (hi >> 5) & 7u;
    g_power.requested_state = (hi >> 8) & 255u;
    if ((lo & 0x8000u) || (hi & ~0x0000ffe0u) || g_power.port > 4u) {
        ++g_power.malformed;
        return POWER_UNSUPPORTED;
    }
    if (g_power.mode != 0u || g_power.requested_state != 0xcu ||
        g_power.rlbm != 0u || g_power.smart_ps != 0u) {
        ++g_power.unsupported_sleep;
        return POWER_UNSUPPORTED;
    }
    /* Idempotent active requests may be recognized only while host-configured
     * TX/RX are enabled and hardware PM tagging is already clear. We do not
     * wake the radio, clear TCR to fake a wake, or hide an unfinished leave-LPS.
     */
    g_power.cr = diag_hw_read(0xb8640100u);
    g_power.tcr = diag_hw_read(0xb8640604u);
    if ((g_power.cr & 255u) == 0xeau || (g_power.cr & 0xc0u) != 0xc0u || (g_power.tcr & 0x80u)) {
        ++g_power.not_awake;
        return POWER_UNSUPPORTED;
    }
    ++g_power.already_active;
    return POWER_ALREADY_ACTIVE;
}

void station_power_rpwm_observe(uint32_t value)
{
    ++g_power.rpwm_observations;
    g_power.last_rpwm = value & 255u;
}
