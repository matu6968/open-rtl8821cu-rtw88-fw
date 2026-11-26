/* SPDX-License-Identifier: GPL-2.0-only */
#include "wowlan.h"
#include "../broader/broader.h"
#include "../doze/doze.h"
#include "../doze/radio.h"
#include "../powersave/lps.h"

#define WOW_CTRL 0xb8640690u
#define RXPKT_NUM 0xb8640284u
#define RX_READ 0xb8641118u
#define RX_WRITE 0xb864111cu
#define RX_END 0xb864011cu
#define RX_BASE 0xb8700000u
#define WAKE_REASON 0xb86401c7u
#define DBG6 0xb86010f8u
#define DBG7 0xb86010fcu
#define WOW_MASK 0x2fu
#define WOW_ARM 0x26u /* WOWHCI | MAGIC | WOWEN; neither unicast nor force */
#define RX_RELEASE (1u << 18)
#define RX_IDLE (1u << 17)
#define RX_POLL (1u << 16)
#define RX_PTR_MASK 0x3ffffu
#define RX_UPDATE_MASK 0x00f1ffffu
#define DMA_TIMEOUT_US 20000u
#define RELEASE_TIMEOUT_US 2000u

volatile struct wowlan_status g_wowlan = {
    .magic=WOWLAN_MAGIC,.abi_version=1,.struct_bytes=sizeof(struct wowlan_status)
};
_Static_assert(sizeof(struct wowlan_status)==188,"WOW1 status ABI");

static uint32_t expired(uint32_t now,uint32_t deadline)
{
    return (int32_t)(now-deadline)>=0;
}

static uint32_t live_owner(void)
{
    if (station_transport_active()) return 1;
    /* Only a physical MAC/BB lifecycle reset abandons the ownership lease.
     * A changed BSSID/AID while the MAC runs must retain a failed owner until
     * disarm, rather than leaking an enabled matcher/release bit to Linux.
     */
    g_wowlan.error=WOWLAN_CONTEXT_LOST;g_wowlan.owned=0;
    g_wowlan.release_pending=0;g_wowlan.state=WOWLAN_IDLE;
    ++g_wowlan.cancels;return 0;
}

static uint32_t context(uint32_t saved)
{
    if (!station_transport_active() || radio_owned() || !radio_awake() ||
        !g_doze.connected || g_lps.state!=LPS_ACTIVE || g_lps.ap_ps ||
        (diag_hw_read(0xb8640604u)&128u) ||
        (compat_read8(0xb8640102u)&3u)!=2u) return 0;
    uint32_t mac=diag_hw_read(0xb8640610u),mh=compat_read16(0xb8640614u);
    uint32_t bssid=diag_hw_read(0xb8640618u),bh=compat_read16(0xb864061cu);
    uint32_t aid=compat_read16(0xb86406a8u)&2047u;
    if ((mac&1u) || !(mac|mh) || (mac==0xffffffffu && mh==65535u) ||
        (bssid&1u) || !(bssid|bh) || (bssid==0xffffffffu && bh==65535u) ||
        !aid || aid>2007u) return 0;
    if (saved) return mac==g_wowlan.mac_lo && mh==g_wowlan.mac_hi &&
        bssid==g_wowlan.bssid_lo && bh==g_wowlan.bssid_hi && aid==g_wowlan.aid;
    g_wowlan.mac_lo=mac;g_wowlan.mac_hi=mh;
    g_wowlan.bssid_lo=bssid;g_wowlan.bssid_hi=bh;
    g_wowlan.aid=aid;
    return 1;
}

static uint32_t tx_quiet(void)
{
    /* The host leaves LPS and stops its network queues before F0. Decline
     * until the real TX/C2H/report producers are quiet; there is no flush or
     * manufactured TX completion. The current F0 still owns its HMEBOX bit.
     */
    return (compat_read16(0xb864041au)&0x7ffu)==0x7ffu &&
        !(diag_hw_read(0xb864041cu)&0xa0000000u) &&
        !diag_hw_read(0xb86405f8u) && !(compat_read8(0xb8640296u)&1u) &&
        (diag_hw_read(0xb8640250u)&RX_PTR_MASK)==(diag_hw_read(0xb864024cu)&RX_PTR_MASK) &&
        (compat_read8(0xb864047cu)&127u)==(compat_read8(0xb864047du)&127u);
}

static void acknowledge(uint32_t status)
{
    /* Lease shared DBG7 only after host stopped diagnostic reads (DBG6 even).
     * The host matches its nonzero cookie and must not read CPU RAM until the
     * arm/disarm handshake completes. Background diagnostics never write it.
     */
    g_wowlan.ack=0x57000000u|(g_wowlan.cookie<<8)|status;
    if (!(diag_hw_read(DBG6)&1u)) diag_hw_write(DBG7,g_wowlan.ack);
}

