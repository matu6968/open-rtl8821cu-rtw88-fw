/* SPDX-License-Identifier: GPL-2.0-only */
#include "rate.h"
#include "../fwlog/fwlog.h"

/* These are the actual stock table aliases, not the older chips' 0484
 * INIDATA_RATE_SEL register. 80106C84 installs masks; 80107C7C publishes rates;
 * ROM 80014290 verifies the published rate|SGI encoding.
 */
#define RATE_TABLE 0xb8660000u
#define RATE_MASK  0xb8661c00u

struct rate_peer {
    uint32_t mask, config, good;
    uint8_t valid, connected, rate, bw, sgi, vht, dirty, rssi;
};
static struct rate_peer peers[RATE_MACIDS];
static uint32_t next_peer;
#define c2h_sequence (g_station.c2h_seq)
volatile struct rate_status g_rate = {
    .magic = RATE_MAGIC, .abi_version = 1,
    .struct_bytes = sizeof(struct rate_status)
};

/* Nominal rates in 500-kbit/s units. Used only to order legal candidates:
 * CCK11 is faster than OFDM6, and HT/VHT MCS0 can be slower than legacy54.
 * SGI comparisons are scaled by 10 versus 9, avoiding unqualified division.
 */
static const uint16_t legacy_rate[12] = {2,4,11,22,12,18,24,36,48,72,96,108};
static const uint16_t mcs_rate[3][10] = {
    {13,26,39,52,78,104,117,130,156,0},
    {27,54,81,108,162,216,243,270,324,360},
    {59,117,176,234,351,468,527,585,702,780}
};

static uint32_t rate_code(const struct rate_peer *p, uint32_t bit)
{
    return bit < 12 ? bit : (p->vht ? bit + 32 : bit);
}

static uint32_t rate_bit(const struct rate_peer *p, uint32_t code)
{
    code &= 127;
    return code < 12 ? code : p->vht && code >= 44 ? code - 32 : code;
}

static uint32_t nominal(const struct rate_peer *p, uint32_t bit)
{
    if (bit < 12) return legacy_rate[bit] * 9u;
    uint32_t value = mcs_rate[p->bw][bit - 12];
    uint32_t scaled = (value << 3) + value;
    if (p->sgi) scaled += value;
    return scaled;
}

static uint32_t lowest(const struct rate_peer *p)
{
    uint32_t chosen = 32, value = 0xffffffffu;
    for (uint32_t bit = 0; bit < 22; ++bit) {
        if ((p->mask & (1u << bit)) && nominal(p, bit) < value) {
            chosen = bit;
            value = nominal(p, bit);
        }
    }
    return chosen;
}

static uint32_t adjacent(const struct rate_peer *p, uint32_t up)
{
    uint32_t current = rate_bit(p, p->rate);
    if (current >= 22 || !(p->mask & (1u << current))) return lowest(p);
    uint32_t value = nominal(p, current), chosen = current;
    uint32_t best = up ? 0xffffffffu : 0;
    for (uint32_t bit = 0; bit < 22; ++bit) {
        if (!(p->mask & (1u << bit))) continue;
        uint32_t candidate = nominal(p, bit);
        if ((up && candidate > value && candidate < best) ||
            (!up && candidate < value && candidate > best)) {
            chosen = bit;
            best = candidate;
        }
    }
    return chosen;
}

static void readback_failed(struct rate_peer *p)
{
    /* A failed policy or mask write can leave the selected rate unchanged.
     * Invalidate the whole software generation rather than letting an older
     * dirty flag publish metadata for a partially programmed configuration.
     */
    p->valid = 0;
    p->dirty = 0;
    p->good = 0;
    ++g_rate.readback_errors;
    g_rate.state = 2;
}

static uint32_t table_matches(uint32_t macid, const struct rate_peer *p,
                              uint32_t code)
{
    uint32_t base = RATE_TABLE + (macid << 5);
    return compat_read8(base) == code &&
           (compat_read8(base + 1) & 0x18) == ((uint32_t)p->bw << 3) &&
           diag_hw_read(RATE_MASK + (macid << 3)) == p->mask &&
           diag_hw_read(RATE_MASK + (macid << 3) + 4) == 0;
}

