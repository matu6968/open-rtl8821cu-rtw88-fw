/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock.h"
#include "../doze/radio.h"
#define SYS_CLK 0xb8600008u
#define WLLPS 0xb8600090u
#define OPTIONS 0xb8600092u
#define TSF_STATE 0xb8640108u
#define CR1 0xb8640101u
#define CAL1 0xb8640199u
#define CAL3 0xb864019bu
#define OSC 0xb8640194u
#define CAL 0xb8640198u
#define BCN 0xb8640550u
#define PS_CTRL 0xb8640554u
#define PS_LO 0xb8640580u
#define PS_HI 0xb8640584u
#define TSF_LO 0xb8640560u
#define TSF_HI 0xb8640564u
#define PS_IRQ (1u<<13)
#define MACSLP 0x10u
#define TRANSITION_POLLS 100000u
#define DEEP_TIMER_PROBE_US 20000u
/* 80102014 maps this USB strap to interface1.8003D964 chooses
 * -27808=9360; its SDIO interface2 control/pad branch is inapplicable.
 */
#define DEEP_ENGINE_OPTIONS 0x9360u
volatile struct clock_status g_clock={
    .magic=CLOCK_MAGIC,.abi_version=1,.struct_bytes=sizeof(struct clock_status)
};
_Static_assert(sizeof(struct clock_status)==228,"CLK1 ABI");
void clock_init(void) {g_clock.state=CLOCK_AWAKE;}
uint32_t clock_owned(void) {return g_clock.owned;}
uint32_t clock_awake(void) {return !g_clock.owned && g_clock.state==CLOCK_AWAKE;}
void clock_wake(void) {g_clock.wake_requested=1;}
static void publish(void)
{
    /* Always-on, host-readable even when the CPU mailbox is unavailable. */
    diag_hw_write(0xb86010fcu,0xd5000000u|(g_clock.mode<<16)|
                  (g_clock.state<<8)|g_clock.error);
}
uint32_t clock_timer_irq(void)
{
    if (!g_clock.timer_irq_owned || g_clock.owned ||
        !(foundation_read32(0xb864013cu)&PS_IRQ)) return 0;
    foundation_write32(0xb864013cu,PS_IRQ);
    foundation_write32(0xb8640138u,foundation_read32(0xb8640138u)&~PS_IRQ);
    g_clock.timer_irq_owned=0;++g_clock.timer_irqs;
    return 1;
}
static void fail(uint32_t error)
{
    g_clock.error=error;g_clock.state=CLOCK_FAILED;
    publish();
    /* Do not software-clear WL_LPS_EN or continue off-domain accesses after
     * an unqualified wake. The independent USB/relay recovery owns that case.
     */
}
static uint32_t source_delay(uint32_t us)
{
    uint32_t start=foundation_now_us();
    for (uint32_t i=0;i<TRANSITION_POLLS;++i) {
        ++g_clock.source_polls;
        if ((uint32_t)(foundation_now_us()-start)>=us) return 1;
    }
    fail(CLOCK_SLEEP_TIMEOUT);return 0;
}
static void restore_sources(void)
{
    compat_write8(WLLPS,g_clock.saved_ctrl);
    compat_write8(OSC,g_clock.saved_osc);
    foundation_write32(CAL,g_clock.saved_cal);
    compat_write8(CR1,g_clock.saved_cr1);
}

