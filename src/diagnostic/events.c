/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"

volatile struct diag_status g_diag = {
    .magic = DIAG_MAGIC, .abi_version = DIAG_ABI_VERSION,
    .struct_bytes = sizeof(struct diag_status), .fw_version = DIAG_FW_VERSION,
    .ring_capacity = DIAG_RING_CAPACITY
};
volatile struct diag_event g_events[DIAG_RING_CAPACITY];

/* One producer: the cooperative main loop. Publish payload before sequence,
 * then head. A reader checks sequence before AND after the three payload words.
 * Complement invalidation also works when the 32-bit sequence wraps to zero.
 * Overwrites count reused slots, not unread messages (no consumer ACK exists).
 */
void diag_emit(uint32_t event, uint32_t arg0, uint32_t arg1)
{
    uint32_t sequence = g_diag.ring_head + 1;
    volatile struct diag_event *record = &g_events[(sequence - 1) & (DIAG_RING_CAPACITY - 1)];
    record->sequence = ~sequence;
    compiler_barrier();
    record->event_id = event;
    record->arg0 = arg0;
    record->arg1 = arg1;
    compiler_barrier();
    record->sequence = sequence;
    compiler_barrier();
    if (g_diag.ring_count < DIAG_RING_CAPACITY)
        ++g_diag.ring_count;
    else
        ++g_diag.ring_overwrites;
    g_diag.ring_head = sequence;
}
