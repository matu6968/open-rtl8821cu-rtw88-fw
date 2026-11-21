/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"

/* Host offsets come from the target compiler, never from a host struct ABI.
 * Ordering is versioned by DIAG_ABI_VERSION and named in format.py.
 */
#define OFF(member) ((uint32_t)offsetof(struct diag_status, member))
__attribute__((section(".diag_layout"), used))
const uint32_t diag_layout[] = {
    DIAG_LAYOUT_MAGIC, DIAG_ABI_VERSION, sizeof(struct diag_status),
    sizeof(struct diag_event), DIAG_RING_CAPACITY, 30,
    OFF(magic), OFF(abi_version), OFF(struct_bytes), OFF(fw_version), OFF(state),
    OFF(progress), OFF(mailbox_reads), OFF(invalid_requests), OFF(last_error),
    OFF(abi_result), OFF(stack_ok), OFF(led_state), OFF(ring_head), OFF(ring_count),
    OFF(ring_capacity), OFF(ring_addr), OFF(ring_overwrites),
    OFF(chip.raw_sys_cfg1), OFF(chip.raw_sys_status1), OFF(chip.raw_wl_bt_power),
    OFF(chip.cut), OFF(chip.vendor_id), OFF(chip.test_chip), OFF(chip.rf_paths),
    OFF(chip.rom_revision), OFF(chip.wifi_func_flag), OFF(chip.bt_func_flag),
    OFF(chip.bt_board_flag), OFF(chip.bt_enabled), OFF(chip.bt_supported)
};
