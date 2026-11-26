/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdint.h>

/* Linux appends the board record immediately after the patch version word. */
extern const volatile uint8_t bt_patch_config[];

#ifdef BT_ROM1_UART
#define BOARD_RECORD_OFFSET 0x000cu
#define BOARD_RECORD_BYTES 16u
void bt_enable_rom1_uart(void);
#else
#define BOARD_RECORD_OFFSET 0x00d9u
#define BOARD_RECORD_BYTES 1u
#endif

#define BOARD_RECORD_HEADER_BYTES 9u

int bt_apply_board_record(const volatile uint8_t *wire, size_t size,
                          volatile uint8_t *setting)
{
    static const uint8_t header[] = {
        0x55, 0xab, 0x23, 0x87, /* config magic */
        3u + BOARD_RECORD_BYTES, 0,
        BOARD_RECORD_OFFSET & 0xffu, BOARD_RECORD_OFFSET >> 8, BOARD_RECORD_BYTES,
    };

    if (size != sizeof(header) + BOARD_RECORD_BYTES)
        return 0;
    for (size_t i = 0; i < sizeof(header); ++i) {
        if (wire[i] != header[i])
            return 0;
    }
#ifdef BT_ROM1_UART
    for (size_t i = 0; i < BOARD_RECORD_BYTES; ++i)
        setting[i] = wire[sizeof(header) + i];
#else
    *setting = wire[sizeof(header)];
#endif
    return 1;
}

void bt_apply_rom1_config(void)
{
    /* Stock patch routines 0x8010e614 (USB) and 0x8010e7bc (UART)
     * apply the record relative to the controller configuration.
     */
    volatile uint8_t *setting = (volatile uint8_t *)(uintptr_t)(0x80120060u + BOARD_RECORD_OFFSET);
    const size_t size = BOARD_RECORD_HEADER_BYTES + BOARD_RECORD_BYTES;
#ifdef BT_ROM1_UART
    if (bt_apply_board_record(bt_patch_config, size, setting))
        bt_enable_rom1_uart();
#else
    bt_apply_board_record(bt_patch_config, size, setting);
#endif
}
