/* SPDX-License-Identifier: GPL-2.0-only */
#include "coex.h"
#include "../ap/ap.h"
#include "../broader/broader.h"
#include "../doze/beacon.h"

/* Byte registers used by the stock sideband transport at 80007dec/80007ef8. */
#define BT_TX_CTRL 0xb8003408u
#define BT_TX_CMD  0xb8003400u
#define BT_TX_LEN  0xb8003401u
#define BT_TX_DATA 0xb8003402u
#define BT_RX_CTRL 0xb8003418u
/* The RX data pointer is the ROM literal at 0x800151c0. */
#define BT_RX_CMD  0xb8003410u
#define BT_PENDING  1u
#define WL_BT_POWER 0xb8600068u
#define BT_COEX_TABLE_H 0xb86406ccu
#define BT_COEX_ANTENNA_BITS 3u
#define BT_FUNC_EN  (1u << 18)
#define SIDEBAND_PAYLOAD 6u
#define H2C_COEX_TDMA 0x60u
#define H2C_BT_INFO 0x61u
#define H2C_WL_CHANNEL 0x66u
#define C2H_BT_INFO 0x09u
#define BT_INFO_REPLY 0x23u
#define BT_INFO_EVENT 0x27u
#define BT_WL_CHANNEL 0x11u
#define BT_INFO_SETUP_LINK (1u << 2)
#define TDMA_BEACON_LEAD_US (3u * 1024u)
#define TDMA_LINK_SETUP_US 5000000u

static uint8_t allowed, bt_live, query_pending, channel_known, channel_pending;
static uint8_t channel[3];
static uint8_t report[3 + SIDEBAND_PAYLOAD];
static uint8_t report_pending;
static uint8_t tdma_update_pending;
static uint8_t tdma_table_bits = UINT8_MAX;
static uint8_t tdma_link_setup;
static uint32_t tdma_window_us;
static uint32_t tdma_setup_started;

static void tdma_select_antenna(uint8_t bits)
{
    if (tdma_table_bits == bits && !tdma_update_pending)
        return;

    uint8_t control = compat_read8(BT_COEX_TABLE_H);
    compat_write8(BT_COEX_TABLE_H, (control & ~BT_COEX_ANTENNA_BITS) | bits);
    tdma_table_bits = bits;
}

void coex_transport_lost(void)
{
    if (allowed && tdma_table_bits == 0 && station_transport_active())
        tdma_select_antenna(BT_COEX_ANTENNA_BITS);
    tdma_window_us = tdma_update_pending = tdma_link_setup = 0;
    tdma_table_bits = UINT8_MAX;
    bt_live = query_pending = channel_known = channel_pending = report_pending = 0;
}

uint32_t coex_configure(void)
{
    /* Cut and the raw eFuse bytes 0xca, 0xc1 and 0xc3. The live Bluetooth
     * enable bit can be clear when Linux probes the WLAN interface first.
     */
    uint32_t profile = (g_diag.chip.cut << 24) | ((uint32_t)g_efuse_logical[0xca] << 16) |
                       (g_compat.board.rf_board << 8) | g_compat.board.bt_setting;
    allowed = diag_uses_usb() && g_compat.board.valid && g_compat.board.bt_board == 1 &&
              g_compat.board.bt_setting != 0xff && profile == RTL8821CU_COEX_BOARD;
    coex_transport_lost();
    return allowed;
}

static uint32_t tdma_command(uint32_t lo, uint32_t hi)
{
    unsigned int first_slot = (lo >> 16) & 0xffu;
    /* Shared-antenna two-slot mode observed on this board. Stock setup
     * selects both antenna bits for this mode and for TDMA disable.
     */
    if (lo == H2C_COEX_TDMA && !hi)
        tdma_window_us = 0;
    else if ((lo & 0xff00ffffu) == 0x03006160u && first_slot &&
             (hi == 0x1011u || hi == 0x1111u))
        tdma_window_us = (first_slot + 3u) << 10;
    else {
        if (tdma_window_us)
            tdma_update_pending = 1;
        tdma_window_us = 0;
        return 0;
    }

    tdma_update_pending = 1;
    return 1;
}

