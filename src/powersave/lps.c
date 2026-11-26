/* SPDX-License-Identifier: GPL-2.0-only */
#include "lps.h"
#include "../ap/ap.h"

/* Port-0 ordinary LPS implements the AP-side power-save handshake.
 * AP_PS denotes an acknowledged PM transition. The deep runtime owns
 * RPWM, CPWM, RF and CPU clock changes.
 */
#define CR             0xb8640100u
#define CPU_MGQ_INFO   0xb864041cu
#define TXPKT_EMPTY    0xb864041au
#define BCNQ_BDNY      0xb8640424u
#define MGQ_BDNY       0xb864047au
#define NULL_STATUS    0xb86404e0u
#define BCNTCFG        0xb8640510u
#define PIFS           0xb8640512u
#define TXPAUSE        0xb8640522u
#define TCR            0xb8640604u
#define FT1ISR         0xb864113cu
#define TX_SRAM        0xb8780000u
#define NULL_EVENTS    0x6000u
#define CPU_MGQ_POLL   0x20000000u
#define TRANSITION_US  75000u
#define NULL_US        20000u
#define QUEUE_US       15000u

static uint32_t operation_deadline, phase_deadline, attempts;
static uint32_t saved_pause, saved_bcntcfg, saved_pifs, saved_boundary;
static uint32_t saved_tcr6, owned, submitted;
volatile struct lps_status g_lps = {
    .magic = LPS_MAGIC, .abi_version = 1, .struct_bytes = sizeof(struct lps_status)
};

