/* SPDX-License-Identifier: GPL-2.0-only */
#include "ap.h"
#include "../compat/compat.h"
#include "../doze/doze.h"
#include "../fwlog/fwlog.h"

#define PORT0_NETWORK_TYPE 0xb8640102u
#define PORT0_TYPE_MASK 3u
#define PORT0_AP 3u

uint32_t ap_port_active(void)
{
    return (compat_read8(PORT0_NETWORK_TYPE) & PORT0_TYPE_MASK) == PORT0_AP;
}

uint32_t ap_poll(void)
{
    static uint32_t previous;
    uint32_t active = ap_port_active();
    if (active == previous) return active;
    previous = active;
    if (active) {
        if (g_doze.connected) station_doze_media(0);
        /* rtw88 owns the beacon template and the MAC schedules transmission. */
        fw_log(FW_LOG_ALWAYS, "AP: port 0 active");
    } else {
        fw_log(FW_LOG_ALWAYS, "AP: port 0 inactive");
    }
    return active;
}

uint32_t ap_h2c(uint32_t lo, uint32_t hi)
{
    uint32_t id = lo & 255u;
    if ((id != 0u && id != 0x20u) || !ap_port_active()) return 0;

    /* rtw88 downloads the beacon to the hardware queue and sends an empty
     * reserved-page command for a beacon-only AP. The optional station
     * templates and client-side PM command have no AP owner.
     */
    return (lo || hi) ? 2u : 1u;
}
