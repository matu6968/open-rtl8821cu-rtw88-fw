/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"
#include "../fwlog/fwlog.h"

/* Register meanings from the bundled 8821C vendor driver read_chip_version
 * and halmac_bit_8821c.h. In particular WL_FUNC_EN is bit 2, BT_FUNC_EN bit 18.
 * Preserve complete raw words for later SKU/board interpretation.
 */
void diag_decode_chip(volatile struct chip_info *info, uint32_t cfg,
                      uint32_t status, uint32_t multifunction)
{
    info->raw_sys_cfg1 = cfg;
    info->raw_sys_status1 = status;
    info->raw_wl_bt_power = multifunction;
    info->cut = (cfg >> 12) & 15;
    info->vendor_id = (cfg >> 16) & 15;
    info->test_chip = (cfg >> 23) & 1;
    info->rf_paths = (cfg & (1u << 27)) ? 2 : 1;
    info->rom_revision = status >> 28;
    info->wifi_func_flag = (multifunction >> 2) & 1;
    info->bt_func_flag = (multifunction >> 18) & 1;
    info->bt_board_flag = DIAG_UNKNOWN;
    info->bt_enabled = 0;
    info->bt_supported = 0;
}

/* Bluetooth activation is unsupported for every board profile. */
int diag_set_bt_enabled(volatile struct chip_info *info, uint32_t enabled)
{
    info->bt_enabled = 0;
    return enabled ? -1 : 0;
}

void diag_log_chip(const volatile struct chip_info *info)
{
    uint32_t status = info->raw_sys_status1;
    enum diag_hci hci = diag_host_interface(status);
    if (hci == DIAG_HCI_PCIE) {
        fw_log(FW_LOG_ALWAYS, "RTL8821C: PCIe strap %bx", diag_hci_strap(status));
        return;
    }
    if (hci == DIAG_HCI_SDIO) {
        fw_log(FW_LOG_ALWAYS, "RTL8821C: SDIO strap %bx", diag_hci_strap(status));
        return;
    }
    /* The eFuse marker describes the board even while Bluetooth is off.
     * A contradictory or missing board observation leaves the variant unknown.
     */
    if (hci == DIAG_HCI_USB && info->bt_board_flag == 1)
        fw_log(FW_LOG_ALWAYS, "RTL8821CU: Bluetooth not yet supported");
    else if (hci == DIAG_HCI_USB && info->bt_board_flag == 0 && info->bt_func_flag == 0)
        fw_log(FW_LOG_ALWAYS, "RTL8811CU: Wi-Fi only");
    else
        fw_log(FW_LOG_ALWAYS, "RTL8821C: variant unknown");
}
