/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8821CU_BT_AFH_H
#define RTL8821CU_BT_AFH_H

#include <stdint.h>

#define BT_AFH_CHANNELS 79u
#define BT_AFH_BYTES 10u

int bt_wlan_afh_map(uint8_t *map, uint8_t type, uint8_t channel, uint8_t span);
void bt_merge_wlan_afh(uint8_t *result, const volatile uint8_t *live,
                       const volatile uint8_t *previous, const uint8_t *next);

#endif
