/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int bt_apply_board_record(const volatile uint8_t *wire, size_t size,
                          volatile uint8_t *setting);

#ifdef BT_ROM1_UART
#define VALUE_BYTES 16
void bt_enable_rom1_uart(void) {}
#else
#define VALUE_BYTES 1
#endif

/* The target-only entry point refers to this linker symbol. */
const volatile uint8_t bt_patch_config[9 + VALUE_BYTES];

int main(void)
{
    uint8_t wire[sizeof(bt_patch_config)];
    uint8_t destination[VALUE_BYTES + 2];
    uint8_t *setting = destination + 1;
    memset(destination, 0x60, sizeof(destination));
    assert(fread(wire, 1, sizeof(wire), stdin) == sizeof(wire));
    assert(bt_apply_board_record(wire, sizeof(wire), setting) == 1);
    assert(memcmp(setting, wire + 9, VALUE_BYTES) == 0);
    assert(destination[0] == 0x60 && destination[VALUE_BYTES + 1] == 0x60);

    uint8_t original[sizeof(destination)];
    memset(original, 0x60, sizeof(original));
    memcpy(destination, original, sizeof(destination));
    assert(bt_apply_board_record(wire, sizeof(wire) - 1, setting) == 0);
    assert(memcmp(destination, original, sizeof(destination)) == 0);

    for (size_t i = 0; i < 9; ++i) {
        wire[i] ^= 1;
        assert(bt_apply_board_record(wire, sizeof(wire), setting) == 0);
        assert(memcmp(destination, original, sizeof(destination)) == 0);
        wire[i] ^= 1;
    }
    return 0;
}