static void fail(uint32_t error)
{
    g_wowlan.error=error;g_wowlan.state=WOWLAN_FAILED;
    if (error==WOWLAN_READBACK) ++g_wowlan.readback_errors;
    if (error==WOWLAN_DMA_TIMEOUT || error==WOWLAN_RELEASE_TIMEOUT)
        ++g_wowlan.timeouts;
    acknowledge(0x80u);
    /* A failed transaction retains ownership until explicit disarm or an
     * external MAC reset. Never hand the host a partially owned RX FIFO.
     */
}

uint32_t station_wowlan_owned(void) { return g_wowlan.owned; }
void station_wowlan_init(void) { g_wowlan.state=WOWLAN_IDLE; }

uint32_t station_wowlan_h2c(uint32_t lo,uint32_t hi)
{
    if ((lo&255u)!=WOWLAN_H2C) return 0;
    if (!diag_uses_usb()) return 2;
    ++g_wowlan.commands;g_wowlan.last_lo=lo;g_wowlan.last_hi=hi;
    uint32_t enable=(lo>>8)&255u,cookie=lo>>16;
    if (hi!=WOWLAN_SIGNATURE || enable>1u || !cookie ||
        (diag_hw_read(DBG6)&1u)) {
        ++g_wowlan.rejected;
        if (!(diag_hw_read(DBG6)&1u)) {
            uint32_t old=g_wowlan.cookie;g_wowlan.cookie=cookie;
            acknowledge(0x80u);g_wowlan.cookie=old;
        }
        return 2;
    }
    g_wowlan.cookie=cookie;
    if (!enable) {
        if (!g_wowlan.owned) { acknowledge(0);return 1; }
        g_wowlan.state=WOWLAN_DISARMING;
        g_wowlan.deadline=foundation_now_us()+DMA_TIMEOUT_US;
        return 1;
    }
    if (g_wowlan.owned) { ++g_wowlan.rejected;acknowledge(0x80);return 2; }
    if (!station_transport_ready() || !context(0)) {
        ++g_wowlan.rejected;g_wowlan.error=WOWLAN_BAD_CONTEXT;
        acknowledge(0x80);return 2;
    }
    uint32_t ctrl=compat_read8(WOW_CTRL),rx=diag_hw_read(RXPKT_NUM);
    uint32_t end=diag_hw_read(RX_END)&RX_PTR_MASK;
    /* Port-0 BSSID selector must already be zero. Refuse another owner's
     * matcher/release transaction and an invalid or non-eight-byte RX ring.
     * RTL8821C RX FIFO is 16 KiB, minus the host-reserved report area.
     */
    if (!tx_quiet() || ctrl || (rx&(RX_RELEASE|RX_POLL)) || end<1023u || end>=16384u ||
        ((end+1u)&7u)) {
        ++g_wowlan.rejected;g_wowlan.error=WOWLAN_BUSY;
        acknowledge(0x80);return 2;
    }
    g_wowlan.error=0;g_wowlan.saved_ctrl=ctrl;
    g_wowlan.saved_release=rx&RX_RELEASE;g_wowlan.rx_end=end;
    g_wowlan.release_pending=0;g_wowlan.wake_reason=0;
    g_wowlan.owned=1;g_wowlan.state=WOWLAN_ARMING;
    g_wowlan.deadline=foundation_now_us()+DMA_TIMEOUT_US;
    compat_write8(WAKE_REASON,0);
    if (!live_owner()) return 1;
    compat_write8(WOW_CTRL,(ctrl&~WOW_MASK)|WOW_ARM);
    if (!live_owner()) return 1;
    if ((compat_read8(WOW_CTRL)&WOW_MASK)!=WOW_ARM) {fail(WOWLAN_READBACK);return 1;}
    /* Stock remote-wake enable transfers the FIFO with RW_RELEASE_EN.
     * Preserve read-only count/idle fields and do not request a release yet.
     */
    compat_write8(RXPKT_NUM+2,compat_read8(RXPKT_NUM+2)|4u);
    return 1;
}

static uint32_t ring_word(uint32_t offset)
{
    uint32_t size=g_wowlan.rx_end+1u;
    if (offset>=size) offset-=size;
    return diag_hw_read(RX_BASE+offset);
}

static void finish_disarm(void)
{
    /* Context was checked immediately before this function. Restore only
     * matcher/release bits; keep the real wake reason until the next arm.
     */
    compat_write8(WOW_CTRL,(compat_read8(WOW_CTRL)&~WOW_MASK)|g_wowlan.saved_ctrl);
    if (!live_owner()) return;
    compat_write8(RXPKT_NUM+2,(compat_read8(RXPKT_NUM+2)&~4u)|
                  (g_wowlan.saved_release>>16));
    if ((compat_read8(WOW_CTRL)&WOW_MASK)!=(g_wowlan.saved_ctrl&WOW_MASK) ||
        (diag_hw_read(RXPKT_NUM)&RX_RELEASE)!=g_wowlan.saved_release) {
        fail(WOWLAN_READBACK);return;
    }
    g_wowlan.owned=0;g_wowlan.state=WOWLAN_IDLE;
    ++g_wowlan.disarms;acknowledge(0);
}

