/* SPDX-License-Identifier: GPL-2.0-only */
#include "rom1_config.h"

#define BT_CONFIG_MAGIC 0x8723ab55u
#define BT_CONFIG_HEADER_BYTES 6u
#define BT_CONFIG_RECORD_HEADER_BYTES 3u
#define BT_CONFIG_DOWNLOAD_END 0x80113fffu
#define BT_CONFIG_RAM 0x80120060u
#define BT_CONFIG_EXTRA_RAM 0x80127b44u

extern const volatile uint8_t bt_patch_config[];

static uint16_t config_u16(const volatile uint8_t *wire)
{
    return (uint16_t)wire[0] | ((uint16_t)wire[1] << 8);
}

int bt_apply_config_records(const volatile uint8_t *wire, size_t available,
                            volatile uint8_t *settings, volatile uint8_t *extra)
{
    if (available < BT_CONFIG_HEADER_BYTES ||
        config_u16(wire) != (BT_CONFIG_MAGIC & 0xffffu) ||
        config_u16(wire + 2) != (BT_CONFIG_MAGIC >> 16))
        return 0;

    size_t length = config_u16(wire + 4);
    wire += BT_CONFIG_HEADER_BYTES;
    if (length > available - BT_CONFIG_HEADER_BYTES)
        return 0;

    /* Validate the whole config before writing controller state. */
    for (size_t pos = 0; pos < length;) {
        size_t left = length - pos;
        if (left < BT_CONFIG_RECORD_HEADER_BYTES)
            return 0;
        uint16_t offset = config_u16(wire + pos);
        uint8_t count = wire[pos + 2];
        if (count > left - BT_CONFIG_RECORD_HEADER_BYTES)
            return 0;
        if (offset == BT_CONFIG_EXTRA_OFFSET) {
            if (count > BT_CONFIG_EXTRA_BYTES)
                return 0;
        } else if (offset >= BT_CONFIG_RAM_BYTES || count > BT_CONFIG_RAM_BYTES - offset) {
            return 0;
        }
        pos += BT_CONFIG_RECORD_HEADER_BYTES + count;
    }

    for (size_t pos = 0; pos < length;) {
        uint16_t offset = config_u16(wire + pos);
        uint8_t count = wire[pos + 2];
        volatile uint8_t *dest = offset == BT_CONFIG_EXTRA_OFFSET ? extra : settings + offset;
        for (size_t i = 0; i < count; ++i)
            dest[i] = wire[pos + BT_CONFIG_RECORD_HEADER_BYTES + i];
        pos += BT_CONFIG_RECORD_HEADER_BYTES + count;
    }
    return 1;
}

void bt_apply_rom1_config(void)
{
    const size_t available = BT_CONFIG_DOWNLOAD_END - (uintptr_t)bt_patch_config;
    volatile uint8_t *settings = (volatile uint8_t *)(uintptr_t)BT_CONFIG_RAM;
    volatile uint8_t *extra = (volatile uint8_t *)(uintptr_t)BT_CONFIG_EXTRA_RAM;
    bt_apply_config_records(bt_patch_config, available, settings, extra);
}