static uint32_t usb_lclk_ticks(uint32_t us)
{
    /* The 5-ms and 20-ms PS0 probes on 0bda:c811 measured about six
     * microseconds of wall time per tick under the USB clock engine.
     * This experimental board profile keeps division out of MIPS16 code. */
    uint32_t result = 0;
    for (uint32_t bit = 1u << 15; bit; bit >>= 1) {
        uint32_t span = (bit << 2) + (bit << 1);
        if (us >= span) {
            result |= bit;
            us -= span;
        }
    }
    return result;
}
static uint32_t bss_hash(uint32_t bank)
{
#ifdef DIAG_HOST_TEST
    uint32_t first=0x80201000u,last=0x80202300u;
#else
    extern uint32_t __bss_start[],__bss_end[];
    uint32_t first=(uint32_t)(uintptr_t)__bss_start,last=(uint32_t)(uintptr_t)__bss_end;
#endif
    uint32_t hash=0x811c9dc5u;
    for (uint32_t a=first;a<last;a+=4u) {
        if (((a-0x80200000u)>>13)!=bank) continue;
        hash=(hash<<5)|(hash>>27);hash^=diag_hw_read(a)^a;
    }
    return hash;
}
static void ram_prepare(void)
{
    g_clock.ram_canary_low=0x198dcaf0u;g_clock.ram_lost_mask=0;
    diag_hw_write(0x80203000u,0x198dcaf1u);
    diag_hw_write(0x80204000u,0x198dcaf2u);
    diag_hw_write(0x80207000u,0x198dcaf3u);
    g_clock.radio_owner_before=radio_owned();
    g_clock.bss_before0=bss_hash(0);g_clock.bss_before1=bss_hash(1);
}
static uint32_t ram_check(void)
{
    uint32_t lost=0;
    if (g_clock.ram_canary_low!=0x198dcaf0u) lost|=1u;
    if (diag_hw_read(0x80203000u)!=0x198dcaf1u) lost|=2u;
    if (diag_hw_read(0x80204000u)!=0x198dcaf2u) lost|=4u;
    if (diag_hw_read(0x80207000u)!=0x198dcaf3u) lost|=8u;
    g_clock.radio_owner_after=radio_owned();
    g_clock.bss_after0=bss_hash(0);g_clock.bss_after1=bss_hash(1);
    if (g_clock.bss_before0!=g_clock.bss_after0) lost|=16u;
    if (g_clock.bss_before1!=g_clock.bss_after1) lost|=32u;
    g_clock.ram_lost_mask=lost;
    return !lost;
}
uint32_t clock_begin(uint32_t mode)
{
    ++g_clock.requests;
    if (g_clock.owned || (mode<CLOCK_MACSLP || mode>CLOCK_STATION) ||
        (core_status()&1u) || foundation_read32(0xb8640138u)) {
        ++g_clock.declines;return 0;
    }
    uint32_t sys=compat_read8(SYS_CLK),ctrl=compat_read8(WLLPS);
    if ((sys&MACSLP) || (ctrl&1u) || sys==255u || ctrl==255u) {
        ++g_clock.declines;return 0;
    }
    g_clock.saved_sys=sys;g_clock.saved_ctrl=ctrl;
    g_clock.saved_options=compat_read16(OPTIONS);
    g_clock.mode=mode;g_clock.error=0;g_clock.wake_requested=0;
    g_clock.options_owned=0;g_clock.transition_polls=0;
    g_clock.free_before=foundation_now_us();
    g_clock.raw_before=foundation_read32(0xb8640560u);
    g_clock.owned=1;g_clock.state=CLOCK_WAIT_SLEEP;
    /* HALMAC_RTL8821C_TRANS_ACT_TO_DEEP_LPS oscillator prerequisites.
     * These calibration/clock writes precede the selected engine policy and
     * do not themselves reset MAC/BB or touch RF state.
     */
    g_clock.saved_cr1=compat_read8(CR1);g_clock.saved_cal1=compat_read8(CAL1);
    g_clock.saved_cal3=compat_read8(CAL3);g_clock.saved_osc=compat_read8(OSC);
    g_clock.saved_cal=foundation_read32(CAL);
    compat_write8(CR1,g_clock.saved_cr1|4u);
    /* Stock 8003B558 programs the internal 32K oscillator through the
     * indirect calibration port before using it. HALMAC's abbreviated
     * enable-only sequence leaves a very inaccurate cold RC source here.
     * These analog calibration settings persist until a cold power cycle;
     * restoring the command bytes does not undo the analog configuration.
     */
    foundation_write32(CAL,(foundation_read32(CAL)&0xffc0ffffu)|0x20000u);
    if (!source_delay(10u)) return 1;
    foundation_write32(CAL,(foundation_read32(CAL)&0xffff003fu)|0x80000140u);
    if (!source_delay(10u)) return 1;
    foundation_write32(CAL,foundation_read32(CAL)&0xffc0ffffu);
    if (!source_delay(10u)) return 1;
    foundation_write32(CAL,(foundation_read32(CAL)&0xffff17ffu)|0x80009800u);
    if (!source_delay(10u)) return 1;
    g_clock.last_cal=foundation_read32(CAL);
    compat_write8(OSC,g_clock.saved_osc|1u);
    compat_write8(WLLPS,ctrl|2u);
    if (!(compat_read8(CR1)&4u) || !(compat_read8(OSC)&1u) ||
        !(compat_read8(WLLPS)&2u)) {fail(CLOCK_READBACK);return 1;}
    if (!source_delay(1000u)) return 1;
    g_clock.timer_owned=0;
    if (mode>=CLOCK_TIMER) {
        g_clock.saved_bcn=compat_read8(BCN);
        g_clock.saved_timer_lo=foundation_read32(PS_LO);
        g_clock.saved_timer_hi=foundation_read32(PS_HI);
        g_clock.saved_timer_ctrl=foundation_read32(PS_CTRL);
        /* Bare probe only: enable the otherwise stopped port-0 TSF counter.
         * Infrastructure integration must use the existing associated TSF.
         */
        if (mode==CLOCK_TIMER) {
            compat_write8(BCN,g_clock.saved_bcn|0x18u);
            if (!source_delay(100u)) return 1;
        }
        uint32_t lead=mode==CLOCK_TIMER?DEEP_TIMER_PROBE_US:g_clock.wake_at_us-foundation_now_us();
        if (mode == CLOCK_STATION && lead <= 200000u) lead = usb_lclk_ticks(lead);
        if (lead<2000u || lead>200000u) {
            restore_sources();g_clock.owned=0;g_clock.state=CLOCK_AWAKE;
            ++g_clock.declines;return 0;
        }
        uint32_t hi=foundation_read32(TSF_HI),lo=foundation_read32(TSF_LO);
        if (foundation_read32(TSF_HI)!=hi) {lo=foundation_read32(TSF_LO);++hi;}
        g_clock.raw_before=lo;g_clock.tsf_hi_before=hi;
        uint32_t deadline=lo+lead;
        g_clock.timer_deadline=deadline;g_clock.timer_pending=0;
        /* ROM800098A0/80009948: comparator0 is a 64-bit TSF deadline,
         * high word first, with selector0554[30:28] cleared. It is separate
         * from timer A and does not require keeping FTIMR unmasked asleep.
         */
        foundation_write32(PS_CTRL,g_clock.saved_timer_ctrl&0x8fffffffu);
        foundation_write32(0xb864013cu,PS_IRQ);
        foundation_write32(PS_HI,hi+(deadline<lo));
        foundation_write32(PS_LO,deadline);
        /* Qualify the timer's hardware wake route independently of CPU IE.
         * Only this comparator source is unmasked; timer A stays stopped.
         */
        foundation_write32(0xb8640138u,PS_IRQ);
        g_clock.timer_irq_owned=1;
        g_clock.timer_owned=1;++g_clock.timer_arms;
    }
    if (mode>=CLOCK_RETAIN) {
        compat_write16(OPTIONS,DEEP_ENGINE_OPTIONS);g_clock.options_owned=1;
        if (compat_read16(OPTIONS)!=DEEP_ENGINE_OPTIONS) {fail(CLOCK_READBACK);return 1;}
    }
    /* Same-epoch samples exclude oscillator preparation from elapsed-time
     * reconstruction. The timer's absolute deadline was armed above.
     */
    g_clock.free_before=foundation_now_us();
    g_clock.raw_before=foundation_read32(TSF_LO);
    ram_prepare();
    compat_write8(SYS_CLK,sys|MACSLP);
    publish();
    if (!(compat_read8(SYS_CLK)&MACSLP)) fail(CLOCK_READBACK);
    return 1;
}
void clock_poll(void)
{
    ++g_clock.polls;
    if (!g_clock.owned || g_clock.state==CLOCK_FAILED) return;
    g_clock.last_sys=compat_read8(SYS_CLK);
    g_clock.last_ctrl=compat_read8(WLLPS);
    if (g_clock.state==CLOCK_WAIT_SLEEP) {
        g_clock.last_tsf=foundation_read32(TSF_STATE);
        if (g_clock.last_tsf&0x8000u) {
            ++g_clock.sleep_stable;g_clock.transition_polls=0;
            g_clock.state=CLOCK_HOLD;
            publish();
            if (g_clock.mode>=CLOCK_RETAIN) {
                ++g_clock.engine_requests;
                /* Publish hold state before the engine can stop the core. */
                compiler_barrier();
#ifdef DIAG_HOST_TEST
                compat_write8(WLLPS,compat_read8(WLLPS)|1u);
#else
                core_deep_clock_gate(compat_read8(WLLPS)|1u);
#endif
            }
        } else if (++g_clock.transition_polls>=TRANSITION_POLLS)
            fail(CLOCK_SLEEP_TIMEOUT);
        return;
    }
    if (g_clock.state==CLOCK_HOLD) {
        ++g_clock.hold_polls;
        if (g_clock.mode>=CLOCK_TIMER && !(g_clock.last_ctrl&1u)) {
            /* Engine completion precedes software MACSLP release. SYS_CLK
             * can still read10 here; waiting for normal clock bits first
             * would leave autonomous wake dependent on host ACTIVE.
             * The next write is AON, then TSF_STATE proves restoration.
             */
            g_clock.wake_requested=1;
        }
        /* Poll counts are execution observations, never a sleep duration.
         * A stopped core cannot time out itself; the host wake is required.
         */
        if (!g_clock.wake_requested) return;
        if (g_clock.mode>=CLOCK_RETAIN && (g_clock.last_ctrl&1u)) {
            if (++g_clock.transition_polls>=TRANSITION_POLLS) fail(CLOCK_ENGINE_STUCK);
            return;
        }
        compat_write8(SYS_CLK,compat_read8(SYS_CLK)&~MACSLP);
        g_clock.state=CLOCK_WAIT_WAKE;g_clock.transition_polls=0;
        publish();
    }
    if (g_clock.state==CLOCK_WAIT_WAKE) {
        g_clock.last_tsf=foundation_read32(TSF_STATE);
        if (!(g_clock.last_tsf&0x8000u)) {
            g_clock.free_after=foundation_now_us();
            g_clock.raw_after=foundation_read32(TSF_LO);
            if (g_clock.timer_owned) {
                g_clock.tsf_after=g_clock.raw_after;
                uint32_t pending=foundation_read32(0xb864013cu)&PS_IRQ;
                g_clock.timer_pending|=pending;
                g_clock.rpwm_at_wake=compat_read8(0xb8600083u);
                if (pending && (g_clock.rpwm_at_wake&1u))
                    ++g_clock.timer_pending_wakes;
                if (!g_clock.timer_pending) {
                    foundation_write32(0xb8640138u,0);g_clock.timer_irq_owned=0;
                }
                foundation_write32(PS_HI,g_clock.saved_timer_hi);
                foundation_write32(PS_LO,g_clock.saved_timer_lo);
                foundation_write32(PS_CTRL,g_clock.saved_timer_ctrl);
                /* A latched CPU line must be acknowledged by its ISR after
                 * clocks are restored, rather than cleared underneath it.
                 */
                if (g_clock.mode==CLOCK_TIMER) compat_write8(BCN,g_clock.saved_bcn);
                g_clock.timer_owned=0;
            }
            if (g_clock.options_owned) {
                compat_write16(OPTIONS,g_clock.saved_options);
                if (compat_read16(OPTIONS)!=g_clock.saved_options) {fail(CLOCK_READBACK);return;}
            }
            if ((compat_read8(SYS_CLK)&MACSLP) || (compat_read8(WLLPS)&1u)) {
                fail(CLOCK_READBACK);return;
            }
            /* One atomic command restore; byte writes with WR=1 can
             * accidentally reprogram an unrelated analog register.
             */
            restore_sources();
            if (!ram_check()) {fail(CLOCK_RETENTION);return;}
#ifndef DIAG_HOST_TEST
            compat_write8(0xb8600082u,g_deep_resume.saved_boot);
            g_deep_resume.active=0;
#endif
            g_clock.owned=0;g_clock.state=CLOCK_AWAKE;++g_clock.wakes;
            publish();
        } else if (++g_clock.transition_polls>=TRANSITION_POLLS)
            fail(CLOCK_WAKE_TIMEOUT);
    }
}
