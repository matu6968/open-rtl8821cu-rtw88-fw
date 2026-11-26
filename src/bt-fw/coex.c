/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include "afh.h"

#define ROM_COEX_TIMER_FLAGS (*(volatile uint8_t *)(uintptr_t)0x8012013bu)
#define ROM_COEX_TIMERS_ENABLE (1u << 6)
#define ROM_COEX_FLAGS (*(volatile uint8_t *)(uintptr_t)0x80120147u)
#define ROM_COEX_IRQ_ENABLE (1u << 7)
#define ROM_MAILBOX_QUEUE (*(const volatile uint32_t *)(uintptr_t)0x801214ccu)
#define ROM_STATUS_TIMER (*(const volatile uint32_t *)(uintptr_t)0x80120814u)
#define ROM_STATUS_NOTIFICATIONS (*(volatile uint8_t *)(uintptr_t)0x80127330u)
#define ROM_STATUS_CHANGE_DETECT (*(volatile uint8_t *)(uintptr_t)0x80127338u)
#define ROM_STATUS_SETUP_LINK (*(volatile uint8_t *)(uintptr_t)0x80120991u)
#define ROM_WLAN_AFH ((volatile uint8_t *)(uintptr_t)0x80124110u)
#define ROM_LIVE_AFH ((const volatile uint8_t *)(uintptr_t)0x80123f90u)
#define BT_SCOREBOARD (*(volatile uint32_t *)(uintptr_t)0xb000a0bcu)
#define BT_SCOREBOARD_START ((1u << 31) | (1u << 17) | (1u << 14))

typedef void (*init_hook_t)(void);
typedef unsigned int (*mailbox_gate_t)(void);
typedef unsigned int (*mailbox_hook_t)(const uint32_t *);
typedef unsigned int (*hci_hook_t)(const uint8_t *, uint8_t *);
typedef void (*apply_afh_t)(const uint8_t *);
typedef void (*send_status_t)(unsigned int);
#define ROM_LATE_INIT (*(init_hook_t volatile *)(uintptr_t)0x801209bcu)
#define ROM_MAILBOX_GATE ((mailbox_gate_t)(uintptr_t)0x8000fb05u)
#define ROM_MAILBOX_HOOK (*(mailbox_hook_t volatile *)(uintptr_t)0x80120994u)
#define ROM_HCI_HOOK (*(hci_hook_t volatile *)(uintptr_t)0x80120ad0u)
#define ROM_APPLY_AFH ((apply_afh_t)(uintptr_t)0x800500b5u)
#define ROM_SEND_STATUS ((send_status_t)(uintptr_t)0x8000da91u)

#define ROM_MAILBOX_OPCODE 0x7fu
#define ROM_STATUS_QUERY 0x23u
#define ROM_WLAN_CHANNEL 0x11u
#define ROM_STATUS_EVENT 0x27u
#define HCI_LE_CREATE_CONNECTION 0x200du

static void publish_coex_ready(void)
{
    /* ROM startup calls this hook after creating the mailbox queue. The
     * queue allocator returns -1 on failure. Use the same gate as ROM TX.
     */
    if (ROM_MAILBOX_QUEUE != UINT32_MAX && ROM_STATUS_TIMER != UINT32_MAX && ROM_MAILBOX_GATE())
        BT_SCOREBOARD = BT_SCOREBOARD_START;
}

static unsigned int apply_wlan_channel(uint32_t header, uint8_t span)
{
    uint8_t wlan[BT_AFH_BYTES];
    uint8_t live[BT_AFH_BYTES];
    if (!bt_wlan_afh_map(wlan, header >> 16, header >> 24, span))
        return 0;

    /* Keep native dispatch for rtw88's shared-antenna clear notice when
     * there is no prior WLAN exclusion to remove.
     */
    unsigned int updated = 0;
    for (unsigned int i = 0; i < BT_AFH_BYTES; ++i)
        updated |= ROM_WLAN_AFH[i] ^ wlan[i];
    if (!updated && !(header & 0xff0000u))
        return 0;

    bt_merge_wlan_afh(live, ROM_LIVE_AFH, ROM_WLAN_AFH, wlan);
    for (unsigned int i = 0; i < BT_AFH_BYTES; ++i)
        ROM_WLAN_AFH[i] = wlan[i];
    ROM_APPLY_AFH(live);
    ROM_SEND_STATUS(ROM_STATUS_EVENT);
    return 1;
}

static unsigned int handle_coex_mailbox(const uint32_t *message)
{
    /* ROM 0x80063038 clears both guards during controller reset. Enable
     * them on the next WLAN query and return 0 for native dispatch.
     */
    uint32_t header = message[0];
    uint32_t opcode = header & ROM_MAILBOX_OPCODE;
    if (opcode == ROM_STATUS_QUERY) {
        ROM_STATUS_NOTIFICATIONS = 1;
        ROM_STATUS_CHANGE_DETECT = 1;
    }
    if (opcode == ROM_WLAN_CHANNEL && ((header >> 8) & 0xffu) == 3u)
        return apply_wlan_channel(header, message[1]);
    return 0;
}

static unsigned int report_le_connection_setup(const uint8_t *command, uint8_t *group)
{
    (void)group;
    uint16_t opcode = command[0] | ((uint16_t)command[1] << 8);
    if (opcode == HCI_LE_CREATE_CONNECTION && command[2] == 25 &&
        ROM_STATUS_NOTIFICATIONS && ROM_MAILBOX_GATE()) {
        /* The ROM status builder copies this flag to bit 2 of HB1.
         * Publish the start of initiation and let native HCI dispatch run.
         */
        uint8_t saved = ROM_STATUS_SETUP_LINK;
        ROM_STATUS_SETUP_LINK = 1;
        ROM_SEND_STATUS(ROM_STATUS_EVENT);
        ROM_STATUS_SETUP_LINK = saved;
    }
    return 0;
}

void bt_enable_rom1_coex(void)
{
    /* The ROM owns the coexistence timers' lifetime across HCI resets. */
    ROM_COEX_TIMER_FLAGS |= ROM_COEX_TIMERS_ENABLE;
    /* ROM 0x80001296 uses this bit to dispatch scoreboard IRQs. ROM
     * 0x8000ec14 calls the late hook after its mailbox setup at 0x8000eb88.
     */
    ROM_COEX_FLAGS |= ROM_COEX_IRQ_ENABLE;
    uint8_t all_channels[BT_AFH_BYTES];
    (void)bt_wlan_afh_map(all_channels, 0, 0, 0);
    for (unsigned int i = 0; i < BT_AFH_BYTES; ++i)
        ROM_WLAN_AFH[i] = all_channels[i];
    ROM_MAILBOX_HOOK = handle_coex_mailbox;
    ROM_LATE_INIT = publish_coex_ready;
    ROM_HCI_HOOK = report_le_connection_setup;
}
