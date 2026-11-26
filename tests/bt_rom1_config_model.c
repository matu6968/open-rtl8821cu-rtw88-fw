/* SPDX-License-Identifier: GPL-2.0-only */
#include "rom1_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const volatile uint8_t bt_patch_config[1];

int main(void)
{
    uint8_t settings[BT_CONFIG_RAM_BYTES + 1];
    uint8_t extra[BT_CONFIG_EXTRA_BYTES + 1];
    uint8_t wire[64];
    size_t size = fread(wire, 1, sizeof(wire), stdin);
    assert(size != sizeof(wire));
    memset(settings, 0xa5, sizeof(settings));
    memset(extra, 0xa5, sizeof(extra));

    for (size_t truncated = 0; truncated < size; ++truncated)
        assert(!bt_apply_config_records(wire, truncated, settings, extra));
    for (size_t i = 0; i < sizeof(settings); ++i)
        assert(settings[i] == 0xa5);
    for (size_t i = 0; i < sizeof(extra); ++i)
        assert(extra[i] == 0xa5);

    assert(bt_apply_config_records(wire, size, settings, extra));
    assert(settings[0x0c] == 0x02);
    assert(settings[0x0d] == 0x80);
    assert(settings[0xd9] == 0x0f);
    assert(settings[BT_CONFIG_RAM_BYTES - 1] == 0x88);
    assert(settings[BT_CONFIG_RAM_BYTES] == 0xa5);
    assert(extra[0] == 1 && extra[3] == 4);
    assert(extra[BT_CONFIG_EXTRA_BYTES] == 0xa5);

    memset(settings, 0xa5, sizeof(settings));
    memset(extra, 0xa5, sizeof(extra));
    wire[size - 4] = 0xf4; /* The final record now starts beyond settings. */
    assert(!bt_apply_config_records(wire, size, settings, extra));
    for (size_t i = 0; i < sizeof(settings); ++i)
        assert(settings[i] == 0xa5);
    for (size_t i = 0; i < sizeof(extra); ++i)
        assert(extra[i] == 0xa5);
    return 0;
}