uint32_t station_wowlan_pre(void)
{
    ++g_wowlan.polls;
    if (!g_wowlan.owned) return 1;
    /* External reset cancels stale ownership with no peripheral writes. */
    if (!live_owner()) return 0;
    uint32_t now=foundation_now_us(),rx=diag_hw_read(RXPKT_NUM);
    g_wowlan.last_rxpkt=rx;g_wowlan.last_ctrl=compat_read8(WOW_CTRL);
    if (g_wowlan.state==WOWLAN_DISARMING) {
        if (rx&RX_POLL) {
            if (expired(now,g_wowlan.deadline)) fail(WOWLAN_RELEASE_TIMEOUT);
            return 0;
        }
        finish_disarm();return 0;
    }
    if (g_wowlan.state==WOWLAN_FAILED) return 0;
    if (!context(1)) {fail(WOWLAN_CONTEXT_LOST);return 0;}
    if (g_wowlan.state==WOWLAN_ARMING) {
        if (!(rx&RX_RELEASE) || (g_wowlan.last_ctrl&WOW_MASK)!=WOW_ARM) {
            fail(WOWLAN_READBACK);return 0;
        }
        if (rx&RX_IDLE) {
            g_wowlan.state=WOWLAN_ARMED;++g_wowlan.arms;acknowledge(1);
        } else if (expired(now,g_wowlan.deadline)) fail(WOWLAN_DMA_TIMEOUT);
        return 0;
    }
    if (g_wowlan.state!=WOWLAN_ARMED) return 0;
    if (!(rx&RX_RELEASE) || (g_wowlan.last_ctrl&WOW_MASK)!=WOW_ARM ||
        (diag_hw_read(RX_END)&RX_PTR_MASK)!=g_wowlan.rx_end) {
        fail(WOWLAN_READBACK);return 0;
    }
    if (g_wowlan.release_pending) {
        if (rx&RX_POLL) {
            if (expired(now,g_wowlan.release_deadline)) fail(WOWLAN_RELEASE_TIMEOUT);
            return 0;
        }
        g_wowlan.release_pending=0;++g_wowlan.rx_released;
    }
    if (!(rx>>24)) return 0;
    uint32_t head=diag_hw_read(RX_READ)&RX_PTR_MASK;
    uint32_t write=diag_hw_read(RX_WRITE)&RX_PTR_MASK;
    g_wowlan.rx_read=head;g_wowlan.rx_write=write;
    if (head>g_wowlan.rx_end || write>g_wowlan.rx_end || (head&7u)) {
        fail(WOWLAN_BAD_RING);return 0;
    }
    for (uint32_t i=0;i<6;++i) g_wowlan.last_descriptor[i]=ring_word(head+(i<<2));
    uint32_t d0=g_wowlan.last_descriptor[0],bytes=d0&0x3fffu;
    uint32_t overhead=24u+(((d0>>16)&15u)<<3)+((d0>>24)&3u);
    uint32_t span=(bytes+overhead+7u)&~7u,size=g_wowlan.rx_end+1u;
    if (!bytes || span>=size || ring_word(head)!=d0 ||
        (diag_hw_read(RX_READ)&RX_PTR_MASK)!=head) {
        fail(WOWLAN_BAD_RING);return 0;
    }
    ++g_wowlan.rx_packets;
    /* MAGIC_WAKE is a hardware match, independent of ordinary traffic or TIM.
     * CRC/ICV errors, software-decrypted frames and C2H never wake this host.
     * The hardware matches against the live port-0 MAC, pinned at arm time.
     */
    if (!(d0&0x0800c000u) && !(g_wowlan.last_descriptor[2]&0x10000000u) &&
        (g_wowlan.last_descriptor[3]&0x80000000u)) {
        ++g_wowlan.magic_matches;g_wowlan.wake_reason=0x21u;
        g_wowlan.wake_time_us=now;compat_write8(WAKE_REASON,0x21u);
        compiler_barrier();
        if (!live_owner()) return 0;
        compat_write8(WOW_CTRL,compat_read8(WOW_CTRL)|1u);
        ++g_wowlan.wake_requests;g_wowlan.state=WOWLAN_TRIGGERED;
        /* Preserve the matched packet for the host when RX DMA resumes.
         * Do not toggle CPWM, reduce clocks or invent a completed host wake.
         */
        return 0;
    }
    if (d0&0x0800c000u) ++g_wowlan.rx_bad;
    uint32_t next=head+span;if (next>=size) next-=size;
    g_wowlan.rx_next=next;g_wowlan.release_pending=1;
    g_wowlan.release_deadline=now+RELEASE_TIMEOUT_US;
    /* Register definition: next[15:0] -> bits15:0, next[19:16] -> bits23:20,
     * then RXPKT_RELEASE_POLL. Never write the RXFF hardware read pointer.
     */
    if (!live_owner()) return 0;
    diag_hw_write(RXPKT_NUM,(rx&~RX_UPDATE_MASK)|(next&65535u)|
                  ((next>>16)<<20)|RX_POLL);
    return 0;
}
