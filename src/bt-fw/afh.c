/* SPDX-License-Identifier: GPL-2.0-only */
#include "afh.h"

int bt_wlan_afh_map(uint8_t *map, uint8_t type, uint8_t channel, uint8_t span)
{
    unsigned int first = 0;
    unsigned int end = 0;

    if (type && span) {
        if (span >= BT_AFH_CHANNELS)
            return 0;
        int center;
        switch (type) {
        case 1: /* 2.4 GHz WLAN channel number */
            if (channel < 1 || channel > 14)
                return 0;
            center = ((int)channel + 1) * 5;
            break;
        case 2:
        case 3: /* 5 GHz harmonic expressed as a Bluetooth channel */
            if (channel >= BT_AFH_CHANNELS)
                return 0;
            center = channel;
            break;
        default:
            return 0;
        }

        int start = center - span / 2;
        /* The stock 2.4 GHz case aligns the start to a channel pair. */
        if (type == 1)
            start &= ~1;
        if (start > 0)
            first = (unsigned int)start;
        end = (unsigned int)center + span / 2u + 1u;
        if (end > BT_AFH_CHANNELS)
            end = BT_AFH_CHANNELS;
    }

    for (unsigned int i = 0; i < BT_AFH_BYTES - 1; ++i)
        map[i] = UINT8_MAX;
    map[BT_AFH_BYTES - 1] = 0x7f;
    for (unsigned int i = first; i < end; ++i)
        map[i / 8u] &= (uint8_t)~(1u << (i % 8u));
    return 1;
}

void bt_merge_wlan_afh(uint8_t *result, const volatile uint8_t *live,
                       const volatile uint8_t *previous, const uint8_t *next)
{
    /* Restore the old WLAN exclusion before installing the new one. */
    for (unsigned int i = 0; i < BT_AFH_BYTES; ++i)
        result[i] = (uint8_t)((live[i] | (uint8_t)~previous[i]) & next[i]);
}
