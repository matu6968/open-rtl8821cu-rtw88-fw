/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

#define BT_HOST_INTERFACE (*(const volatile uint32_t *)(uintptr_t)0xb000a0a0u)
#define BT_HOST_UART (1u << 2)
#define BT_UART_CONTROL (*(volatile uint32_t *)(uintptr_t)0xb5000030u)
#define ROM_UART_SAVED_CONTROL (*(volatile uint32_t *)(uintptr_t)0x80108a08u)

typedef uint32_t (*irq_save_t)(void);
typedef void (*irq_restore_t)(uint32_t);
typedef void (*uart_flag_t)(uint16_t);
typedef void (*uart_resume_t)(void);
#define ROM_IRQ_SAVE ((irq_save_t)(uintptr_t)0x800081ecu)
#define ROM_IRQ_RESTORE ((irq_restore_t)(uintptr_t)0x80008208u)
#define ROM_SET_UART_FLAG ((uart_flag_t)(uintptr_t)0x8000b529u)
#define ROM_UART_RESUME (*(uart_resume_t volatile *)(uintptr_t)0x80120934u)

static void restore_uart_control(void)
{
    /* The stock CS hook at 0x80111a50 consumes the saved UART control
     * word and preserves the peripheral's low halfword.
     */
    uint32_t irq_state = ROM_IRQ_SAVE();
    uint32_t saved = (BT_HOST_INTERFACE & BT_HOST_UART) ? ROM_UART_SAVED_CONTROL : 0;
    ROM_UART_SAVED_CONTROL = 0;
    BT_UART_CONTROL = (BT_UART_CONTROL & 0xffffu) | (saved & 0xffff0000u);
    ROM_SET_UART_FLAG(0);
    ROM_IRQ_RESTORE(irq_state);
}

void bt_enable_rom1_uart(void)
{
    /* H5 and the pre-download baud change already run in the controller
     * ROM. Keep its transport and supply the CS recovery callback.
     */
    ROM_UART_RESUME = restore_uart_control;
}
