/* SPDX-License-Identifier: GPL-2.0-only */
#include "compat.h"
#include "../broader/broader.h"
#include "../coex/coex.h"
#include "../doze/radio.h"
#include "../fwlog/fwlog.h"

#define MAC 0xb8600000u
#define MCU 0xb8640000u
#define MCU_HMETFR 0xb86401ccu /* write one box bit to acknowledge */
volatile struct compat_status g_compat = {
    .magic = COMPAT_MAGIC, .abi_version = 1, .struct_bytes = sizeof(struct compat_status)
};
uint8_t g_efuse_physical[EFUSE_SIZE], g_efuse_logical[EFUSE_SIZE];
uint8_t g_feature[FEATURE_SIZE];

static uint32_t read_factory(void)
{
    uint32_t ctl;
    /* WiFi bank, programming LDO disabled. A read always writes FLAG=0.
     * No code path enables programming voltage or issues FLAG=1 writes.
     * Finish all reads before publishing ready: Linux subsequently owns eFuse.
     */
    compat_write8(MAC + 0x37, compat_read8(MAC + 0x37) & 0x7f);
    diag_hw_write(MAC + 0x34, diag_hw_read(MAC + 0x34) & ~0x300u);
    ctl = diag_hw_read(MAC + 0x30) & ~0x8003ffffu;
    for (uint32_t i = 0; i < EFUSE_SIZE; ++i) {
        diag_hw_write(MAC + 0x30, ctl | (i << 8));
        uint32_t attempts = 100000, v;
        do {
            v = diag_hw_read(MAC + 0x30);
            if (v & 0x80000000u)
                break;
        } while (--attempts);
        if (!attempts)
            return COMPAT_EFUSE_TIMEOUT;
        g_efuse_physical[i] = v;
        g_compat.efuse_bytes = i + 1;
    }
    return COMPAT_OK;
}

void compat_init(void)
{
    uint32_t used = 0, error;
    g_compat.feature_requested = compat_read8(MCU + 0x1a0) == 0xfd;
    error = read_factory();
    if (!error)
        error = compat_expand_efuse(g_efuse_physical, EFUSE_USABLE, g_efuse_logical, &used);
    g_compat.efuse_used = used;
    if (!error) {
        compat_decode_board(g_efuse_logical,
                            diag_host_interface(g_diag.chip.raw_sys_status1),
                            &g_compat.board);
        if (!g_compat.board.valid)
            error = COMPAT_BAD_BOARD;
    }
    g_diag.chip.bt_board_flag = error ? DIAG_UNKNOWN : g_compat.board.bt_board;
    if (!error && coex_configure())
        fw_log(FW_LOG_ALWAYS, "RTL8821CU: experimental Bluetooth mailbox");
    else
        diag_log_chip(&g_diag.chip);
    if (error) {
        g_compat.error = error;
        g_compat.state = COMPAT_FAILED;
        diag_emit(EVENT_CONTROL_ERROR, error, g_compat.efuse_bytes);
        return;
    }
    diag_emit(EVENT_FACTORY, g_compat.board.rtl_id, g_compat.board.rfe);
    compat_build_feature(g_efuse_physical, g_feature);
    if (g_compat.feature_requested) {
        for (uint32_t i = 0; i < FEATURE_SIZE; ++i)
            compat_write8(MCU + 0x1a2 + i, g_feature[i]);
        compat_write8(MCU + 0x1a1, 0); /* sequence */
        compiler_barrier();
        compat_write8(MCU + 0x1a0, 0x19); /* publish ID last, stock 80037a38 */
        ++g_compat.feature_reports;
        diag_emit(EVENT_FEATURE, 0x19, FEATURE_SIZE);
    }
    /* Exactly the halfword RMW used at stock entry 80030184. A word write
     * also touches adjacent MCU control bits and is not interchangeable.
     */
    compat_write16(MAC + 0x80, compat_read16(MAC + 0x80) | 0x8000);
    g_compat.ready = 1;
    g_compat.state = COMPAT_READY;
}

void compat_poll(void)
{
    if (g_compat.state != COMPAT_READY)
        return;
    /* CR/BB become enabled before RF/cache restore finishes. Defer H2C
     * ownership until the full radio transaction has released its owner.
     */
    if (radio_owned()) return;
    if (g_station.iqk_completed && !station_transport_active()) return;
    /* Stock capture 80036a58 reads the four HMEBOX/EX pairs via MAC and
     * releases each only after copying it, through the MCU W1C alias.
     * This stage consumes unsupported commands so the transport cannot wedge;
     * no RF, power, coex, or packet action is reported as completed.
     */
    uint32_t pending = compat_read8(MCU_HMETFR) & 15;
    for (uint32_t box = 0; box < 4; ++box) {
        if (!(pending & (1u << box)))
            continue;
        uint32_t lo = diag_hw_read(MCU + 0x1d0 + (box << 2));
        uint32_t hi = diag_hw_read(MCU + 0x1f0 + (box << 2));
        g_compat.last_box = box;
        g_compat.last_h2c_lo = lo; g_compat.last_h2c_hi = hi;
        ++g_compat.h2c_count;
        uint32_t handled = broader_register_h2c(lo, hi);
        if (!handled) {
        ++g_compat.h2c_unsupported;
        diag_emit(EVENT_H2C_UNSUPPORTED, lo, hi);
        }
        compiler_barrier();
        compat_write8(MCU_HMETFR, 1u << box);
    }
}
