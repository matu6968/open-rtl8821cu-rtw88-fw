/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"
#include "../compat/compat.h"
#include "../interrupts/foundation.h"
#include "../station/station.h"
#include "../deep/deep.h"

#define DBG6 0xb86010f8u
#define DBG7 0xb86010fcu

static void stack_guard_init(void)
{
    volatile uint32_t *guard = (volatile uint32_t *)(uintptr_t)DIAG_STACK_BOTTOM;
    for (uint32_t i = 0; i < 8; ++i)
        guard[i] = 0xc0de5100u + i;
}

static uint32_t stack_guard_ok(void)
{
    volatile uint32_t *guard = (volatile uint32_t *)(uintptr_t)DIAG_STACK_BOTTOM;
    for (uint32_t i = 0; i < 8; ++i)
        if (guard[i] != 0xc0de5100u + i)
            return 0;
    return 1;
}

_Noreturn void diag_main(void)
{
    stack_guard_init();
    g_diag.ring_addr = (uint32_t)(uintptr_t)g_events;
    diag_emit(EVENT_BOOT, DIAG_FW_VERSION, DIAG_ABI_VERSION);
    g_diag.abi_result = diag_abi_test();
    diag_emit(EVENT_ABI, g_diag.abi_result, DIAG_ABI_ALL);
    if (g_diag.abi_result != DIAG_ABI_ALL) {
        g_diag.last_error = ERROR_ABI;
    } else {
        uint32_t cfg = diag_hw_read(0xb86000f0u);
        uint32_t status = diag_hw_read(0xb86000f4u);
        uint32_t functions = diag_hw_read(0xb8600068u);
        diag_decode_chip(&g_diag.chip, cfg, status, functions);
        (void)diag_set_bt_enabled(&g_diag.chip, 0);
        diag_emit(EVENT_CHIP, cfg, functions);
        diag_emit(EVENT_BT_DISABLED, g_diag.chip.bt_func_flag, DIAG_UNKNOWN);
    }
    g_diag.stack_ok = stack_guard_ok();
    if (!g_diag.stack_ok)
        g_diag.last_error = ERROR_STACK;
    g_diag.state = g_diag.last_error ? DIAG_FAILED : DIAG_RUNNING;
    if (g_diag.last_error)
        diag_emit(EVENT_ERROR, g_diag.last_error, g_diag.abi_result);
    if (g_diag.state == DIAG_RUNNING)
        foundation_init();
    if (g_foundation.state != 1) {
        g_diag.last_error = ERROR_FOUNDATION;
        g_diag.state = DIAG_FAILED;
    }
    if (g_diag.state == DIAG_RUNNING)
        station_init();
    if (g_diag.state == DIAG_RUNNING)
        compat_init();
    diag_hw_write(DBG6, 0);
    diag_hw_write(DBG7, 0x11111111u);
    for (;;) {
        if (!deep_pre()) continue;
        ++g_diag.progress;
        station_poll();
        diag_mailbox_poll();
        compat_poll();
        foundation_poll();
        /* The interrupt foundation uses the qualified microsecond counter. */
        if (foundation_heartbeat_due()) {
            g_diag.stack_ok = stack_guard_ok();
            if (!g_diag.stack_ok && g_diag.last_error != ERROR_STACK) {
                g_diag.last_error = ERROR_STACK;
                g_diag.state = DIAG_FAILED;
                diag_emit(EVENT_ERROR, ERROR_STACK, g_diag.progress);
            }
            diag_emit(EVENT_HEARTBEAT, g_diag.progress, 0);
        }
    }
}
