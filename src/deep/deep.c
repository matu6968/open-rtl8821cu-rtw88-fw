/* SPDX-License-Identifier: GPL-2.0-only */
#include "deep.h"
#include "clock.h"
#include "../doze/radio.h"
#include "../doze/doze.h"
#include "../powersave/lps.h"
#include "../wowlan/wowlan.h"
#define FT1IMR 0xb8641138u
#define FT1ISR 0xb864113cu
#define RPWM 0xb8600083u
#define CPWM 0xb864012fu
volatile struct deep_status g_deep={
    .magic=DEEP_MAGIC,.abi_version=1,.struct_bytes=sizeof(struct deep_status)
};
static uint32_t tim_at_commit, cycles_at_commit, beacon_at_wake;
static uint32_t missed_at_gate, late_at_gate, beacon_result;

uint32_t deep_hold_radio_awake(void)
{
    return g_deep.mode == 5u && g_deep.state == 4u && g_deep.requested &&
           g_deep.associated_cycles != cycles_at_commit;
}
_Static_assert(sizeof(struct deep_status)==192,"DPS1 ABI");
static void service_request(void);
uint32_t deep_now_us(uint32_t raw) {return raw+g_deep.time_offset;}
static void acknowledge(uint32_t status)
{
    if (!(diag_hw_read(0xb86010f8u)&1u))
        diag_hw_write(0xb86010fcu,0x44000000u|(g_deep.cookie<<8)|status);
}
void deep_init(void)
{
    clock_init();
    uint32_t saved=core_irq_save();
    g_deep.saved_ft1imr=foundation_read32(FT1IMR);
    g_deep.last_toggle=compat_read8(RPWM)&0x80u;
    foundation_write32(FT1ISR,1u);
    foundation_write32(FT1IMR,g_deep.saved_ft1imr|1u);
    core_irq_restore(saved);
}
uint32_t deep_rpwm_irq(struct core_frame *frame)
{
    uint32_t handled=clock_timer_irq();
    uint32_t pending=foundation_read32(FT1ISR);
    if (!(pending&1u)) return handled;
    ++g_deep.irq_events;g_deep.last_isr=pending;
    g_deep.last_cause=frame->cause;g_deep.last_status=frame->status;g_deep.last_epc=frame->epc;
    uint32_t first=0,second=1;
    for (uint32_t i=0;i<4u && first!=second;++i) {
        first=compat_read8(RPWM);second=compat_read8(RPWM);
    }
    foundation_write32(FT1ISR,1u);
    if (first!=second) {++g_deep.unstable;return 1;}
    if (g_deep.pending) ++g_deep.overruns;
    g_deep.last_raw=first;compiler_barrier();g_deep.pending=1;
    service_request();
    return 1;
}
static void service_request(void)
{
    uint32_t saved=core_irq_save(),raw=g_deep.last_raw,pending=g_deep.pending;
    g_deep.pending=0;core_irq_restore(saved);
    if (!pending) return;
    if (raw&~0xc1u) {g_deep.error=3;return;}
    if ((raw&0x80u)==g_deep.last_toggle) {++g_deep.duplicates;return;}
    g_deep.last_toggle=raw&0x80u;g_deep.requested=raw&1u;++g_deep.accepted;
    if (raw&0x40u) {
        /* Stock acceptance toggles only CPWM bit7, before clock entry. */
        uint32_t value=(compat_read8(CPWM)^0x80u)&0x80u;
        compat_write8(CPWM,value);g_deep.last_cpwm=compat_read8(CPWM);
        if (g_deep.last_cpwm!=value) g_deep.error=1;
        ++g_deep.cpwm_acks;
    }
}
uint32_t deep_h2c(uint32_t lo,uint32_t hi)
{
    uint32_t id=lo&255u;
    if (id!=0xf1u && id!=0xf2u) return 0;
    ++g_deep.commands;g_deep.last_lo=lo;g_deep.last_hi=hi;
    uint32_t mode=(lo>>8)&255u,cookie=lo>>16;
    if (id==0xf2u) {
        /* One-way commit after ACTIVE/LCLK request acceptance and the
         * host's awake bridge flush. No DBG ACK read is safe after commit.
         */
        if (hi!=DEEP_MAGIC || mode || !cookie || cookie!=g_deep.cookie ||
            g_deep.state!=1u || g_deep.mode<2u || !g_deep.requested ||
            clock_owned() || station_wowlan_owned() || radio_owned()) {
            ++g_deep.rejected_commits;return 2;
        }
        tim_at_commit = g_lps.tim_wakes;
        cycles_at_commit = g_deep.associated_cycles;
        beacon_result = 0;
        ++g_deep.commits;g_deep.state=4;return 1;
    }
    if (hi!=DEEP_MAGIC || mode>5u || !cookie || clock_owned() ||
        station_wowlan_owned() || radio_owned() ||
        (mode>=2u && mode<=4u && g_station.iqk_completed) ||
        (mode==5u && (!g_station.iqk_completed || !g_doze.connected)) ||
        (mode>=2u && (g_deep.requested || (compat_read8(RPWM)&1u))) ||
        (diag_hw_read(0xb86010f8u)&1u)) {
        ++g_deep.rejected;return 2;
    }
    g_deep.cookie=cookie;g_deep.mode=mode;g_deep.state=mode?1:0;
    g_deep.error=0;acknowledge(mode?1:0);return 1;
}
uint32_t deep_pre(void)
{
    if (clock_owned()) {
        /* CPU IE is disabled and only always-on registers are touched while
         * clock ownership is active. The host timer independently requests
         * wake; a failed engine remains owned until relay recovery.
         */
        if (!(compat_read8(RPWM)&1u)) clock_wake();
        clock_poll();
        if (g_clock.error==CLOCK_RETENTION && !(compat_read8(0xb8600008u)&0x10u))
            diag_mailbox_poll(); /* clocks restored, normal producers held */
        if (!clock_awake()) return 0;
        ++g_deep.clock_cycles;++g_deep.timer_epochs;
        if (g_deep.mode==5u) {
            ++g_deep.associated_cycles;
            uint32_t elapsed=g_clock.raw_after-g_clock.raw_before;
            uint32_t counted=g_clock.free_after-g_clock.free_before;
            beacon_at_wake = g_doze.fresh_beacons;
            /* The free-running counter slows with TSF in this USB profile.
             * Repair the local beacon deadline epoch once on return. */
            if (elapsed <= 200000u / 6u)
                elapsed = (elapsed << 2) + (elapsed << 1);
            else
                elapsed = 200001u;
            g_deep.last_elapsed_us=elapsed;g_deep.last_missing_us=0;
            if (elapsed<=200000u && elapsed>counted+128u) {
                uint32_t missing=elapsed-counted;
                g_deep.time_offset+=missing;g_deep.compensated_us+=missing;
                g_deep.last_missing_us=missing;++g_deep.epoch_corrections;
            } else if (elapsed>200000u || counted>elapsed+2000u) {
                g_deep.error=4;g_deep.mode=0;
                radio_wake(RADIO_WAKE_FAILURE);
            }
            /* Hand the retained RF transaction to its restoration path
             * before ordinary station work. TXPAUSE remains owned through
             * the complete RF and cached-IQK readback transaction.
             */
            g_deep.rf_pre_cr=diag_hw_read(0xb8640100u);
            g_deep.rf_pre_sys=compat_read8(0xb8600002u);
            if (radio_owned() && radio_asleep())
                radio_wake((g_clock.rpwm_at_wake&1u)?RADIO_WAKE_TBTT:RADIO_WAKE_H2C);
            g_deep.rf_resume_state=g_radio.state;
            g_deep.rf_resume_error=g_radio.error;
        }
        uint32_t now=foundation_now_us();
        if (now<g_foundation.time_low) ++g_foundation.time_high;
        g_foundation.time_low=now;
        if (foundation_timer_start(TIMER_PERIOD_US)) ++g_deep.timer_failures;
        g_deep.wake_progress=g_diag.progress;g_deep.state=4;
        core_irq_restore(g_deep.saved_irq);
    }
    service_request();
    if (g_deep.mode==5u && !g_deep.requested && radio_owned())
        radio_wake(RADIO_WAKE_H2C);
    if (g_deep.mode == 5u && g_deep.state == 4u && g_deep.requested && radio_awake() &&
        !(diag_hw_read(0xb86010f8u) & 1u)) {
        /* Only a valid beacon advances the foreground counter. A real TIM
         * or DTIM takes precedence over an ordinary post-wake beacon. */
        if (deep_hold_radio_awake() && !beacon_result) {
            if (g_doze.missed_beacons != missed_at_gate || g_doze.late_wakes != late_at_gate)
                beacon_result = 0xd505c000u;
            else if (g_doze.fresh_beacons != beacon_at_wake)
                beacon_result = 0xd5054000u;
        }
        if (beacon_result == 0xd505c000u)
            diag_hw_write(0xb86010fcu, beacon_result);
        else if (g_lps.tim_wakes != tim_at_commit)
            diag_hw_write(0xb86010fcu, 0xd5058000u);
        else if (beacon_result)
            diag_hw_write(0xb86010fcu, beacon_result);
    }
    if (g_deep.mode>=2u && g_deep.state==4u && g_deep.requested &&
        !(foundation_read32(FT1ISR)&1u)) {
        uint32_t generation=g_deep.accepted;
        if (g_deep.mode==5u) {
            if (!radio_owned() || !radio_asleep() || g_lps.state!=LPS_AP_PS || g_lps.ap_ps!=1u ||
                !g_doze.connected || g_doze.sync_streak<2u ||
                (compat_read8(0xb86401ccu)&15u)) return 1;
            generation=g_radio.enters;
            uint32_t now=foundation_now_us(),lead=g_radio.wake_at-now;
            if (lead<7000u || lead>200000u) {
                ++g_deep.scheduling_declines;return 1;
            }
            g_clock.wake_at_us=g_radio.wake_at-6000u;
            missed_at_gate = g_doze.missed_beacons;
            late_at_gate = g_doze.late_wakes;
        }
        if (g_deep.entry_request==generation) return 1;
        g_deep.entry_request=generation;
        g_deep.saved_irq=core_irq_save();foundation_timer_stop();
        g_deep.entry_progress=g_diag.progress;
        if (clock_begin(g_deep.mode)) {g_deep.state=2;return 0;}
        if (g_deep.mode!=5u) {g_deep.error=2;g_deep.state=3;}
        else ++g_deep.scheduling_declines;
        (void)foundation_timer_start(TIMER_PERIOD_US);core_irq_restore(g_deep.saved_irq);
    }
    return 1;
}
