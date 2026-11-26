/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <string.h>

#include "broader/broader.h"
#include "coex/coex.h"
#include "doze/beacon.h"

#define TX 0xb8003400u
#define TX_CONTROL 0xb8003408u
#define RX 0xb8003410u
#define RX_CONTROL 0xb8003418u
#define QUERY 0x00000161u
#define CHANNEL 0x24060166u
#define TDMA 0x03356160u
#define TABLE 0xb86406ccu

volatile struct compat_status g_compat;
volatile struct diag_status g_diag;
volatile struct station_status g_station;
volatile struct beacon_status g_beacon;
uint8_t g_efuse_logical[EFUSE_SIZE];

static uint8_t chip[RX_CONTROL - TX + 1], tx_frame[8], c2h[9];
static uint32_t powered, transport, ap, busy, reports, writes;
static uint32_t now_us, table;

uint32_t station_transport_active(void) { return transport; }
uint32_t ap_port_active(void) { return ap; }
uint32_t foundation_now_us(void) { return now_us; }

uint32_t diag_hw_read(uint32_t address)
{
    assert(address == 0xb8600068u);
    return powered ? 1u << 18 : 0;
}

uint32_t compat_read8(uint32_t address)
{
    if (address == TABLE) return table;
    assert(address >= TX && address < TX + sizeof(chip));
    return chip[address - TX];
}

void compat_write8(uint32_t address, uint32_t value)
{
    if (address == TABLE) {
        assert((table & ~3u) == (value & ~3u));
        table = value;
        return;
    }
    assert(address >= TX && address < TX + sizeof(chip));
    ++writes;
    if (address == RX_CONTROL) {
        assert(value & 1);
        chip[address - TX] &= ~1u;
        return;
    }
    chip[address - TX] = value;
    if (address == TX_CONTROL)
        memcpy(tx_frame, chip, sizeof(tx_frame));
}

uint32_t station_c2h_send(const uint8_t *frame, uint32_t bytes)
{
    assert(bytes == sizeof(c2h));
    if (busy) return 0;
    memcpy(c2h, frame, bytes);
    ++reports;
    return 1;
}

static void arrive(uint8_t id, uint8_t first)
{
    chip[RX_CONTROL - TX] = 1;
    chip[RX - TX] = id;
    chip[RX - TX + 1] = 6;
    for (unsigned i = 0; i < 6; ++i)
        chip[RX - TX + 2 + i] = first + i;
}

