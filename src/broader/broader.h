/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_BROADER_H
#define RTL8811CU_BROADER_H
#include "../station/station.h"
#define BROADER_MAGIC 0x31445242u /* BRD1 */
struct broader_status {
    uint32_t magic, abi_version, struct_bytes, state;
    uint32_t register_commands, register_handled, register_unsupported;
    uint32_t last_lo, last_hi;
    uint32_t iqk_rf18, iqk_adc_clock, iqk_frontend;
};
extern volatile struct broader_status g_broader;
void broader_init(void);
void broader_poll(void);
uint32_t broader_register_h2c(uint32_t lo, uint32_t hi);
/* Complete C2H frame, including its ID/sequence bytes; nonblocking. */
uint32_t station_c2h_send(const uint8_t *frame, uint32_t bytes);
uint32_t station_transport_active(void);
uint32_t station_transport_ready(void);
#endif