static uint32_t expired(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void fail(uint32_t error)
{
    g_lps.error = error;
    g_lps.state = LPS_FAILED;
    if (error == LPS_READBACK) ++g_lps.readback_errors;
}

static uint32_t radio_running(void)
{
    uint32_t cr = diag_hw_read(CR);
    g_lps.last_cr = cr;
    return (cr & 255u) != 0xeau && (cr & 0xc0u) == 0xc0u &&
           (compat_read8(0xb8600002u) & 3u) == 3u;
}

/* Release only our AC0..3 pause bits. Other TXPAUSE bits may have changed
 * independently. The caller must not start another CPU-MGQ transaction until
 * this owner has consumed its completion and restored these shared settings.
 */
static void release_queue(void)
{
    if (!owned) return;
    compat_write16(MGQ_BDNY, saved_boundary);
    compat_write16(BCNTCFG, saved_bcntcfg);
    compat_write8(PIFS, saved_pifs);
    compat_write8(TXPAUSE, (compat_read8(TXPAUSE) & ~15u) | (saved_pause & 15u));
    owned = 0;
}

static uint32_t tag_pm(uint32_t pm)
{
    /* Stock 80034608: port 0, action1 sets24/6/7; action0 clears24/7.
     * Bit6 is cleared before NULL submission so its own FC controls PM.
     * Change bit7 only after successful hardware NULL status, never just
     * because the CPU-MGQ submit poll cleared.
     */
    uint32_t tcr = diag_hw_read(TCR);
    diag_hw_write(TCR, pm ? tcr | 0x01000000u : tcr & ~0x01000000u);
    uint32_t low = compat_read8(TCR);
    compat_write8(TCR, pm ? low | 0xc0u : low & ~0x80u);
    g_lps.last_tcr = diag_hw_read(TCR);
    return (g_lps.last_tcr & 0x01000080u) == (pm ? 0x01000080u : 0);
}

static void queue_phase(uint32_t pm, uint32_t now)
{
    g_lps.tx_pm = pm;
    phase_deadline = now + QUEUE_US;
    g_lps.deadline = operation_deadline;
    g_lps.state = LPS_WAIT_QUEUE;
}

/* Validate a host-downloaded, ordinary, unprotected ToDS NULL template.
 * TX SRAM is 64 KiB on RTL8821C; a reserved page is 128 bytes. Do not let a
 * malformed H2C page escape this aperture, or turn another frame into NULL.
 * SPE_RPT is forbidden: these frames are completed through NULL0/1 status,
 * and must not create an unmatched host CCX report.
 */
static uint32_t null_template(void)
{
    if (!g_lps.page_valid) return 0;
    uint32_t page = (compat_read16(BCNQ_BDNY) & 0xfffu) + g_lps.null_page;
    if (page >= 512u) return 0;
    uint32_t address = TX_SRAM + (page << 7);
    uint32_t desc0 = diag_hw_read(address);
    uint32_t fc = compat_read16(address + 48);
    if ((desc0 & 65535u) != 24u || ((desc0 >> 16) & 255u) != 48u ||
        (fc & ~0x1800u) != 0x0148u ||
        (diag_hw_read(address + 8) & 0x80000u) ||
        (diag_hw_read(address + 20) & 0x00fc0000u)) return 0;
    g_lps.tx_address = address;
    return 1;
}

static uint32_t submit_null(uint32_t now)
{
    uint32_t mgq = diag_hw_read(CPU_MGQ_INFO);
    /* Stock serializes this shared CPU queue before editing its head. Here
     * we decline while occupied; no busy queue is flushed or repointed.
     * Check the AC queues before pausing them so queued traffic can drain.
     */
    if (mgq & 0x3000f000u || (compat_read16(TXPKT_EMPTY) & 0x10fu) != 0x10fu)
        return 0;
    if (!null_template()) {
        fail(g_lps.page_valid ? LPS_BAD_TEMPLATE : LPS_NO_TEMPLATE);
        return 0;
    }
    uint32_t address = g_lps.tx_address, pm = g_lps.tx_pm;
    saved_pause = compat_read8(TXPAUSE);
    saved_bcntcfg = compat_read16(BCNTCFG);
    saved_pifs = compat_read8(PIFS);
    saved_boundary = compat_read16(MGQ_BDNY);
    saved_tcr6 = compat_read8(TCR) & 0x40u;
    owned = 1;
    compat_write8(TXPAUSE, saved_pause | 15u);
    /* W1C only our two events. Never enable or clear another MCU source. */
    uint32_t pending = diag_hw_read(FT1ISR) & NULL_EVENTS;
    if (pending) ++g_lps.stale_events;
    diag_hw_write(FT1ISR, NULL_EVENTS);
    if (diag_hw_read(FT1ISR) & NULL_EVENTS) {
        release_queue(); fail(LPS_READBACK); return 0;
    }
    uint32_t page = (address - TX_SRAM) >> 7;
    compat_write16(MGQ_BDNY, page);
    /* Readable equivalent of stock8003A4F8 plus80034914 (port0).
     * Keep the host's data rate/security/header addresses; set a bounded
     * retry limit8, hardware sequence, PM FC and the appropriate NULL flag.
     */
    compat_write16(address + 32, (compat_read16(address + 32) | 0x8000u) & 0xc0c0u);
    uint32_t rate = diag_hw_read(address + 16);
    rate &= compat_read8(address + 3) & 1u ? 0xffffe0ffu : 0xffff00ffu;
    rate = (rate & 0xff01ffffu) | 0x00220000u;
    diag_hw_write(address + 16, rate);
    compat_write8(address + 5, compat_read8(address + 5) & 0x7fu);
    compat_write8(PIFS, 0x1c);
    compat_write16(BCNTCFG, 0x4413);
    uint32_t fc1 = compat_read8(address + 49) & ~0x18u;
    compat_write8(address + 49, fc1 | (pm ? 0x10u : 0));
    uint32_t nulls = (compat_read8(address + 9) & 0x3fu) | (pm ? 0x80u : 0x40u);
    compat_write8(address + 9, nulls);
    compat_write8(TCR, compat_read8(TCR) & ~0x40u);
    if (compat_read16(MGQ_BDNY) != page || diag_hw_read(address + 16) != rate ||
        compat_read8(address + 9) != nulls ||
        (compat_read8(address + 49) & 0x18u) != (pm ? 0x10u : 0) ||
        compat_read8(TCR) & 0x40u) {
        compat_write8(TCR, (compat_read8(TCR) & ~0x40u) | saved_tcr6);
        release_queue(); fail(LPS_READBACK); return 0;
    }
    compiler_barrier();
    /* Stock800367A4, HALMAC BIT_CPUMGT_POLL. This requests real MAC TX;
     * submit-poll clear alone says nothing about AP acknowledgement.
     */
    diag_hw_write(CPU_MGQ_INFO, diag_hw_read(CPU_MGQ_INFO) | CPU_MGQ_POLL);
    submitted = 1;
    ++attempts;
    if (pm) ++g_lps.null1_submissions; else ++g_lps.null0_submissions;
    phase_deadline = now + NULL_US;
    g_lps.state = LPS_WAIT_ACK;
    return 1;
}

void station_lps_init(void)
{
    operation_deadline = phase_deadline = attempts = 0;
    owned = submitted = 0;
    g_lps.state = LPS_ACTIVE;
    g_lps.error = LPS_OK;
    g_lps.page_valid = g_lps.ap_ps = 0;
}

uint32_t station_lps_h2c(uint32_t lo, uint32_t hi)
{
    uint32_t id = lo & 255u;
    if (id != 0 && id != 0x20u) return 0;
    ++g_lps.commands;
    g_lps.last_lo = lo; g_lps.last_hi = hi;
    if (id == 0) {
        if (hi & 0xffffff00u || g_lps.state == LPS_WAIT_QUEUE ||
            g_lps.state == LPS_WAIT_ACK || g_lps.ap_ps) goto rejected;
        uint32_t null_page = lo >> 24;
        if (!null_page || null_page == 255u) goto rejected;
        g_lps.null_page = null_page;
        g_lps.qos_null_page = hi & 255u;
        g_lps.page_valid = 1;
        ++g_lps.reserved_updates;
        return 1;
    }
    /* Awake-only interfaces use the idempotent station_power_h2c handler. */
    if (!diag_uses_usb()) return 0;
    uint32_t mode = (lo >> 8) & 127u, rlbm = (lo >> 16) & 15u;
    uint32_t smart = (lo >> 20) & 15u, interval = lo >> 24;
    uint32_t port = (hi >> 5) & 7u, state = (hi >> 8) & 255u;
    /* Only the captured rtw88 port0 ordinary contract is accepted. UAPSD,
     * coexistence, multiple ports and vendor DTIM/awake variants need their
     * own state machines. Register H2C delivery is not sleep completion.
     */
    if (lo & 0x8000u || hi & ~0x0000ffe0u || port || interval != 1u ||
        !((mode == 0 && !rlbm && !smart && state == 12u) ||
          (mode == 1u && rlbm == 1u && smart == 2u && !state))) goto rejected;
    if (!radio_running()) goto rejected;
    if (g_lps.state == LPS_FAILED && owned && submitted) goto rejected;
    if (mode && !null_template()) goto rejected;
    g_lps.requested_mode = mode; g_lps.rlbm = rlbm;
    g_lps.smart_ps = smart; g_lps.awake_interval = interval; g_lps.port = port;
    g_lps.error = LPS_OK;
    uint32_t now = foundation_now_us();
    if (g_lps.state == LPS_WAIT_ACK || g_lps.state == LPS_WAIT_QUEUE) {
        /* Do not edit an in-flight null. Finish it and then honor the newest
         * command. An ACTIVE request therefore cannot be lost during entry.
         */
        if (g_lps.state == LPS_WAIT_QUEUE) g_lps.tx_pm = mode;
        operation_deadline = now + TRANSITION_US;
        g_lps.deadline = operation_deadline;
        return 1;
    }
    g_lps.last_tcr = diag_hw_read(TCR);
    if (!mode && !g_lps.ap_ps && !(g_lps.last_tcr & 0x80u)) {
        g_lps.state = LPS_ACTIVE; ++g_lps.already_active; return 1;
    }
    if (mode && g_lps.ap_ps == 1u && g_lps.last_tcr & 0x80u) return 1;
    if (!null_template()) goto rejected;
    attempts = 0;
    operation_deadline = now + TRANSITION_US;
    queue_phase(mode, now);
    return 1;
rejected:
    ++g_lps.rejected;
    return 2;
}

void station_lps_poll(void)
{
    /* BB reset precedes CR shutdown during host power-off. Discard this
     * generation even in AP_PS or ACTIVE, before touching stale TX SRAM.
     * A host shutdown is a lifecycle event, not a failed PM-null exchange.
     */
    if (!radio_running()) {
        if (g_lps.page_valid || g_lps.ap_ps || owned || g_lps.state != LPS_ACTIVE)
            station_lps_cancel();
        return;
    }
    if ((g_lps.page_valid || g_lps.ap_ps || owned || g_lps.state != LPS_ACTIVE) &&
        ap_port_active()) {
        /* Let an already submitted null retire before releasing the shared
         * queue. AP mode never starts another station PM exchange.
         */
        if (owned && submitted) {
            if (diag_hw_read(CPU_MGQ_INFO) & CPU_MGQ_POLL) return;
            diag_hw_write(FT1ISR, diag_hw_read(FT1ISR) & NULL_EVENTS);
        }
        if (g_lps.ap_ps && (diag_hw_read(TCR) & 0x80u)) (void)tag_pm(0);
        release_queue();
        station_lps_cancel();
        return;
    }
    if (g_lps.state == LPS_FAILED && owned && submitted) {
        /* A timeout cannot revoke hardware ownership. Reclaim shared queue
         * settings only after the outstanding submit really completes. Keep
         * AP state unknown and require an explicit real PM0 recovery.
         */
        if (!(diag_hw_read(CPU_MGQ_INFO) & CPU_MGQ_POLL)) {
            diag_hw_write(FT1ISR, diag_hw_read(FT1ISR) & NULL_EVENTS);
            submitted = 0;
            release_queue();
        }
        return;
    }
    if (g_lps.state != LPS_WAIT_QUEUE && g_lps.state != LPS_WAIT_ACK) return;
    uint32_t now = foundation_now_us();
    if (g_lps.state == LPS_WAIT_QUEUE) {
        if (expired(now, operation_deadline) || expired(now, phase_deadline)) {
            ++g_lps.queue_timeouts; fail(LPS_QUEUE_TIMEOUT); return;
        }
        submit_null(now);
        return;
    }
    uint32_t isr = diag_hw_read(FT1ISR) & NULL_EVENTS;
    g_lps.last_isr = isr;
    uint32_t expected = g_lps.tx_pm ? 0x4000u : 0x2000u;
    if (isr & ~expected) {
        ++g_lps.stale_events; diag_hw_write(FT1ISR, isr & ~expected);
    }
    if ((isr & expected) && !(diag_hw_read(CPU_MGQ_INFO) & CPU_MGQ_POLL)) {
        g_lps.last_null_status = compat_read8(NULL_STATUS);
        uint32_t acked = (g_lps.last_null_status >> g_lps.tx_pm) & 1u;
        uint32_t ack_observed_us = foundation_now_us();
        diag_hw_write(FT1ISR, expected);
        submitted = 0;
        if (acked) {
            g_lps.ap_ps = g_lps.tx_pm;
            if (g_lps.tx_pm) ++g_lps.null1_acks; else ++g_lps.null0_acks;
            uint32_t tagged = tag_pm(g_lps.tx_pm);
            release_queue();
            if (!tagged) { fail(LPS_READBACK); return; }
            if (!g_lps.tx_pm) diag_emit(52,ack_observed_us,g_lps.null0_acks);
            if (g_lps.requested_mode == g_lps.tx_pm) {
                g_lps.state = g_lps.tx_pm ? LPS_AP_PS : LPS_ACTIVE;
                g_lps.error = LPS_OK;
            } else queue_phase(g_lps.requested_mode, now);
            return;
        }
        ++g_lps.null_failures;
        g_lps.error = LPS_NULL_FAILED;
    } else if (!expired(now, phase_deadline) && !expired(now, operation_deadline)) return;
    else {
        ++g_lps.null_timeouts; g_lps.error = LPS_NULL_TIMEOUT;
        /* If the CPU queue still owns the descriptor, leave all shared
         * settings untouched. Repurposing an in-flight head is unsafe.
         */
        if (diag_hw_read(CPU_MGQ_INFO) & CPU_MGQ_POLL) {
            g_lps.ap_ps = 2; fail(LPS_NULL_TIMEOUT); return;
        }
        submitted = 0;
    }
    release_queue();
    /* AP might have received a null even when its ACK was lost. Recover
     * through a real PM0 exchange; never clear TCR to manufacture a wake.
     */
    g_lps.ap_ps = 2;
    g_lps.requested_mode = 0;
    if (attempts >= 3u || expired(now, operation_deadline)) {
        g_lps.state = LPS_FAILED; return;
    }
    ++g_lps.recoveries;
    queue_phase(0, now);
}

void station_lps_beacon(uint32_t psr)
{
    ++g_lps.beacon_events;
    if (psr & 0x4000u) ++g_lps.tim_unicast;
    if (psr & 0x8000u) ++g_lps.dtim_multicast;
    if (!(psr & 0xc000u) || g_lps.state != LPS_AP_PS) return;
    ++g_lps.tim_wakes;
    g_lps.requested_mode = 0;
    attempts = 0;
    uint32_t now = foundation_now_us();
    operation_deadline = now + TRANSITION_US;
    queue_phase(0, now);
}

void station_lps_cancel(void)
{
    ++g_lps.cancelled;
    owned = submitted = 0;
    g_lps.page_valid = g_lps.ap_ps = 0;
    g_lps.requested_mode = 0;
    g_lps.state = LPS_ACTIVE;
    g_lps.error = LPS_OK;
}

/* Policy recovery after a missed beacon is not a fabricated TIM event. */
void station_lps_request_active(void)
{
    if (g_lps.state != LPS_AP_PS) return;
    g_lps.requested_mode = 0;
    attempts = 0;
    uint32_t now = foundation_now_us();
    operation_deadline = now + TRANSITION_US;
    queue_phase(0, now);
}
