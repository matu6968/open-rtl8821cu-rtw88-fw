/* SPDX-License-Identifier: GPL-2.0-only */
#include "beacon.h"

#define CR          0xb8640100u
#define SYS_FUNC_EN 0xb8600002u
#define FE1ISR      0xb8640124u
#define BCN_CTRL    0xb8640550u
#define BCN_SPACE   0xb8640554u
#define TSF_LOW     0xb8640560u
#define RCR         0xb8640608u
#define BSSID       0xb8640618u
#define BCN_PSR     0xb86406a8u
#define BEACON_RX   0x00200000u
#define TIM_PARSER  0x00040000u
#define BCN_REPORT  4u

static uint32_t saved_report, saved_parser, connected;
volatile struct beacon_status g_beacon = {
    .magic = BEACON_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct beacon_status)
};
_Static_assert(sizeof(struct beacon_status) == 160, "BCN1 status ABI");

static uint32_t radio_running(void)
{
    uint32_t cr = diag_hw_read(CR);
    return (cr & 255u) != 0xeau && (cr & 0xc0u) == 0xc0u &&
           (compat_read8(SYS_FUNC_EN) & 3u) == 3u;
}

static void forget(void)
{
    g_beacon.owned = 0;
    g_beacon.context_valid = g_beacon.tsf_valid = 0;
    g_beacon.state = BEACON_OFF;
    g_beacon.error = BEACON_OK;
}

static void release_controls(void)
{
    if (!g_beacon.owned || !radio_running()) return;
    /* Only these two bits belong to this module. RCR is changed as one byte
     * so no unrelated key/decryption/filter byte is rewritten by a wide RMW.
     */
    compat_write8(BCN_CTRL, (compat_read8(BCN_CTRL) & ~BCN_REPORT) | saved_report);
    compat_write8(RCR + 2, (compat_read8(RCR + 2) & ~4u) | saved_parser);
}

void station_beacon_init(void)
{
    saved_report = saved_parser = connected = 0;
    forget();
}

void station_beacon_cancel(void)
{
    ++g_beacon.cancels;
    release_controls();
    connected = 0;
    forget();
}

void station_beacon_media(uint32_t value)
{
    ++g_beacon.media_commands;
    g_beacon.last_media = value ? 1u : 0u;
    if (!value) station_beacon_cancel();
    else {
        if (g_beacon.state == BEACON_FAILED) forget();
        connected = 1;
    }
}

static uint32_t context(uint32_t *lo, uint32_t *hi, uint32_t *aid, uint32_t *interval)
{
    uint32_t cr = diag_hw_read(CR);
    g_beacon.control = compat_read8(BCN_CTRL);
    g_beacon.rcr = diag_hw_read(RCR);
    *lo = diag_hw_read(BSSID);
    *hi = compat_read16(BSSID + 4);
    *aid = compat_read16(BCN_PSR) & 0x7ffu;
    *interval = compat_read16(BCN_SPACE);
    /* Port 0 must be associated infrastructure, with BSSID checking and
     * beacon TSF update enabled. A scan/monitor filter is not sufficient.
     */
    return ((cr >> 16) & 3u) == 2u &&
           (g_beacon.control & 0x58u) == 8u &&
           (g_beacon.rcr & 0x80u) && *aid && *aid <= 2007u && *interval &&
           !(*lo & 1u) && (*lo || *hi);
}

static uint32_t arm(uint32_t lo, uint32_t hi, uint32_t aid, uint32_t interval)
{
    saved_report = compat_read8(BCN_CTRL) & BCN_REPORT;
    saved_parser = compat_read8(RCR + 2) & 4u;
    g_beacon.owned = 1;
    compat_write8(BCN_CTRL, compat_read8(BCN_CTRL) | BCN_REPORT);
    compat_write8(RCR + 2, compat_read8(RCR + 2) | 4u);
    g_beacon.control = compat_read8(BCN_CTRL);
    g_beacon.rcr = diag_hw_read(RCR);
    if (!(g_beacon.control & BCN_REPORT) || !(g_beacon.rcr & TIM_PARSER)) {
        ++g_beacon.readback_errors;
        release_controls();
        forget();
        g_beacon.state = BEACON_FAILED;
        g_beacon.error = BEACON_READBACK;
        return 0;
    }
    /* FE1ISR is W1C. Drop any pre-association or pre-parser report exactly
     * once; it cannot establish a fresh beacon for this generation.
     */
    uint32_t pending = diag_hw_read(FE1ISR) & BEACON_RX;
    if (pending) ++g_beacon.stale_clears;
    diag_hw_write(FE1ISR, BEACON_RX);
    g_beacon.bssid_lo = lo; g_beacon.bssid_hi = hi;
    g_beacon.aid = aid; g_beacon.interval_tu = interval;
    g_beacon.context_valid = 1;
    g_beacon.tsf_valid = 0;
    g_beacon.state = BEACON_ARMED;
    g_beacon.error = BEACON_OK;
    ++g_beacon.arms;
    return 0;
}

