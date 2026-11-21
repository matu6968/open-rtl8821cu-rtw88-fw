/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8811CU_COMPAT_H
#define RTL8811CU_COMPAT_H
#include "../diagnostic/diag.h"

#define COMPAT_MAGIC 0x31504d43u /* CMP1; independent versioned control ABI */
#define EFUSE_SIZE 512u
#define EFUSE_USABLE 416u /* physical size minus 96 protected bytes */
#define FEATURE_SIZE 13u
enum compat_state { COMPAT_STARTING, COMPAT_READY, COMPAT_FAILED };
enum compat_error { COMPAT_OK, COMPAT_EFUSE_TIMEOUT, COMPAT_EFUSE_TRUNCATED,
    COMPAT_EFUSE_RANGE, COMPAT_BAD_BOARD };
enum compat_event { EVENT_FACTORY = 16, EVENT_FEATURE = 17,
    EVENT_H2C_UNSUPPORTED = 18, EVENT_CONTROL_ERROR = 19 };

struct board_info {
    uint32_t valid, rtl_id, mac_valid, mac_lo, mac_hi;
    uint32_t rfe, package, crystal, thermal, channel_plan, country;
    uint32_t rf_board, bt_setting, antenna, pa, lna_2g, lna_5g;
    uint32_t bt_board, shared_antenna;
};
struct compat_status {
    uint32_t magic, abi_version, struct_bytes, state, error;
    uint32_t efuse_bytes, efuse_used, feature_requested, feature_reports, ready;
    uint32_t h2c_count, h2c_unsupported, last_box, last_h2c_lo, last_h2c_hi;
    uint32_t packet_transport_supported, rf_supported, bt_supported;
    struct board_info board;
};
_Static_assert(sizeof(struct compat_status) == 148, "control ABI v1");
extern volatile struct compat_status g_compat;
extern uint8_t g_efuse_physical[EFUSE_SIZE], g_efuse_logical[EFUSE_SIZE];
extern uint8_t g_feature[FEATURE_SIZE];

#ifdef DIAG_HOST_TEST
uint32_t compat_read8(uint32_t address);
uint32_t compat_read16(uint32_t address);
void compat_write8(uint32_t address, uint32_t value);
void compat_write16(uint32_t address, uint32_t value);
#else
static inline uint32_t compat_read8(uint32_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline uint32_t compat_read16(uint32_t a) { return *(volatile uint16_t *)(uintptr_t)a; }
static inline void compat_write8(uint32_t a, uint32_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline void compat_write16(uint32_t a, uint32_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
#endif
uint32_t compat_expand_efuse(const uint8_t *physical, uint32_t size,
                             uint8_t *logical, uint32_t *used);
void compat_decode_board(const uint8_t *logical, volatile struct board_info *board);
void compat_build_feature(const uint8_t *physical, uint8_t *feature);
void compat_init(void);
void compat_poll(void);
#endif
