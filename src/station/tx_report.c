/* SPDX-License-Identifier: GPL-2.0-only */
#include "station.h"
#include "../broader/broader.h"
#include "rate.h"
static uint32_t report_ready;

uint32_t station_transport_active(void)
{
    /* Host power-off clears BB reset first, while CR can still say TX/RX on.
     * The report producer/SRAM is unreliable during that transition. The low
     * SYS_FUNC_EN byte uses the qualified B860 alias, like other 00xx MAC I/O.
     */
    uint32_t cr = compat_read8(0xb8640100u);
    return cr != 0xeau && (cr & 0xc0u) == 0xc0u &&
           (compat_read8(0xb8600002u) & 3u) == 3u;
}

uint32_t station_transport_ready(void)
{
    if (!station_transport_active()) report_ready = 0;
    return report_ready;
}

void station_tx_report_start(void)
{
    /* A warm firmware download can expose the prior MAC's producer/payload
     * before host FIFO setup and RF calibration reset the report engine.
     * Drop that stale generation once, after initialization/calibration and
     * before the host's first management transmission. No TX status is made
     * from uninitialized SRAM contents.
     */
    if (station_transport_ready()) return;
    if (!station_transport_active()) return;
    compat_write8(0xb864047du,compat_read8(0xb864047cu) & 127);
    report_ready = 1;
}

static uint32_t c2h_transport_ready(const uint8_t *frame)
{
    /* The log producer has its own RX FIFO epoch, including failed IQK. */
    return frame[0] == 0 ? station_transport_active() : station_transport_ready();
}

uint32_t station_c2h_send(const uint8_t *frame, uint32_t bytes)
{
    if (!bytes || bytes > 32) return 0;
    if (!c2h_transport_ready(frame)) return 0;
    if (compat_read8(0xb8640296u) & 1) {
        ++g_station.c2h_busy;
        return 0;
    }
    uint32_t start = (diag_hw_read(0xb864011cu) & 0x3ffff) + 1;
    if (start & 3 || start > 0x3ffc0) {
        ++g_station.c2h_bad_ptr;
        return 0;
    }
    compat_write16(0xb8640294u,start & 0xffff);
    compat_write8(0xb8640297u,(compat_read8(0xb8640297u) & 0xf0) | (start >> 16));
    if (!c2h_transport_ready(frame)) return 0;
    uint32_t base = 0xb8700000u + start;
    for (uint32_t i = 0; i < 6; ++i) diag_hw_write(base + (i << 2),0);
    diag_hw_write(base,0x80000000u | bytes);
    diag_hw_write(base+8,0x10000000u);
    for (uint32_t offset = 0; offset < bytes; offset += 4) {
        uint32_t word = 0;
        for (uint32_t j = 0; j < 4 && offset+j < bytes; ++j)
            word |= (uint32_t)frame[offset+j] << (j << 3);
        diag_hw_write(base+24+offset,word);
    }
    compiler_barrier();
    if (!c2h_transport_ready(frame)) return 0;
    compat_write8(0xb8640296u,1);
    return 1;
}

static uint32_t c2h_tx_report(const uint32_t report[4])
{
    uint8_t frame[20];
    frame[0] = 255; frame[1] = g_station.c2h_seq;
    frame[2] = 15; frame[3] = 16;
    for (uint32_t i = 0; i < 16; ++i)
        frame[i+4] = report[i >> 2] >> ((i & 3) << 3);
    if (!station_c2h_send(frame,sizeof frame)) return 0;
    ++g_station.c2h_seq;
    return 1;
}

void station_tx_report_poll(void)
{
    if (!g_station.iqk_completed)
        return;
    if (!station_transport_ready()) return;
    /* Stock 8003B4A0: 128 x 16-byte report ring, CPU SRAM at B8661000;
     * hardware producer 047C and MCU byte consumer 047D. Report type 2 maps
     * through service 8020725C to 8003B400, the FF/0F/10 CCX forwarder.
     */
    uint32_t read = compat_read8(0xb864047du) & 127;
    uint32_t write = compat_read8(0xb864047cu) & 127;
    for (uint32_t budget = 0; budget < 4 && read != write; ++budget) {
        if (!station_transport_ready()) return;
        uint32_t report[4];
        for (uint32_t i = 0; i < 4; ++i)
            report[i] = diag_hw_read(0xb8661000u + (read << 4) + (i << 2));
        if (!station_transport_ready()) return;
        uint32_t type = (report[0] >> 5) & 7;
        if (type == 2) {
            if (!c2h_tx_report(report)) return;
            ++g_station.tx_forwarded;
        } else {
            if (!type) station_rate_tx_report(report);
            ++g_station.tx_other; /* non-CCX reports; RA is separate in broad builds */
        }
        ++g_station.tx_reports;
        for (uint32_t i = 0; i < 4; ++i)
            g_station.last_tx_report[i] = report[i];
        read = (read + 1) & 127;
        compiler_barrier();
        compat_write8(0xb864047du,read);
    }
}
