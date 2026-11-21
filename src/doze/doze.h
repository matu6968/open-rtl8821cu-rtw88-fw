/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_DOZE_H
#define RTL8811CU_DOZE_H
#include "../station/station.h"
#define DOZE_MAGIC 0x31455a44u /* DZE1 */
enum doze_state { DOZE_OBSERVING, DOZE_WAIT_BEACON, DOZE_SLEEPING,
    DOZE_WAKING, DOZE_DISABLED };
struct doze_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t connected, enabled, polls, fresh_beacons, sync_streak;
    uint32_t sleep_attempts, sleeps, wakes, missed_beacons, resyncs;
    uint32_t tim_wakes, dtim_wakes, recovery_wakes, radio_failures, cancels;
    uint32_t last_beacon_us, interval_us, next_wake_us, beacon_deadline_us;
    uint32_t now_us, last_psr, last_radio_state, last_beacon_state;
    uint32_t last_sleep_count, last_wake_count, wake_guard_us, late_wakes;
    uint32_t last_tim_us, last_tim_psr, last_tim_sleep_start, last_tim_doze_us;
};
extern volatile struct doze_status g_doze;
void station_doze_init(void);
void station_doze_media(uint32_t connected);
/* Before packet/report/LPS work: progress owned RF state first. */
uint32_t station_doze_pre(void);
/* After packet/report/rate/LPS work: consume fresh beacon, then consider doze. */
void station_doze_poll(void);
#endif