uint32_t station_beacon_poll(struct beacon_event *event)
{
    ++g_beacon.polls;
    if (!connected) return 0;
    if (g_beacon.state == BEACON_FAILED) return 0;
    if (!radio_running()) {
        if (g_beacon.owned || g_beacon.context_valid) {
            ++g_beacon.cancels;
            /* Host reset/off invalidates ownership; no stale MMIO restore. */
            forget();
        }
        return 0;
    }
    uint32_t lo, hi, aid, interval;
    if (!context(&lo, &hi, &aid, &interval)) {
        if (g_beacon.context_valid || g_beacon.owned) {
            ++g_beacon.context_changes;
            release_controls();
            forget();
        }
        return 0;
    }
    if (!g_beacon.context_valid || lo != g_beacon.bssid_lo || hi != g_beacon.bssid_hi ||
        aid != g_beacon.aid || interval != g_beacon.interval_tu ||
        !(g_beacon.control & BCN_REPORT) || !(g_beacon.rcr & TIM_PARSER)) {
        if (g_beacon.context_valid || g_beacon.owned) {
            ++g_beacon.context_changes;
            release_controls();
            forget();
        }
        return arm(lo, hi, aid, interval);
    }
    g_beacon.last_pending = diag_hw_read(FE1ISR);
    if (!(g_beacon.last_pending & BEACON_RX)) {
        ++g_beacon.no_event_polls;
        return 0;
    }
    ++g_beacon.events;
    uint32_t report = diag_hw_read(BCN_PSR);
    uint32_t tsf = diag_hw_read(TSF_LOW);
    uint32_t now = foundation_now_us();
    diag_hw_write(FE1ISR, BEACON_RX);
    /* New beacon/report while copying: retain the new latch for next poll,
     * or reject a changed parser word. Do not reuse an ambiguous old TIM.
     */
    if (diag_hw_read(BCN_PSR) != report || (diag_hw_read(FE1ISR) & BEACON_RX)) {
        ++g_beacon.raced_events;
        g_beacon.state = BEACON_ARMED;
        g_beacon.tsf_valid = 0;
        return 0;
    }
    uint32_t period = (report >> 16) & 255u, count = report >> 24;
    /* Bits11..13 are not described by the vendor header. Real8821C reports
     * set bit13 (for example00022001), and stock80033C5E masks onlyC000
     * for its TIM/DTIM decision. Preserve those bits in raw PSR without
     * inventing a reserved-zero or parser-valid interpretation for them.
     */
    if ((report & 0x7ffu) != aid || !period || count >= period ||
        ((report & 0x8000u) && count)) {
        ++g_beacon.invalid_reports;
        g_beacon.last_psr = report;
        g_beacon.state = BEACON_ARMED;
        g_beacon.error = BEACON_REPORT;
        g_beacon.tsf_valid = 0;
        return 0;
    }
    uint32_t spacing = 0, delta = 0, missed = 0;
    if (g_beacon.tsf_valid) {
        spacing = now - g_beacon.last_local_us;
        delta = tsf - g_beacon.last_tsf;
        g_beacon.last_spacing_us = spacing;
        g_beacon.last_tsf_delta = delta;
        if (!delta || delta >= 0x80000000u || spacing >= 0x80000000u) {
            ++g_beacon.clock_resets;
            g_beacon.state = BEACON_ARMED;
            g_beacon.error = BEACON_CLOCK;
            g_beacon.tsf_valid = 0;
            return 0;
        }
        uint32_t interval_us = interval << 10;
        /* A latched bit can coalesce multiple arrivals. Count evidence of a
         * gap, not an invented exact number of received/missed packets.
         */
        if (spacing > interval_us + (interval_us >> 1) ||
            delta > interval_us + (interval_us >> 1)) {
            ++g_beacon.missing_intervals;
            /* This latch could predate a foreground stall. Discard it and
             * wait for the next post-clear event before resynchronizing.
             */
            g_beacon.state = BEACON_ARMED;
            g_beacon.error = BEACON_GAP;
            g_beacon.tsf_valid = 0;
            return 0;
        }
    }
    g_beacon.last_psr = report; g_beacon.last_tsf = tsf;
    g_beacon.last_local_us = now; g_beacon.last_spacing_us = spacing;
    g_beacon.last_tsf_delta = delta; g_beacon.dtim_count = count;
    g_beacon.dtim_period = period; g_beacon.tsf_valid = 1;
    g_beacon.state = BEACON_SYNCED; g_beacon.error = BEACON_OK;
    ++g_beacon.valid_events;
    if (report & 0x4000u) ++g_beacon.tim_unicast;
    if (report & 0x8000u) ++g_beacon.dtim_multicast;
    if (!count) ++g_beacon.dtim_beacons;
    if (event) {
        event->psr = report; event->aid = aid;
        event->dtim_count = count; event->dtim_period = period;
        event->interval_tu = interval; event->tsf_low = tsf;
        event->local_time_us = now; event->spacing_us = spacing;
        event->tsf_delta = delta; event->bssid_lo = lo; event->bssid_hi = hi;
        event->missed_interval = missed;
    }
    return 1;
}
