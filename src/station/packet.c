/* SPDX-License-Identifier: GPL-2.0-only */
#include "station.h"
#include "../fwlog/fwlog.h"

void station_packet(const uint32_t *p)
{
    uint32_t length = p[1] & 0xffff, id = p[0] >> 16;
    ++g_station.packet_count;
    g_station.last_id = id; g_station.last_seq = p[1] >> 16;
    for (uint32_t i = 0; i < 8; ++i)
        g_station.last_packet[i] = p[i];
    if ((p[0] & 0xffff) != 0xff01 || length < 8 || length > 32) {
        ++g_station.packet_bad;
    } else if (id == 0x0d && length == 12) {
        ++g_station.general_count;
        g_station.fw_boundary = (p[2] >> 16) & 255;
    } else if (id == 0x11 && length == 16) {
        ++g_station.phydm_count;
        g_station.rfe = p[2] & 255;
        g_station.rf_type = (p[2] >> 8) & 255;
        g_station.cut = (p[2] >> 16) & 255;
        g_station.antenna = p[2] >> 24;
    } else if (id == 0x0e && length == 9) {
        /* The host has set up the RX FIFO by the first valid IQK request.
         * IQK failure text must be deliverable without enabling TX reports. */
        fw_log_transport_start();
        ++g_station.iqk_requests;
        g_station.last_iqk = p[2] & 3;
        uint32_t start = foundation_now_us();
        uint32_t errors = station_iqk(p[2] & 1);
        g_station.iqk_access_errors = errors;
        g_station.iqk_duration_us = foundation_now_us() - start;
        uint32_t report = diag_hw_read(0xb8641bf0u);
        g_station.iqk_fail_mask = report;
        if (errors & STATION_IQK_TIMEOUT)
            fw_log(FW_LOG_ALWAYS, "IQK: calibration timed out (status 0x%bx)", errors);
        else if (errors)
            fw_log(FW_LOG_ALWAYS, "IQK: request failed (status 0x%bx)", errors);
        else if (report & 0xffu)
            fw_log(FW_LOG_ALWAYS, "IQK: calibration failed (TX/RX mask 0x%bx)", report);
        if (!errors) {
            /* Rearm after a host BB/MAC reset only once real calibration has
             * completed again. Ordinary thermal IQK preserves live reports.
             */
            station_tx_report_start();
            ++g_station.iqk_completed;
        }
    } else {
        ++g_station.packet_unsupported;
    }
}

void station_packet_poll(void)
{
    uint32_t write = diag_hw_read(0xb8640250u) & 0x3ffff;
    uint32_t read = diag_hw_read(0xb864024cu) & 0x3ffff;
    if (read == write)
        return;
    uint32_t head = diag_hw_read(0xb8640244u) & 0x3ffff;
    uint32_t tail = diag_hw_read(0xb8640248u) & 0x3ffff;
    uint32_t stride = 16u << (compat_read8(0xb8640254u) & 3);
    g_station.head = head; g_station.tail = tail;
    g_station.read = read; g_station.write = write; g_station.stride = stride;
    /* No SRAM access until the host has installed a bounded, aligned queue.
     * Stock 800368a4 advances the current TAIL slot to HEAD, inclusive.
     * Copy the whole command before releasing ownership, and process at most
     * four slots per foreground iteration so diagnostics remain responsive.
     */
    if (stride != 32 || !head || tail < head || tail - head > 4096 ||
        (head | tail | read | write) & 31 || read < head || read > tail ||
        write < head || write > tail)
        return;
    for (uint32_t budget = 0; budget < 4 && read != write; ++budget) {
        uint32_t p[8];
        for (uint32_t i = 0; i < 8; ++i) {
            p[i] = diag_hw_read(0xb8780000u + read + (i << 2));
        }
        station_packet(p);
        if (read == tail) {
            read = head;
            ++g_station.wraps;
        } else {
            read += stride;
        }
        compiler_barrier();
        diag_hw_write(0xb864024cu, read);
    }
    g_station.read = read;
}
