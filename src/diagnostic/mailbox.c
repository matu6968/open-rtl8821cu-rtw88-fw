/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"

#define DBG6 0xb86010f8u
#define DBG7 0xb86010fcu

/* Same request/response words as dump_bootrom.read_word. Only the diagnostic
 * data arena is readable in this image; arbitrary peripheral loads can hang
 * this core. Invalid requests are acknowledged with DIAG_BAD_ADDRESS.
 */
void diag_mailbox_poll(void)
{
    uint32_t request = diag_hw_read(DBG6);
    uint32_t address, value;
    if (!(request & 1))
        return;
    address = request & ~1u;
    if (request < 0x80000000u) {
        value = request; /* existing low-address echo protocol */
    } else if (!(address & 3) && address >= DIAG_READ_START && address < DIAG_READ_END) {
        value = diag_hw_read(address);
        ++g_diag.mailbox_reads;
    } else {
        value = DIAG_BAD_ADDRESS;
        ++g_diag.invalid_requests;
    }
    diag_hw_write(DBG7, value);
    diag_hw_write(DBG6, address);
}
