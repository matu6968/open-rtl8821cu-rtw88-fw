/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BT_ROM1_CONFIG_H
#define BT_ROM1_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define BT_CONFIG_RAM_BYTES 0x03f4u
#define BT_CONFIG_EXTRA_OFFSET 0x1007u
#define BT_CONFIG_EXTRA_BYTES 4u

int bt_apply_config_records(const volatile uint8_t *wire, size_t available,
                            volatile uint8_t *settings, volatile uint8_t *extra);

#endif
