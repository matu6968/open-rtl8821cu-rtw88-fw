/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

extern const volatile uint32_t bt_patch_version;
#ifdef BT_ROM1_IMAGE
void bt_apply_rom1_config(void);
#endif
#ifdef BT_ROM1_FULL
void bt_apply_rom1_efuse(void);
#endif
#if defined(BT_ROM1_FULL) && defined(BT_ROM1_UART)
void bt_enable_rom1_uart(void);
#endif
#ifdef BT_ROM1_COEX
void bt_enable_rom1_coex(void);
#endif

/* The common prefix of both stock entries publishes the word that btrtl
 * writes into the last four bytes of the downloaded patch.
 */
__attribute__((section(".text.entry")))
void bt_publish_patch_version(void)
{
    uint32_t version = bt_patch_version;
    *(volatile uint16_t *)(uintptr_t)0x80120056u = version >> 16;
    *(volatile uint16_t *)(uintptr_t)0x80120054u = version & 0xffffu;
#if defined(BT_ROM1_FULL) && defined(BT_ROM1_UART)
    bt_enable_rom1_uart();
#endif
#ifdef BT_ROM1_FULL
    bt_apply_rom1_efuse();
#endif
#ifdef BT_ROM1_IMAGE
    bt_apply_rom1_config();
#endif
#ifdef BT_ROM1_COEX
    bt_enable_rom1_coex();
#endif
}