int main(void)
{
    g_diag.chip.cut = 4;
    g_diag.chip.raw_sys_status1 = 0x400010c9;
    g_efuse_logical[0xca] = 0x21;
    g_compat.board.rf_board = 0x20;
    g_compat.board.bt_setting = 0x01;
    g_compat.board.bt_board = 1;

    transport = powered = 1;
    assert(!coex_configure());
    assert(!coex_h2c(QUERY, 0));
    coex_poll();
    assert(!writes);
    g_compat.board.valid = 1;
    g_compat.board.bt_board = 0;
    assert(!coex_configure());
    g_compat.board.bt_board = DIAG_UNKNOWN;
    assert(!coex_configure());
    g_compat.board.bt_board = 1;
    g_compat.board.bt_setting = 0xff;
    assert(!coex_configure());
    g_compat.board.bt_setting = 0x00;
    assert(!coex_configure());
    g_compat.board.bt_setting = 0x01;
    for (uint32_t strap = 0; strap < 4; ++strap) {
        if (strap == 1) continue;
        g_diag.chip.raw_sys_status1 = strap << 12;
        assert(!coex_configure());
        assert(!coex_h2c(QUERY, 0));
        coex_poll();
        assert(!writes);
    }
    g_diag.chip.raw_sys_status1 = 0x400010c9;
    assert(coex_configure());

    assert(coex_h2c(0x61, 0) == 2);
    assert(coex_h2c(CHANNEL, 1) == 2);
    assert(!coex_h2c(0x4b213560u, 0x1234));
    ap = 1;
    assert(!coex_h2c(QUERY, 0));
    ap = 0;
    powered = 0;
    assert(coex_h2c(QUERY, 0) == 1);
    assert(coex_h2c(CHANNEL, 0) == 1);
    coex_poll();
    assert(!writes);

    powered = 1;
    transport = 0;
    coex_poll();
    assert(!writes);
    transport = 1;
    chip[TX_CONTROL - TX] = 3;
    coex_poll();
    assert(!writes);
    chip[TX_CONTROL - TX] = 2;
    coex_poll();
    const uint8_t expected_query[] = {0x23, 1, 1, 0, 0, 0, 0, 0};
    assert(chip[TX_CONTROL - TX] == 3);
    assert(!memcmp(tx_frame, expected_query, sizeof(tx_frame)));
    coex_poll();
    assert(writes == 9);
    chip[TX_CONTROL - TX] = 0;
    coex_poll();
    const uint8_t expected_channel[] = {0x11, 3, 1, 6, 0x24, 0, 0, 0};
    assert(!memcmp(tx_frame, expected_channel, sizeof(tx_frame)));

    powered = 0;
    chip[TX_CONTROL - TX] = 0;
    coex_poll();
    assert(writes == 18);
    powered = 1;
    coex_poll();
    assert(writes == 27);
    assert(!memcmp(tx_frame, expected_channel, sizeof(tx_frame)));

    arrive(0x23, 0x10);
    busy = 1;
    g_station.c2h_seq = 255;
    coex_poll();
    assert(!chip[RX_CONTROL - TX]);
    assert(!reports && g_station.c2h_seq == 255);
    arrive(0x27, 0x20);
    coex_poll();
    assert(chip[RX_CONTROL - TX]);
    busy = 0;
    coex_poll();
    const uint8_t expected_report[] = {9, 255, 1, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    assert(!memcmp(c2h, expected_report, sizeof(c2h)));
    assert(reports == 1 && g_station.c2h_seq == 256);
    coex_poll();
    const uint8_t expected_event[] = {9, 0, 2, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
    assert(!memcmp(c2h, expected_event, sizeof(c2h)));
    assert(reports == 2 && g_station.c2h_seq == 257);
    arrive(0x30, 0x40);
    coex_poll();
    assert(!chip[RX_CONTROL - TX] && reports == 2);

    assert(coex_h2c(QUERY, 0) == 1);
    arrive(0x23, 0x30);
    busy = 1;
    coex_poll();
    coex_transport_lost();
    chip[TX_CONTROL - TX] = 0;
    unsigned prior_writes = writes;
    busy = 0;
    coex_poll();
    assert(reports == 2 && writes == prior_writes);

    table = 0x18;
    assert(coex_h2c(TDMA, 0x1011) == 1);
    coex_poll();
    assert(table == 0x1b);
    g_beacon.state = BEACON_SYNCED;
    g_beacon.tsf_valid = 1;
    g_beacon.interval_tu = 100;
    g_beacon.last_local_us = UINT32_MAX - 2000;
    now_us = g_beacon.last_local_us + 1000;
    coex_poll();
    assert(table == 0x18);
    now_us = g_beacon.last_local_us + 56000;
    coex_poll();
    assert(table == 0x1b);
    now_us = g_beacon.last_local_us + 100000;
    coex_poll();
    assert(table == 0x18);
    g_beacon.tsf_valid = 0;
    coex_poll();
    assert(table == 0x1b);
    g_beacon.tsf_valid = 1;
    coex_poll();
    assert(table == 0x18);
    assert(coex_h2c(0x60, 0) == 1);
    coex_poll();
    assert(table == 0x1b);
    assert(coex_h2c(TDMA, 0x1111) == 1);
    coex_poll();
    assert(table == 0x18);
    now_us = g_beacon.last_local_us + 200000;
    coex_poll();
    assert(table == 0x1b);
    now_us = g_beacon.last_local_us + 1000;
    coex_poll();
    assert(table == 0x18);
    assert(!coex_h2c(TDMA, 0x1211));
    coex_poll();
    assert(table == 0x1b);
    assert(coex_h2c(TDMA, 0x1111) == 1);
    coex_poll();
    assert(table == 0x18);
    arrive(0x27, 0x40);
    chip[RX - TX + 5] |= 4;
    coex_poll();
    assert(table == 0x1b);
    now_us += 4999999;
    g_beacon.last_local_us = now_us - 1000;
    coex_poll();
    assert(table == 0x1b);
    ++now_us;
    coex_poll();
    assert(table == 0x18);
    coex_transport_lost();
    assert(table == 0x1b);
    return 0;
}