static uint32_t program(uint32_t macid, struct rate_peer *p, uint32_t bit)
{
    if (bit >= 22 || !(p->mask & (1u << bit))) return 0;
    uint32_t code = rate_code(p, bit);
    if (code >= 12 && p->sgi) code |= 128;
    uint32_t base = RATE_TABLE + (macid << 5);
    /* Stock retains the host/factory power selection in the low three bits.
     * This controller does not implement firmware TX power training/probes.
     */
    uint32_t policy = (compat_read8(base + 1) & 7) | (p->bw << 3);
    diag_hw_write(RATE_MASK + (macid << 3), p->mask);
    diag_hw_write(RATE_MASK + (macid << 3) + 4, 0); /* one RF path / one stream */
    compat_write8(base + 1, policy);
    compat_write8(base + 6, 0); /* normal selected rate, no vendor probe mode */
    if (macid < 3) {
        compat_write8(0xb8640453u, code);
        compat_write8(0xb86404cfu, policy);
    }
    compiler_barrier();
    compat_write8(base, code);
    if (!table_matches(macid, p, code) || compat_read8(base + 1) != policy) {
        readback_failed(p);
        return 0;
    }
    p->rate = code;
    p->dirty = 1;
    p->good = 0;
    ++g_rate.programs;
    g_rate.last_macid = macid;
    g_rate.last_rate = code;
    g_rate.last_bw = p->bw;
    return 1;
}

void station_rate_init(void)
{
    for (uint32_t i = 0; i < RATE_MACIDS; ++i) {
        peers[i].valid = 0;
        peers[i].connected = 0;
        peers[i].dirty = 0;
        peers[i].good = 0;
    }
    next_peer = c2h_sequence = 0;
    g_rate.state = 1;
}

static uint32_t configure(uint32_t lo, uint32_t mask)
{
    uint32_t macid = (lo >> 8) & 255, id = (lo >> 16) & 31;
    uint32_t bw = (lo >> 24) & 3, vht = (lo >> 28) & 3;
    if (macid >= RATE_MACIDS) {
        ++g_rate.bad_commands;
        return 2;
    }
    struct rate_peer *p = &peers[macid];
    /* Incoming BW is the host's maximum. NO_UPDATE retains a smaller current
     * width, but cannot retain a width above a newly reduced maximum. Dynamic
     * bandwidth probing/search is deliberately not part of this controller.
     */
    if (p->valid && (lo & (1u << 27)) && p->bw < bw) bw = p->bw;
    /* Disabled RA and enabled firmware power training need different policy;
     * neither is sent by the current rtw88 path. Do not claim these requests.
     */
    if (lo & (1u << 31) || !(lo & (1u << 30))) goto unsupported;
    /* rtw88 defines these as the one-stream RAID choices. All other RAID
     * layouts require unsupported streams or an unknown table interpretation.
     */
    uint32_t allowed;
    if (id == 1 || id == 3 || id == 5) {
        if (vht || bw > 1) goto unsupported;
        allowed = 0x000fffffu;
    } else if (id == 10 || id == 11) {
        if (!vht || bw > 2) goto unsupported;
        allowed = 0x003fffffu;
    } else if (id == 6 || id == 7) {
        if (vht || bw) goto unsupported;
        allowed = 0xfffu;
    } else if (id == 8) {
        if (vht || bw) goto unsupported;
        allowed = 15;
    } else goto unsupported;
    if (id == 5 || id == 7 || id == 10) allowed &= ~15u;
    /* VHT MCS9 has no one-stream 20-MHz encoding. */
    if (vht && !bw) allowed &= ~(1u << 21);
    mask &= allowed;
    if (!mask) goto unsupported;
    uint32_t old_bit = rate_bit(p, p->rate);
    uint32_t keep = p->valid && !(lo & (3u << 21));
    p->mask = mask;
    p->config = lo;
    p->sgi = (lo >> 23) & 1;
    p->vht = vht != 0;
    p->bw = bw;
    p->valid = 1;
    /* Media status can arrive before or after RA setup in different drivers.
     * A usable mask is sufficient for initial traffic; explicit disconnect
     * invalidates it and prevents stale feedback/report publication.
     */
    p->connected = 1;
    uint32_t bit = keep && old_bit < 22 && (mask & (1u << old_bit)) ? old_bit : lowest(p);
    g_rate.last_macid = macid;
    g_rate.last_mask = mask;
    g_rate.last_config = lo;
    ++g_rate.mask_updates;
    fw_log(FW_LOG_RA, "ID=%bx,DisRA=%bx,DisPT=%bx,RtID=%bx",
           macid, lo >> 31, (lo >> 30) & 1u, id);
    fw_log(FW_LOG_RA, "BW=%bx,BWM=%bx,NoUpBw=%bx,SGI=%bx,VHT=%bx",
           bw, (lo >> 24) & 3u, (lo >> 27) & 1u, (lo >> 23) & 1u, vht);
    return program(macid, p, bit) ? 1 : 2;
unsupported:
    ++g_rate.unsupported_configs;
    return 2;
}

