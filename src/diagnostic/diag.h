/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_DIAG_H
#define RTL8811CU_DIAG_H

#include <stdint.h>
#include <stddef.h>

#define DIAG_MAGIC 0x31414944u /* DIA1, little endian */
#define DIAG_LAYOUT_MAGIC 0x314c4944u /* DIL1 */
#define DIAG_ABI_VERSION 1u
#define DIAG_FW_VERSION 0x00020000u
#define DIAG_RING_CAPACITY 64u
#define DIAG_UNKNOWN 0xffffffffu
#define DIAG_BAD_ADDRESS 0xbadadd00u
#define DIAG_ABI_ALL 0x3fu
#define DIAG_READ_START 0x80200200u
#define DIAG_READ_END 0x80205000u
#define DIAG_STACK_BOTTOM 0x80205000u
#define DIAG_STACK_TOP 0x80206000u

enum diag_state { DIAG_STARTING, DIAG_RUNNING, DIAG_FAILED };
enum diag_event_id {
    EVENT_BOOT = 1, EVENT_ABI = 2, EVENT_CHIP = 3, EVENT_HEARTBEAT = 4,
    EVENT_BT_DISABLED = 5, EVENT_HOST_INTERFACE = 6, EVENT_ERROR = 255
};
enum diag_error { ERROR_NONE, ERROR_ABI, ERROR_STACK, ERROR_FOUNDATION };
enum diag_hci { DIAG_HCI_UNKNOWN = 0, DIAG_HCI_USB = 1, DIAG_HCI_SDIO = 2, DIAG_HCI_PCIE = 3 };

/* Keep runtime function flags and the factory board observation independent.
 * All ABI members are u32, including boolean and unknown-valued observations.
 */
struct chip_info {
    uint32_t raw_sys_cfg1;
    uint32_t raw_sys_status1;
    uint32_t raw_wl_bt_power;
    uint32_t cut;
    uint32_t vendor_id;
    uint32_t test_chip;
    uint32_t rf_paths;
    uint32_t rom_revision;
    uint32_t wifi_func_flag;
    uint32_t bt_func_flag;
    uint32_t bt_board_flag;
    uint32_t bt_enabled;
    uint32_t bt_supported;
};

struct diag_status {
    uint32_t magic, abi_version, struct_bytes, fw_version, state;
    uint32_t progress, mailbox_reads, invalid_requests, last_error;
    uint32_t abi_result, stack_ok, led_state; /* led_state is reserved in ABI v1. */
    uint32_t ring_head, ring_count, ring_capacity, ring_addr, ring_overwrites;
    struct chip_info chip;
};

struct diag_event {
    uint32_t sequence, event_id, arg0, arg1;
};

_Static_assert(sizeof(struct chip_info) == 52, "chip ABI");
_Static_assert(sizeof(struct diag_status) == 120, "status ABI");
_Static_assert(sizeof(struct diag_event) == 16, "event ABI");

extern volatile struct diag_status g_diag;
extern volatile struct diag_event g_events[DIAG_RING_CAPACITY];
extern volatile uint32_t g_abi_results[6];

/* Stock WLAN 24.11.0 at 0x80102014 reads SYS_STATUS1 (0x00f4).
 * Keep interface IDs numerically aligned with the stock decoder.
 */
static inline uint32_t diag_hci_strap(uint32_t status)
{
    return (status >> 12) & 3u;
}

static inline enum diag_hci diag_host_interface(uint32_t status)
{
    if (status == DIAG_UNKNOWN)
        return DIAG_HCI_UNKNOWN;
    uint32_t strap = diag_hci_strap(status);
    if (strap == 0)
        return DIAG_HCI_SDIO;
    if (strap == 1)
        return DIAG_HCI_USB;
    return DIAG_HCI_PCIE;
}

static inline uint32_t diag_uses_usb(void)
{
    return diag_hci_strap(g_diag.chip.raw_sys_status1) == 1u;
}

static inline void compiler_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

#ifdef DIAG_HOST_TEST
uint32_t diag_hw_read(uint32_t address);
void diag_hw_write(uint32_t address, uint32_t value);
#else
static inline uint32_t diag_hw_read(uint32_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}
static inline void diag_hw_write(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}
#endif

void diag_decode_chip(volatile struct chip_info *info, uint32_t cfg,
                      uint32_t status, uint32_t multifunction);
int diag_set_bt_enabled(volatile struct chip_info *info, uint32_t enabled);
void diag_log_chip(const volatile struct chip_info *info);
void diag_emit(uint32_t event, uint32_t arg0, uint32_t arg1);
uint32_t diag_abi_test(void);
uint32_t diag_read_sp(void);
void diag_mailbox_poll(void);
_Noreturn void diag_main(void);

#endif