uint32_t coex_h2c(uint32_t lo, uint32_t hi)
{
    uint32_t id = lo & 255u;
    if (!allowed || ap_port_active())
        return 0;
    if (id == H2C_COEX_TDMA)
        return tdma_command(lo, hi);
    if (id != H2C_BT_INFO && id != H2C_WL_CHANNEL)
        return 0;
    if (hi) return 2;
    /* Mailbox completion records that the request was queued. A C2H status
     * report is generated only after the controller sends data.
     */
    if (id == H2C_BT_INFO) {
        if (lo != (H2C_BT_INFO | (1u << 8))) return 2;
        query_pending = 1;
    } else {
        channel[0] = lo >> 8;
        channel[1] = lo >> 16;
        channel[2] = lo >> 24;
        channel_known = channel_pending = 1;
    }
    return 1;
}

static void tdma_poll(void)
{
    if (!tdma_window_us && !tdma_update_pending && !tdma_link_setup)
        return;

    uint32_t now = foundation_now_us();
    if (tdma_link_setup && now - tdma_setup_started >= TDMA_LINK_SETUP_US)
        tdma_link_setup = 0;

    uint8_t bits = BT_COEX_ANTENNA_BITS;
    if (!tdma_link_setup && tdma_window_us &&
        g_beacon.state == BEACON_SYNCED && g_beacon.tsf_valid) {
        uint32_t interval_us = g_beacon.interval_tu << 10;
        uint32_t elapsed = now - g_beacon.last_local_us;
        if (interval_us > tdma_window_us + TDMA_BEACON_LEAD_US &&
            elapsed < interval_us + (interval_us >> 1)) {
            /* The stock waveform clears the bits in the first timed
             * window and sets them through the rest of the beacon cycle.
             */
            uint32_t phase = elapsed + TDMA_BEACON_LEAD_US;
            if (phase >= interval_us)
                phase -= interval_us;
            if (phase < tdma_window_us)
                bits = 0;
        }
    }
    tdma_select_antenna(bits);
    tdma_update_pending = 0;
}

static void sideband_send(uint8_t control, uint8_t cmd, const uint8_t *data, uint8_t length)
{
    compat_write8(BT_TX_CMD, cmd);
    compat_write8(BT_TX_LEN, length);
    for (uint32_t i = 0; i < SIDEBAND_PAYLOAD; ++i)
        compat_write8(BT_TX_DATA + i, i < length ? data[i] : 0);
    compiler_barrier();
    compat_write8(BT_TX_CTRL, control | BT_PENDING);
}

static void receive_report(void)
{
    uint32_t control = compat_read8(BT_RX_CTRL);
    if (!(control & BT_PENDING)) return;
    uint32_t cmd = compat_read8(BT_RX_CMD);
    if (cmd == BT_INFO_REPLY || cmd == BT_INFO_EVENT) {
        /* Stock dispatch reads the six status bytes at offsets 2 through 7. */
        report[0] = C2H_BT_INFO;
        report[2] = cmd == BT_INFO_REPLY ? 1 : 2;
        for (uint32_t i = 0; i < SIDEBAND_PAYLOAD; ++i)
            report[3 + i] = compat_read8(BT_RX_CMD + 2 + i);
        /* Hold the setup grant across the controller's first BLE events.
         * HB1 bit 2 is the existing rtw88 link-setup indication.
         */
        if (cmd == BT_INFO_EVENT && (report[6] & BT_INFO_SETUP_LINK)) {
            tdma_setup_started = foundation_now_us();
            tdma_link_setup = 1;
        }
        report_pending = 1;
    }
    compiler_barrier();
    compat_write8(BT_RX_CTRL, control | BT_PENDING);
}

void coex_poll(void)
{
    if (!allowed || !station_transport_active()) return;
    if (!(diag_hw_read(WL_BT_POWER) & BT_FUNC_EN)) {
        if (tdma_table_bits == 0)
            tdma_select_antenna(BT_COEX_ANTENNA_BITS);
        tdma_link_setup = 0;
        bt_live = report_pending = 0;
        return;
    }
    if (!bt_live) {
        bt_live = 1;
        channel_pending = channel_known;
    }

    if (!report_pending) receive_report();
    tdma_poll();
    if (report_pending) {
        report[1] = g_station.c2h_seq;
        if (station_c2h_send(report, sizeof(report))) {
            ++g_station.c2h_seq;
            report_pending = 0;
        }
    }

    if (!query_pending && !channel_pending) return;
    uint8_t control = compat_read8(BT_TX_CTRL);
    if (control & BT_PENDING) return;
    if (query_pending) {
        const uint8_t request = 1;
        sideband_send(control, BT_INFO_REPLY, &request, sizeof(request));
        query_pending = 0;
    } else {
        sideband_send(control, BT_WL_CHANNEL, channel, sizeof(channel));
        channel_pending = 0;
    }
}
