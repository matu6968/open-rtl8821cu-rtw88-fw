/* SPDX-License-Identifier: GPL-2.0-only */
#include "station.h"
#include <stddef.h>
#include "../broader/broader.h"
#include "../doze/doze.h"
#include "../wowlan/wowlan.h"
#include "../deep/deep.h"
void *memcpy(void *dest, const void *src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
    return dest;
}
volatile struct station_status g_station = {
    .magic = STATION_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct station_status)
};
void station_init(void)
{
    /* Host owns MAC/PHY/channel and transmit queues. This build does not
     * include the receive-only policy, which would force NO_LINK/TXPAUSE.
     * No optional firmware offloads or Bluetooth are advertised.
     */
    g_station.state = 1;
    g_compat.packet_transport_supported = 1;
    broader_init();
    station_wowlan_init();
    deep_init();
}
void station_poll(void)
{
    ++g_station.polls;
    /* WoW owns the RX FIFO. Normal C2H/report and RF-doze code must wait
     * until the complete disarm transaction releases it.
     */
    if (!station_wowlan_pre()) return;
    if (!station_doze_pre()) return;
    station_packet_poll();
    station_tx_report_poll();
    broader_poll();
}
