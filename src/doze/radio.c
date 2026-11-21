/* SPDX-License-Identifier: GPL-2.0-only */
#include "radio.h"

/* Readable ordinary RF doze/wake from stock8003B670/8003B724. CPU, timer A,
 * interrupt masks and clocks stay running. No RPWM/CPWM or LCLK transition.
 * The caller owns beacon/TIM scheduling and acknowledged AP PM signalling.
 */
#define SYS_FUNC       0xb8600002u
#define LED_CFG        0xb860004cu
#define SDIO_CTRL      0xb8600070u
#define CR             0xb8640100u
#define TCR            0xb8640604u
#define TXPAUSE        0xb8640522u
#define STATE_MON      0xb86406b4u
#define BB_C00         0xb8640c00u
#define BB_8B4         0xb86408b4u
#define RF0            0xb8642800u
#define RF1            0xb8642804u
#define RF18           0xb8642860u
#define RF_WRITE       0xb8640c90u
#define RF_MASK        0xfffffu
#define RF_SETTLE_US   13u
#define WAKE_LIMIT_US  4000u
#define PHASE_POLLS    2048u
static uint32_t saved_valid, phase_polls, wake_started, sleep_counted;
static uint32_t pending_wake, last_time;
volatile struct radio_status g_radio = {
    .magic=RADIO_MAGIC,.abi_version=1,.struct_bytes=sizeof(struct radio_status)
};

