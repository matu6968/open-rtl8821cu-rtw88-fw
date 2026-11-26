/* SPDX-License-Identifier: GPL-2.0-only */
#include "compat.h"

/* 8821C packet eFuse format, also decoded by rtw88/efuse.c. Newer records
 * replace selected words from earlier records. Never read the protected tail
 * as packet headers, and never accept a word crossing either map boundary.
 * The raw physical and expanded logical maps are retained for calibration.
 */
uint32_t compat_expand_efuse(const uint8_t *physical, uint32_t size,
                             uint8_t *logical, uint32_t *used)
{
    uint32_t p = 0;
    *used = 0;
    for (uint32_t i = 0; i < EFUSE_SIZE; ++i)
        logical[i] = 0xff;
    while (p < size) {
        uint32_t h = physical[p++], block, words;
        if (h == 0xff) {
            *used = p - 1;
            return COMPAT_OK;
        }
        if ((h & 31) == 15) {
            if (p == size)
                return COMPAT_EFUSE_TRUNCATED;
            uint32_t h2 = physical[p++];
            if (h2 == 0xff) {
                *used = p - 2;
                return COMPAT_OK;
            }
            block = ((h2 & 0xf0) >> 1) | (h >> 5);
            words = h2 & 15;
        } else {
            block = h >> 4;
            words = h & 15;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            if (words & (1u << i))
                continue;
            uint32_t l = (block << 3) + (i << 1);
            if (l + 1 >= EFUSE_SIZE)
                return COMPAT_EFUSE_RANGE;
            if (p + 1 >= size)
                return COMPAT_EFUSE_TRUNCATED;
            logical[l] = physical[p++];
            logical[l + 1] = physical[p++];
        }
    }
    *used = p;
    return COMPAT_OK;
}

/* Match the bus-specific MAC fields and common RF fields in rtw8821c_efuse.
 * Preserve erased fields as 0xff and leave default/regulatory policy to Linux.
 */
void compat_decode_board(const uint8_t *l, enum diag_hci hci,
                         volatile struct board_info *b)
{
    uint32_t offset = 0, any = 0, all = 255;
    switch (hci) {
    case DIAG_HCI_USB:  offset = 0x107; break;
    case DIAG_HCI_SDIO: offset = 0x11a; break;
    case DIAG_HCI_PCIE: offset = 0xd0; break;
    default: break;
    }
    b->mac_valid = b->mac_lo = b->mac_hi = 0;
    if (offset) {
        const uint8_t *mac = l + offset;
        b->mac_lo = mac[0] | ((uint32_t)mac[1] << 8) |
            ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
        b->mac_hi = mac[4] | ((uint32_t)mac[5] << 8);
        for (uint32_t i = 0; i < 6; ++i) {
            any |= mac[i];
            all &= mac[i];
        }
        b->mac_valid = any && all != 255 && !(mac[0] & 1);
    }
    b->rtl_id = l[0] | ((uint32_t)l[1] << 8);
    b->rfe = l[0xca] & 31;
    b->package = (l[0xca] >> 5) & 1;
    b->crystal = l[0xb9]; b->thermal = l[0xba];
    b->channel_plan = l[0xb8]; b->country = l[0xcb] | ((uint32_t)l[0xcc] << 8);
    b->rf_board = l[0xc1]; b->bt_setting = l[0xc3];
    b->antenna = l[0xc9]; b->pa = l[0xbc];
    b->lna_2g = l[0xbd] | ((uint32_t)l[0xbe] << 8);
    b->lna_5g = l[0xbf] | ((uint32_t)l[0xc0] << 8);
    b->bt_board = l[0xc1] == 255 ? DIAG_UNKNOWN : ((l[0xc1] & 0xe0) == 0x20);
    b->shared_antenna = l[0xc3] == 255 ? DIAG_UNKNOWN : !(l[0xc3] & 1);
    b->valid = b->rtl_id == 0x8129 && l[0xca] != 255;
}

/* Reconstruct stock 80037904/8003796c/800379b0 and capability unpacking at
 * 80037ae4. These are physical HW capabilities, not firmware offload promises.
 * Reserved bits in byte 7 are deliberately forced as in the reference.
 */
void compat_build_feature(const uint8_t *p, uint8_t *f)
{
    uint32_t bw = p[0x1fb] & 7, ant = p[0x1fb] >> 5;
    uint32_t proto = (p[0x1fd] >> 2) & 3, nic = p[0x1fd] >> 6;
    /* Stock 80037a44 substitutes these alternate fields only for zero-valued
     * capabilities. The NSS bits have no such substitution.
     */
    if (!bw) bw = p[0x1fa] & 7;
    if (!ant) ant = p[0x1fa] >> 5;
    if (!proto) proto = p[0x1fd] & 3;
    if (!nic) nic = (p[0x1fd] >> 4) & 3;
    for (uint32_t i = 0; i < 6; ++i)
        f[i] = p[0x1f4 + i];
    f[6] = bw | (p[0x1fb] & 0x18) | (ant << 5);
    f[7] = (proto << 2) | (nic << 6) | 0x33;
    for (uint32_t i = 0; i < 4; ++i)
        f[8 + i] = p[0x1f0 + i];
    f[12] = p[0x1fc];
}
