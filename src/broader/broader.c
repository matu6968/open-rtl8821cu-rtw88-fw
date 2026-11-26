/* SPDX-License-Identifier: GPL-2.0-only */
#include "broader.h"
#include "../ap/ap.h"
#include "../coex/coex.h"
#include "../station/rate.h"
#include "../station/power.h"
#include "../powersave/lps.h"
#include "../doze/doze.h"
#include "../wowlan/wowlan.h"
#include "../deep/deep.h"
#include "../fwlog/fwlog.h"
#include "../doze/radio.h"

volatile struct broader_status g_broader = {
    .magic = BROADER_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct broader_status)
};

void broader_init(void)
{
    station_rate_init();
    station_lps_init();
    station_doze_init();
    g_broader.state = 1;
}

uint32_t broader_register_h2c(uint32_t lo, uint32_t hi)
{
    ++g_broader.register_commands;
    g_broader.last_lo = lo; g_broader.last_hi = hi;
    uint32_t result = 0;
    result = deep_h2c(lo,hi);
    if (!result) result = station_wowlan_h2c(lo,hi);
    /* Only the validated disarm command can change ownership while
     * firmware consumes RX packets on behalf of the suspended host.
     */
    if (!result && station_wowlan_owned()) result = 2;
    if (!result) result = station_rate_h2c(lo,hi);
    if (result == 1 && (lo & 255u) == 1u &&
        !((lo >> 16) & 255u) && !ap_port_active()) {
        uint32_t connected = !!(lo & 256u);
        station_doze_media(connected);
        if (!connected) station_lps_cancel();
    }
    if (!result) result = ap_h2c(lo,hi);
    if (!result) result = station_lps_h2c(lo,hi);
    if (!result) result = station_power_h2c(lo,hi);
    if (!result) result = fw_log_h2c(lo,hi);
#ifdef RTL8821CU_COEX_BOARD
    if (!result) result = coex_h2c(lo, hi);
#endif
    if (result == 1) {
        ++g_broader.register_handled;
        return 1;
    }
    /* Releasing an unsupported register mailbox never denotes RF success. */
    ++g_broader.register_unsupported;
    return 0;
}

void broader_poll(void)
{
    uint32_t ap = ap_poll();
    station_lps_poll();
    if (station_transport_ready()) station_rate_poll();
    if (ap) coex_transport_lost();
    if (!ap) station_doze_poll();
    if (!radio_owned()) {
        if (!ap) coex_poll();
        fw_log_poll();
    }
}