static uint32_t due(uint32_t now,uint32_t deadline)
{
    return (int32_t)(now-deadline)>=0;
}
uint32_t radio_owned(void)
{
    return saved_valid;
}
uint32_t radio_asleep(void)
{
    return g_radio.state==RADIO_ASLEEP;
}
uint32_t radio_awake(void)
{
    return !saved_valid && (g_radio.state==RADIO_AWAKE || g_radio.state==RADIO_FAILED_AWAKE);
}
static void error(uint32_t why)
{
    if (!g_radio.error) g_radio.error=why;
    if (why==RADIO_READBACK) ++g_radio.readback_errors;
    else if (why==RADIO_IQK_CACHE) ++g_radio.iqk_errors;
    else ++g_radio.timeouts;
}
static uint32_t lifecycle(void)
{
    uint32_t cr=diag_hw_read(CR),sys=compat_read8(SYS_FUNC);
    /* Preserve the failing restoration sample while the retained owner
     * continues checking for a later host shutdown.
     */
    if (!g_radio.restore_failure_mask) {
        g_radio.last_cr=cr;g_radio.last_sys_func=sys;
    }
    /* An external reset clears bit1 first. Only this owner may clear bit0.
     * On any external lifecycle change forget saved MMIO without writing it.
     */
    if ((cr&255u)==0xeau || (sys&2u)!=2u ||
        (cr&0x3fu)!=(g_radio.saved_cr&0x3fu)) return 0;
    /* Bit0 and TX/RX can be partially changed by our own failed writes.
     * They are verified by each phase, not used to abandon ownership. In
     * particular FAILED_OWNED must retain an off radio until host reset.
     */
    return 1;
}
static uint32_t owned_live(void)
{
    if (!saved_valid) return 0;
    if (!lifecycle()) { radio_cancel(); return 0; }
    return 1;
}
static uint32_t write8(uint32_t address,uint32_t value)
{
    if (!owned_live()) return 0;
    compat_write8(address,value);
    return 1;
}
static uint32_t write32(uint32_t address,uint32_t value)
{
    if (!owned_live()) return 0;
    diag_hw_write(address,value);
    return 1;
}
static uint32_t masked(uint32_t address,uint32_t mask,uint32_t value)
{
    if (!owned_live()) return 0;
    uint32_t old=diag_hw_read(address);
    return write32(address,(old&~mask)|(value&mask));
}
static uint32_t quiet(void)
{
    uint32_t empty=compat_read16(0xb864041au);
    uint32_t rx=diag_hw_read(0xb8640284u);
    g_radio.last_empty=empty;g_radio.last_rxpkt=rx;
    return (empty&0x7ffu)==0x7ffu && (rx&0xff000000u)==0 &&
        (rx&0x20000u) && !(compat_read8(0xb8640296u)&1u) &&
        !(compat_read8(0xb86401ccu)&15u) && !diag_hw_read(0xb86405f8u) &&
        (diag_hw_read(0xb8640250u)&0x3ffffu)==(diag_hw_read(0xb864024cu)&0x3ffffu) &&
        (compat_read8(0xb864047cu)&127u)==(compat_read8(0xb864047du)&127u);
}
void radio_init(void)
{
    saved_valid=phase_polls=sleep_counted=pending_wake=0;
    g_radio.state=RADIO_AWAKE;g_radio.error=RADIO_OK;
    g_radio.restore_failure_mask=g_radio.restore_check_phase=g_radio.restore_checks=0;
}
uint32_t radio_sleep(uint32_t wake_at)
{
    ++g_radio.sleep_requests;
    uint32_t now=foundation_now_us(),lead=wake_at-now;
    uint32_t cr=diag_hw_read(CR),sys=compat_read8(SYS_FUNC);
    if (g_radio.state!=RADIO_AWAKE || saved_valid || g_radio.error ||
        lead<500u || lead>1000000u || (cr&255u)!=255u || (sys&3u)!=3u ||
        !(compat_read8(TCR)&0x80u) || (compat_read8(0xb86000aau)&0x80u) || !quiet()) goto decline;
    uint32_t rf=diag_hw_read(RF0)&RF_MASK;
    if ((rf&0xf0000u)!=0x30000u) goto decline;
    g_radio.saved_rf1=diag_hw_read(RF1)&RF_MASK;
    if (g_radio.saved_rf1==RF_MASK || (g_radio.saved_rf1&1u)) goto decline;
    g_radio.saved_rf0=rf;g_radio.saved_cr=cr;g_radio.saved_sys_func=sys;
    g_radio.saved_bbc00=diag_hw_read(BB_C00);g_radio.saved_bb8b4=diag_hw_read(BB_8B4);
    g_radio.saved_led=diag_hw_read(LED_CFG);g_radio.saved_sdio=diag_hw_read(SDIO_CTRL);
    g_radio.saved_pause=compat_read8(TXPAUSE);g_radio.saved_state_sel=compat_read8(STATE_MON+3);
    g_radio.last_rf18=diag_hw_read(RF18)&RF_MASK;
    if ((g_radio.saved_bbc00&3u)!=3u || g_radio.saved_bb8b4&0x40u) goto decline;
    /* Pause only after queues drained, then check again. If a host packet
     * raced the pause, leave it queued and restore; never flush that packet.
     */
    saved_valid=1;phase_polls=sleep_counted=pending_wake=0;last_time=now;
    if (!write8(TXPAUSE,255u) || !owned_live()) return 0;
    if (!quiet()) {
        if (!write8(TXPAUSE,g_radio.saved_pause) || !owned_live()) return 0;
        saved_valid=0;goto decline;
    }
    g_radio.wake_at=wake_at;
    g_radio.restore_check_phase=0;
    g_radio.rf1_post_iqk_delta=0;
    if (!masked(LED_CFG,0x01800000u,0) ||
        !masked(SDIO_CTRL,0x04000000u,0) ||
        !masked(BB_C00,3u,0) ||
        !write32(RF_WRITE,0) || !owned_live()) return 0;
    /* RF path A register0 above is a real SIPI request. */
    g_radio.phase_deadline=now+RF_SETTLE_US;
    g_radio.state=RADIO_RF_OFF_WAIT;
    return 1;
decline:
    ++g_radio.enter_declines;return 0;
}
static void count_sleep(uint32_t now)
{
    if (!sleep_counted && g_radio.state==RADIO_ASLEEP) {
        uint32_t dt=now-g_radio.sleep_started;
        g_radio.last_doze_us=dt;g_radio.accumulated_doze_us+=dt;
        if (dt>g_radio.max_doze_us) g_radio.max_doze_us=dt;
        sleep_counted=1;
    }
}
static void begin_wake(uint32_t reason)
{
    uint32_t now=foundation_now_us();count_sleep(now);wake_started=now;
    g_radio.wake_reason=reason;
    if (reason==RADIO_WAKE_TBTT) ++g_radio.timed_wakes;
    else if (reason==RADIO_WAKE_H2C || reason==RADIO_WAKE_TX) ++g_radio.demand_wakes;
    /* Keep TXPAUSE owned until RF/IQK restore. Enable original MAC blocks,
     * release only the BB reset bit we changed, then settle before SIPI.
     */
    if (!write8(CR+1,(compat_read8(CR+1)&~2u)|((g_radio.saved_cr>>8)&2u)) ||
        !write8(CR,(compat_read8(CR)&~0xc0u)|(g_radio.saved_cr&0xc0u)) ||
        !write8(STATE_MON+3,9u) || !write8(STATE_MON,0x86u) ||
        !masked(BB_8B4,0x40u,g_radio.saved_bb8b4) ||
        !write8(SYS_FUNC,(compat_read8(SYS_FUNC)&~1u)|(g_radio.saved_sys_func&1u)) ||
        !owned_live()) return;
    g_radio.phase_deadline=now+1u;g_radio.operation_deadline=now+WAKE_LIMIT_US;
    phase_polls=0;g_radio.state=RADIO_BB_ON_WAIT;
}
void radio_wake(uint32_t reason)
{
    if (!saved_valid || g_radio.state==RADIO_BB_ON_WAIT ||
        g_radio.state==RADIO_RF_ON_WAIT || g_radio.state==RADIO_RF1_ON_WAIT ||
        g_radio.state==RADIO_FAILED_OWNED) return;
    if (!lifecycle()) {radio_cancel();return;}
    /* Do not overwrite an RF SIPI operation before its settling interval.
     * A host ACTIVE command racing entry is retained and executed afterward.
     */
    if (g_radio.state==RADIO_RF_OFF_WAIT &&
        !due(foundation_now_us(),g_radio.phase_deadline)) {
        pending_wake=reason;return;
    }
    begin_wake(reason);
}
static uint32_t restored(uint32_t phase)
{
    if (!owned_live()) return 0;
    /* Capture every compared value exactly once, including registers after
     * the first mismatch. A later diagnostic read is not the failing sample.
     * The auxiliary full STATE_MON word and the compared selector byte are
     * retained separately so a mixed-width observation cannot hide a race.
     */
    uint32_t rf0=diag_hw_read(RF0)&RF_MASK;
    uint32_t rf1=diag_hw_read(RF1)&RF_MASK;
    uint32_t cr=diag_hw_read(CR),sys=compat_read8(SYS_FUNC);
    uint32_t c00=diag_hw_read(BB_C00),b4=diag_hw_read(BB_8B4);
    uint32_t state=diag_hw_read(STATE_MON),selector=compat_read8(STATE_MON+3);
    uint32_t led=diag_hw_read(LED_CFG),sdio=diag_hw_read(SDIO_CTRL);
    g_radio.last_rf0=rf0;g_radio.last_rf1=rf1;
    g_radio.last_cr=cr;g_radio.last_sys_func=sys;
    g_radio.last_bbc00=c00;g_radio.last_bb8b4=b4;
    g_radio.last_state_mon=state;g_radio.last_state_sel=selector;
    g_radio.last_led=led;g_radio.last_sdio=sdio;
    g_radio.restore_check_phase=phase;++g_radio.restore_checks;
    /* The full RF0 SIPI word was verified before C00 re-enable. Once AGC
     * resumes it legitimately changes the lower gain bits; mode3 is the
     * persistent awake condition, not a frozen gain value.
     */
    uint32_t mask=0;
    if ((rf0&0xf0000u)!=(g_radio.saved_rf0&0xf0000u)) mask|=RADIO_RESTORE_RF0;
    /* Stock 800358F0(0,1) owns RF1 bit0, not a frozen whole RF1 word.
     * C00 is already enabled here, and unowned RF1 fields can change even
     * before cache application. Retain the full observations and write
     * target, while bit0 must remain clear and reads must be valid.
     */
    if (phase==RADIO_RESTORE_POST_IQK)
        g_radio.rf1_post_iqk_delta=rf1^g_radio.rf1_written;
    if ((rf1&1u) || rf1==RF_MASK) mask|=RADIO_RESTORE_RF1;
    if ((cr&0x2c0u)!=(g_radio.saved_cr&0x2c0u)) mask|=RADIO_RESTORE_CR;
    if ((sys&3u)!=(g_radio.saved_sys_func&3u)) mask|=RADIO_RESTORE_SYS_FUNC;
    if ((c00&3u)!=(g_radio.saved_bbc00&3u)) mask|=RADIO_RESTORE_BBC00;
    if ((b4&0x40u)!=(g_radio.saved_bb8b4&0x40u)) mask|=RADIO_RESTORE_BB8B4;
    if (selector!=g_radio.saved_state_sel) mask|=RADIO_RESTORE_STATE_SEL;
    if ((led&0x01800000u)!=(g_radio.saved_led&0x01800000u)) mask|=RADIO_RESTORE_LED;
    if ((sdio&0x04000000u)!=(g_radio.saved_sdio&0x04000000u)) mask|=RADIO_RESTORE_SDIO;
    if (mask && !g_radio.restore_failure_mask) g_radio.restore_failure_mask=mask;
    return !mask;
}
void radio_poll(void)
{
    ++g_radio.polls;
    if (!saved_valid) return;
    if (!lifecycle()) {radio_cancel();return;}
    uint32_t now=foundation_now_us();
    if (now!=last_time) {last_time=now;phase_polls=0;}
    if (++phase_polls>PHASE_POLLS) {
        error(RADIO_CLOCK_STALLED);g_radio.state=RADIO_FAILED_OWNED;return;
    }
    if (g_radio.state==RADIO_ASLEEP) {
        if ((g_radio.last_sys_func&1u) || (g_radio.last_cr&0x2c0u)) {
            error(RADIO_READBACK);g_radio.state=RADIO_FAILED_OWNED;return;
        }
        if (due(now,g_radio.wake_at)) radio_wake(RADIO_WAKE_TBTT);
        else if ((compat_read8(0xb86401ccu)&15u) ||
            (diag_hw_read(0xb8640250u)&0x3ffffu)!=(diag_hw_read(0xb864024cu)&0x3ffffu)) radio_wake(RADIO_WAKE_H2C);
        else if ((compat_read16(0xb864041au)&0x7ffu)!=0x7ffu || diag_hw_read(0xb86405f8u)) radio_wake(RADIO_WAKE_TX);
        return;
    }
    if (g_radio.state==RADIO_RF_OFF_WAIT) {
        if (!due(now,g_radio.phase_deadline)) return;
        g_radio.last_rf0=diag_hw_read(RF0)&RF_MASK;
        if (g_radio.last_rf0) {error(RADIO_READBACK);radio_wake(RADIO_WAKE_FAILURE);return;}
        if (pending_wake) {uint32_t reason=pending_wake;pending_wake=0;begin_wake(reason);return;}
        if (!write8(SYS_FUNC,compat_read8(SYS_FUNC)&~1u) ||
            !masked(BB_8B4,0x40u,0x40u) ||
            !write8(CR,compat_read8(CR)&~0xc0u) ||
            !write8(CR+1,compat_read8(CR+1)&~2u) || !owned_live()) return;
        g_radio.last_cr=diag_hw_read(CR);g_radio.last_sys_func=compat_read8(SYS_FUNC);
        g_radio.last_bbc00=diag_hw_read(BB_C00);g_radio.last_bb8b4=diag_hw_read(BB_8B4);
        if ((g_radio.last_cr&0x2c0u) || (g_radio.last_sys_func&3u)!=2u ||
            (g_radio.last_bbc00&3u) || !(g_radio.last_bb8b4&0x40u)) {
            error(RADIO_READBACK);
            /* Our just-written state may have failed partially; return to
             * the pre-sleep awake setting before trusting the off guards.
             */
            /* A failed owned write is different from an external BB reset.
             * Only bit1 clear / MAC DMA shutdown forbids rollback writes.
             */
            if (!(compat_read8(SYS_FUNC)&2u) ||
                (compat_read8(CR)&0x3fu)!=(g_radio.saved_cr&0x3fu)) radio_cancel();
            else begin_wake(RADIO_WAKE_FAILURE);
            return;
        }
        g_radio.sleep_started=now;g_radio.state=RADIO_ASLEEP;++g_radio.enters;
        phase_polls=0;return;
    }
    if (g_radio.state==RADIO_BB_ON_WAIT) {
        if (!due(now,g_radio.phase_deadline)) return;
        if ((compat_read8(SYS_FUNC)&3u)!=(g_radio.saved_sys_func&3u)) {
            if (!due(now,g_radio.operation_deadline)) return;
            error(RADIO_READBACK);g_radio.state=RADIO_FAILED_OWNED;return;
        }
        if (!write32(RF_WRITE,g_radio.saved_rf0) || !owned_live()) return;
        g_radio.phase_deadline=now+RF_SETTLE_US;phase_polls=0;
        g_radio.state=RADIO_RF_ON_WAIT;return;
    }
    if (g_radio.state==RADIO_RF1_ON_WAIT) {
        if (!due(now,g_radio.phase_deadline)) return;
        g_radio.last_rf1=diag_hw_read(RF1)&RF_MASK;
        if ((g_radio.last_rf1&1u) || g_radio.last_rf1==RF_MASK) {
            if (!due(now,g_radio.operation_deadline)) return;
            error(RADIO_WAKE_TIMEOUT);g_radio.state=RADIO_FAILED_OWNED;return;
        }
    } else if (g_radio.state==RADIO_RF_ON_WAIT) {
        if (!due(now,g_radio.phase_deadline)) return;
        g_radio.last_rf0=diag_hw_read(RF0)&RF_MASK;
        if (g_radio.last_rf0!=g_radio.saved_rf0) {
            if (!due(now,g_radio.operation_deadline)) return;
            error(RADIO_WAKE_TIMEOUT);g_radio.state=RADIO_FAILED_OWNED;return;
        }
        if (!masked(BB_C00,3u,g_radio.saved_bbc00) ||
            !masked(LED_CFG,0x01800000u,g_radio.saved_led) ||
            !write8(STATE_MON+3,g_radio.saved_state_sel) || !owned_live()) return;
        /* Stock 8003BA4C(1) calls 800358F0(0,1): read RF1 live and clear
         * only bit0. Preserve hardware-managed fields from this wake,
         * rather than restoring an earlier whole-word snapshot.
         */
        if (!owned_live()) return;
        uint32_t rf1=diag_hw_read(RF1)&RF_MASK;
        if (rf1==RF_MASK) {
            error(RADIO_READBACK);g_radio.state=RADIO_FAILED_OWNED;return;
        }
        g_radio.rf1_written=rf1&~1u;
        if (!write32(RF_WRITE,0x100000u|g_radio.rf1_written) || !owned_live()) return;
        g_radio.phase_deadline=now+RF_SETTLE_US;phase_polls=0;
        g_radio.state=RADIO_RF1_ON_WAIT;return;
    } else return;
    if (!masked(SDIO_CTRL,0x04000000u,g_radio.saved_sdio) || !owned_live()) return;
    if (!restored(RADIO_RESTORE_PRE_IQK)) {
        if (saved_valid) {error(RADIO_READBACK);g_radio.state=RADIO_FAILED_OWNED;}
        return;
    }
    uint32_t start=foundation_now_us();
    if (!owned_live()) return;
    uint32_t iqk=station_iqk_doze_restore();
    g_radio.iqk_duration_us=foundation_now_us()-start;
    if (!owned_live()) return;
    if (iqk) error(RADIO_IQK_CACHE);
    if (!restored(RADIO_RESTORE_POST_IQK)) {
        if (saved_valid) {error(RADIO_READBACK);g_radio.state=RADIO_FAILED_OWNED;}
        return;
    }
    if (!write8(TXPAUSE,g_radio.saved_pause) || !owned_live()) return;
    g_radio.wake_duration_us=foundation_now_us()-wake_started;
    ++g_radio.wakes;saved_valid=0;
    g_radio.state=g_radio.error?RADIO_FAILED_AWAKE:RADIO_AWAKE;
}
void radio_cancel(void)
{
    ++g_radio.cancels;saved_valid=phase_polls=sleep_counted=pending_wake=0;
    g_radio.state=g_radio.error?RADIO_FAILED_AWAKE:RADIO_AWAKE;
}