uint32_t station_rate_h2c(uint32_t lo, uint32_t hi)
{
    uint32_t command = lo & 255;
    if (command != 0x40 && command != 0x42 && command != 1) return 0;
    ++g_rate.commands;
    g_rate.last_h2c_lo = lo;
    g_rate.last_h2c_hi = hi;
    if (command == 0x40) return configure(lo, hi);
    uint32_t macid = command == 1 ? (lo >> 16) & 255 : (lo >> 8) & 255;
    if (macid >= RATE_MACIDS) {
        ++g_rate.bad_commands;
        return 2;
    }
    struct rate_peer *p = &peers[macid];
    g_rate.last_macid = macid;
    if (command == 1) {
        ++g_rate.media_updates;
        /* Range media reports are not sent by the current rtw88 interface. */
        if (lo & 0x0000fe00u || lo & 0xff000000u || hi) {
            ++g_rate.bad_commands;
            return 2;
        }
        p->connected = (lo >> 8) & 1;
        if (!p->connected) {
            p->valid = p->dirty = 0;
            p->good = 0;
        }
    } else {
        uint32_t rssi = lo >> 24;
        if (rssi > 100) {
            ++g_rate.bad_commands;
            return 2;
        }
        p->rssi = rssi;
        g_rate.last_rssi = rssi;
        ++g_rate.rssi_updates;
    }
    return 1;
}

void station_rate_tx_report(const uint32_t report[4])
{
    /* Stock 801094C0, type-0 hardware record, sixteen bytes:
     * MACID=byte2, selected/final rates=byte3/9, OK/fail/total=byte4/5/12
     * low six bits. Byte6 bit0 marks a report outside this SU data path.
     */
    uint32_t macid = (report[0] >> 16) & 255;
    uint32_t initial = report[0] >> 24;
    uint32_t final = (report[2] >> 8) & 255;
    uint32_t ok = report[1] & 63, fail = (report[1] >> 8) & 63;
    uint32_t total = report[3] & 63;
    if ((report[0] & 0xe0) || macid >= RATE_MACIDS ||
        (report[1] & (1u << 16)) || !total || ok + fail != total) goto ignored;
    struct rate_peer *p = &peers[macid];
    if (!p->valid || !p->connected || initial != p->rate) goto ignored;
    ++g_rate.report_samples;
    g_rate.last_macid = macid;
    g_rate.last_successes = ok;
    g_rate.last_failures = fail;
    g_rate.last_count = total;
    g_rate.last_retry = report[2] & 127;
    uint32_t bit;
    if (fail * 4 >= total || final != initial) {
        bit = adjacent(p, 0);
        p->good = 0;
        if (rate_code(p, bit) != (p->rate & 127) && program(macid, p, bit)) {
            ++g_rate.rate_down;
            fw_log(FW_LOG_RA, "Rate: decreased");
        }
    } else if (!fail) {
        p->good += ok; /* at most 63 per record, reset at a sixteen-frame window */
        if (p->good >= 16) {
            bit = adjacent(p, 1);
            p->good = 0;
            if (rate_code(p, bit) != (p->rate & 127) && program(macid, p, bit)) {
                ++g_rate.rate_up;
                fw_log(FW_LOG_RA, "Rate: increased");
            }
        }
    } else p->good = 0;
    return;
ignored:
    ++g_rate.report_ignored;
}

void station_rate_poll(void)
{
    if (!g_station.iqk_completed) return;
    for (uint32_t budget = 0; budget < 4; ++budget) {
        uint32_t macid = next_peer;
        next_peer = (next_peer + 1) & 127;
        struct rate_peer *p = &peers[macid];
        if (!p->valid || !p->connected || !p->dirty) continue;
        if (!table_matches(macid, p, p->rate)) {
            readback_failed(p);
            continue;
        }
        uint8_t frame[9] = {0x0c, c2h_sequence & 255, p->rate, macid, 0, 0, 0, 0, p->bw};
        if (!station_c2h_send(frame, sizeof(frame))) {
            ++g_rate.c2h_busy;
            return;
        }
        ++c2h_sequence;
        ++g_rate.c2h_reports;
        p->dirty = 0;
    }
}
