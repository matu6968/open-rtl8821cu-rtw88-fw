/* SPDX-License-Identifier: GPL-2.0-only */
#include "doze.h"
#include "beacon.h"
#include "radio.h"
#include "../broader/broader.h"
#include "../powersave/lps.h"
#include "../fwlog/fwlog.h"
#include "../deep/deep.h"

#define WAKE_GUARD_US 8000u
#define BEACON_LATE_US 30000u
volatile struct doze_status g_doze = {
    .magic=DOZE_MAGIC,.abi_version=1,.struct_bytes=sizeof(struct doze_status),
    .wake_guard_us=WAKE_GUARD_US
};
_Static_assert(sizeof(struct doze_status)==144,"DZE1 status ABI");

static uint32_t due(uint32_t now,uint32_t deadline)
{
    return (int32_t)(now-deadline)>=0;
}

static void unsync(void)
{
    g_doze.sync_streak=0;
    g_doze.next_wake_us=g_doze.beacon_deadline_us=0;
    g_doze.state=DOZE_WAIT_BEACON;
}

void station_doze_init(void)
{
    station_beacon_init();radio_init();
    g_doze.enabled=1;
    g_doze.state=DOZE_OBSERVING;
}

void station_doze_media(uint32_t connected)
{
    g_doze.connected=!!connected;
    station_beacon_media(connected);
    unsync();
    if (!connected) {
        ++g_doze.cancels;
        /* Register H2C was admitted only after an owned radio woke. */
        if (radio_owned()) radio_wake(RADIO_WAKE_POLICY);
        g_doze.state=DOZE_OBSERVING;
    }
}

uint32_t station_doze_pre(void)
{
    ++g_doze.polls;
    radio_poll();
    g_doze.last_radio_state=g_radio.state;
    g_doze.now_us=foundation_now_us();
    if (g_radio.enters!=g_doze.last_sleep_count) {
        g_doze.last_sleep_count=g_radio.enters;
        g_doze.sleeps=g_radio.enters;
    }
    if (g_radio.wakes!=g_doze.last_wake_count) {
        g_doze.last_wake_count=g_radio.wakes;
        g_doze.wakes=g_radio.wakes;
        /* Actual wake completion, not the requested wake time. */
        if (g_radio.wake_reason==RADIO_WAKE_TBTT && g_doze.next_wake_us &&
            due(g_doze.now_us,g_doze.next_wake_us+WAKE_GUARD_US)) ++g_doze.late_wakes;
    }
    if (g_radio.error) {
        if (!g_doze.error) ++g_doze.radio_failures;
        g_doze.error=g_radio.error;
        g_doze.state=DOZE_DISABLED;
        if (!radio_awake()) return 0;
        station_lps_request_active();
    }
    if (radio_owned()) {
        g_doze.state=radio_asleep()?DOZE_SLEEPING:DOZE_WAKING;
        return 0;
    }
    if (!radio_awake()) return 0;
    if (g_station.iqk_completed && !station_transport_active()) {
        if (g_doze.connected || g_beacon.context_valid) {
            g_doze.connected=0;++g_doze.cancels;
            station_beacon_cancel();station_lps_cancel();unsync();
        }
        /* Invalidate the report generation after an external MAC/BB reset.
         * Owned RF doze never calls this while its own BB bit is clear.
         */
        (void)station_transport_ready();
        fw_log_transport_lost();
        return 0;
    }
    return 1;
}

void station_doze_poll(void)
{
    if (!g_doze.connected || radio_owned() || !radio_awake()) return;
    struct beacon_event event;
    uint32_t fresh=station_beacon_poll(&event);
    uint32_t now=foundation_now_us();g_doze.now_us=now;
    g_doze.last_beacon_state=g_beacon.state;
    /* The host removes the linked beacon context for channel/filter
     * changes such as scans. Its old deadline cannot survive that change. */
    if (!g_beacon.context_valid) {
        if (g_doze.sync_streak) unsync();
        return;
    }
    /* A late fresh event must not erase the previous wake deadline. */
    if (g_doze.beacon_deadline_us && due(now,g_doze.beacon_deadline_us)) {
        ++g_doze.missed_beacons;++g_doze.recovery_wakes;
        unsync();station_lps_request_active();return;
    }
    if (fresh) {
        ++g_doze.fresh_beacons;
        if (event.interval_tu<20u || event.interval_tu>1000u) {
            unsync();station_lps_request_active();return;
        }
        uint32_t interval=event.interval_tu<<10;
        /* Wake for every beacon. A conservative guard covers foreground
         * observation delay and the bounded RF/cache restore, without an
         * unqualified TSF modulo/division or a claim to an RX timestamp.
         */
        if (interval<20480u || interval>1024000u || event.missed_interval) {
            unsync();station_lps_request_active();return;
        }
        if (!g_doze.sync_streak) ++g_doze.resyncs;
        if (g_doze.sync_streak<2u) ++g_doze.sync_streak;
        g_doze.last_beacon_us=event.local_time_us;
        g_doze.interval_us=interval;g_doze.last_psr=event.psr;
        g_doze.next_wake_us=event.local_time_us+interval-WAKE_GUARD_US;
        g_doze.beacon_deadline_us=event.local_time_us+interval+BEACON_LATE_US;
        if (g_lps.state==LPS_AP_PS && (event.psr&0xc000u)) {
            if (event.psr&0x4000u) ++g_doze.tim_wakes;
            if (event.psr&0x8000u) ++g_doze.dtim_wakes;
            g_doze.last_tim_us=event.local_time_us;g_doze.last_tim_psr=event.psr;
            g_doze.last_tim_sleep_start=g_radio.sleep_started;
            g_doze.last_tim_doze_us=g_radio.last_doze_us;
            diag_emit(50,event.local_time_us,event.psr);
            diag_emit(51,g_radio.sleep_started,g_radio.last_doze_us);
            diag_emit(53,event.local_time_us,event.tsf_low);
        }
        station_lps_beacon(event.psr);
    } else if (g_beacon.state!=BEACON_SYNCED) {
        if (g_doze.sync_streak) {
            ++g_doze.recovery_wakes;unsync();station_lps_request_active();
        }
    }
    if (g_doze.error) {g_doze.state=DOZE_DISABLED;return;}
    g_doze.state=g_doze.sync_streak<2u?DOZE_WAIT_BEACON:DOZE_OBSERVING;
    if (!g_doze.enabled || g_doze.sync_streak<2u || g_beacon.state!=BEACON_SYNCED ||
        g_lps.state!=LPS_AP_PS || g_lps.ap_ps!=1u || !g_lps.requested_mode ||
        !g_doze.next_wake_us || due(now+2000u,g_doze.next_wake_us)) return;
    if (deep_hold_radio_awake()) return;
    ++g_doze.sleep_attempts;
    if (radio_sleep(g_doze.next_wake_us)) g_doze.state=DOZE_WAKING;
}
