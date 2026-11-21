/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_STATION_POWER_H
#define RTL8811CU_STATION_POWER_H
#include "../interrupts/foundation.h"

#define POWER_MAGIC 0x31525750u /* PWR1: observation, not a sleep-state promise */
enum station_power_result {
    POWER_NOT_COMMAND = 0,
    POWER_ALREADY_ACTIVE = 1,
    POWER_UNSUPPORTED = 2
};
struct station_power_status {
    uint32_t magic, abi_version, struct_bytes, requests;
    uint32_t already_active, unsupported_sleep, malformed, not_awake;
    uint32_t mode, rlbm, smart_ps, awake_interval, port, requested_state;
    uint32_t last_lo, last_hi, cr, tcr, rpwm_observations, last_rpwm;
};
extern volatile struct station_power_status g_power;
/* Caller still owns/relinquishes H2C box ownership. Return 1 only means a
 * requested active state was already observed, with no transition needed.
 * Return 2 must retain unsupported/error accounting; it never denotes success.
 */
uint32_t station_power_h2c(uint32_t lo, uint32_t hi);
/* An observation hook for an independently established RPWM receive path.
 * This does not write CPWM, toggle an acknowledgement, or change any clock.
 */
void station_power_rpwm_observe(uint32_t value);
#endif
