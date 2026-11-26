/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>

#include "../src/bt-fw/afh.h"

static unsigned int enabled(const uint8_t *map, unsigned int channel)
{
    return (map[channel / 8u] >> (channel % 8u)) & 1u;
}

static void check_range(const uint8_t *map, unsigned int first, unsigned int end)
{
    for (unsigned int i = 0; i < BT_AFH_CHANNELS; ++i)
        assert(enabled(map, i) == (i < first || i >= end));
    assert(!enabled(map, BT_AFH_CHANNELS));
}

int main(void)
{
    uint8_t previous[BT_AFH_BYTES], next[BT_AFH_BYTES];
    uint8_t live[BT_AFH_BYTES], result[BT_AFH_BYTES];

    assert(bt_wlan_afh_map(previous, 1, 1, 24));
    check_range(previous, 0, 23);
    assert(bt_wlan_afh_map(next, 1, 6, 20));
    check_range(next, 24, 46);

    for (unsigned int i = 0; i < BT_AFH_BYTES; ++i)
        live[i] = previous[i];
    live[7] &= (uint8_t)~1u;
    bt_merge_wlan_afh(result, live, previous, next);
    for (unsigned int i = 0; i < BT_AFH_CHANNELS; ++i)
        assert(enabled(result, i) == (enabled(next, i) && i != 56));
    assert(!enabled(result, BT_AFH_CHANNELS));

    assert(bt_wlan_afh_map(next, 1, 14, 20));
    check_range(next, 64, 79);
    assert(bt_wlan_afh_map(next, 2, 68, 5));
    check_range(next, 66, 71);
    assert(bt_wlan_afh_map(next, 3, 68, 5));
    check_range(next, 66, 71);
    assert(bt_wlan_afh_map(next, 0, 0, 0));
    check_range(next, 0, 0);
    bt_merge_wlan_afh(result, live, previous, next);
    for (unsigned int i = 0; i < BT_AFH_CHANNELS; ++i)
        assert(enabled(result, i) == (i != 56));

    assert(!bt_wlan_afh_map(next, 1, 0, 20));
    assert(!bt_wlan_afh_map(next, 1, 15, 20));
    assert(!bt_wlan_afh_map(next, 2, 79, 20));
    assert(!bt_wlan_afh_map(next, 4, 6, 20));
    assert(!bt_wlan_afh_map(next, 1, 6, 79));
    check_range(next, 0, 0);
    return 0;
}
