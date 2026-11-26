/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

typedef int (*efuse_read_t)(uint16_t offset, uint8_t *value);

#define ROM_EFUSE_READ ((efuse_read_t)(uintptr_t)0x8000a7d1u)
#define BT_EFUSE_READ_LIMIT 0x80u
#define BT_EFUSE_RX_MODE_OFFSET 0x6fu
#define BT_EFUSE_BDADDR_FIRST_WORD 0x22u
#define BT_BDADDR (*(volatile uint16_t (*)[3])(uintptr_t)0x801200a4u)
#define BT_BOARD_SELECT (*(volatile uint8_t *)(uintptr_t)0xb000a013u)
#ifdef BT_ROM1_UART
#define BT_BOARD_RX_MODE (*(volatile uint8_t *)(uintptr_t)0x80127bc8u)
#else
#define BT_BOARD_RX_MODE (*(volatile uint8_t *)(uintptr_t)0x80127bc0u)
#endif

/* CU 0x8010d53c and CS 0x8010d6e4 decode the same physical eFuse records.
 * The three default words are applied only if a factory word is present.
 */
int bt_read_efuse_bdaddr(efuse_read_t read, uint16_t limit, uint16_t address[3])
{
    uint16_t words[3] = {0x9987u, 0x4c23u, 0x00e0u};
    int programmed = 0;
    uint16_t cursor = 0;

    while (cursor < limit) {
        uint8_t header;
        if (!read(cursor++, &header))
            return 0;
        if (header == 0xffu || cursor == limit)
            break;

        unsigned int section = header >> 4;
        if ((header & 0x1fu) == 0x0fu) {
            unsigned int low = header >> 5;
            if (!read(cursor++, &header))
                return 0;
            section = low | ((header >> 4) << 3);
        }
        for (unsigned int word = 0; word < 4; ++word) {
            if (header & (1u << word))
                continue;
            if (limit - cursor < 2)
                goto finished;
            uint8_t low, high;
            if (!read(cursor++, &low) || !read(cursor++, &high))
                return 0;
            unsigned int logical = section * 4u + word;
            if (logical >= BT_EFUSE_BDADDR_FIRST_WORD &&
                logical - BT_EFUSE_BDADDR_FIRST_WORD < 3) {
                uint16_t value = low | ((uint16_t)high << 8);
                words[logical - BT_EFUSE_BDADDR_FIRST_WORD] = value;
                programmed |= value != UINT16_MAX;
            }
        }
    }

finished:
    if (programmed) {
        for (unsigned int i = 0; i < 3; ++i)
            address[i] = words[i];
    }
    return programmed;
}

unsigned int bt_efuse_rx_mode(uint8_t field)
{
    switch (field & 0x0fu) {
    case 6: return 1;
    case 5: return 2;
    default: return 0;
    }
}

void bt_apply_rom1_efuse(void)
{
    uint16_t address[3];
    if (bt_read_efuse_bdaddr(ROM_EFUSE_READ, BT_EFUSE_READ_LIMIT, address)) {
        for (unsigned int i = 0; i < 3; ++i)
            BT_BDADDR[i] = address[i];
    }

    /* CU 0x8010d500 and CS 0x8010d6a8 select the same field. */
    uint8_t field;
    BT_BOARD_SELECT = 0x40u;
    BT_BOARD_RX_MODE = ROM_EFUSE_READ(BT_EFUSE_RX_MODE_OFFSET, &field) ? bt_efuse_rx_mode(field) : 0;
}
