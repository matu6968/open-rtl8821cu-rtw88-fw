/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_BEACON_H
#define RTL8811CU_BEACON_H
#include "../interrupts/foundation.h"

#define BEACON_MAGIC 0x314e4342u /* BCN1 */
enum beacon_state { BEACON_OFF, BEACON_ARMED, BEACON_SYNCED, BEACON_FAILED };
enum beacon_error { BEACON_OK, BEACON_READBACK, BEACON_REPORT, BEACON_CLOCK, BEACON_GAP };

struct beacon_event {
    uint32_t psr, aid, dtim_count, dtim_period, interval_tu;
    /* Current associated-port TSF and local foreground observation time.
     * Neither is represented as a hardware RX timestamp.
     */
    uint32_t tsf_low, local_time_us, spacing_us, tsf_delta;
    uint32_t bssid_lo, bssid_hi, missed_interval;
};

struct beacon_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t media_commands, arms, cancels, context_changes, readback_errors;
    uint32_t events, valid_events, invalid_reports, raced_events, stale_clears;
    uint32_t tim_unicast, dtim_multicast, dtim_beacons, missing_intervals, clock_resets;
    uint32_t polls, bssid_lo, bssid_hi, aid, interval_tu;
    uint32_t last_psr, last_tsf, last_local_us, last_spacing_us, last_tsf_delta;
    uint32_t dtim_count, dtim_period, control, rcr, last_pending;
    uint32_t owned, last_media, tsf_valid, context_valid, no_event_polls;
};
extern volatile struct beacon_status g_beacon;
void station_beacon_init(void);
/* The accepted port-0 H2C media event is the association-lifecycle owner. */
void station_beacon_media(uint32_t connected);
void station_beacon_cancel(void);
/* Call only while the radio is awake. Own RF doze must skip this function;
 * the same CR/BB-reset patterns otherwise mean a host shutdown.
 * Return 1 only for a new, bounded, hardware-latched associated beacon report.
 * This function never unmasks an interrupt or reads the host RX DMA ring.
 */
uint32_t station_beacon_poll(struct beacon_event *event);
#endif
