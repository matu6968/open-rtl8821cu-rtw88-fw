/* SPDX-License-Identifier: GPL-2.0-only */
#include "station.h"
#include "../broader/broader.h"
#include <stdbool.h>
#include "../deep/clock.h"
volatile struct deep_iqk_status g_deep_iqk = {
    .magic=0x314b4944u,.abi_version=1,.struct_bytes=sizeof(struct deep_iqk_status)
};
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef bool boolean;
#define BIT(n) (1u << (n))
#define BIT26 BIT(26)
#define BIT25 BIT(25)
#define BIT24 BIT(24)
#define MASKDWORD 0xffffffffu
#define RFREGOFFSETMASK 0xfffffu
#define NUM 4
#define SS_8821C 1
#define MAC_REG_NUM_8821C 3
#define BB_REG_NUM_8821C 10
#define RF_REG_NUM_8821C 6
#define TX_IQK 0
#define RX_IQK 1
#define TXIQK 0
#define RXIQK 1
#define RXIQK1 1
#define RXIQK2 2
#define LOK_delay_8821C 2
#define GS_delay_8821C 2
#define WBIQK_delay_8821C 2
#define kcount_limit_80m 2
#define kcount_limit_others 4
#define rxiqk_gs_limit 4
#define ODM_BAND_2_4G 0
#define ODM_BAND_5G 1
#define ODM_COMP_CALIBRATION 0
enum odm_rf_radio_path_e { ODM_RF_PATH_A };
#include "iqk_state.h"
struct PHY_DM_STRUCT {
    struct _IQK_INFORMATION IQK_info;
    struct {
        u32 iqk_step, iqk_start_time, iqk_progressing_time, iqk_total_progressing_time;
    } rf_calibrate_info;
    u8 *p_band_type, *p_band_width;
    bool mp_mode, is_linked, ext_pa, ext_pa_5g, debug_components;
    u32 support_interface, cut_version, n_iqk_cnt, n_iqk_ok_cnt, n_iqk_fail_cnt;
};
static struct PHY_DM_STRUCT dm;
static u8 band, bandwidth;
static u32 access_error;

static u32 mmio(u32 reg)
{
    /* Same CPU aliases as the runtime; RF direct read is B8642800+4*reg. */
    return ((reg < 0x100 || (reg >= 0x1000 && reg < 0x1100)) ? 0xb8600000u : 0xb8640000u) + reg;
}
static u32 shift(u32 mask)
{
    u32 n = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; ++n; }
    return n;
}
static u32 odm_read_4byte(struct PHY_DM_STRUCT *unused, u32 reg)
{
    (void)unused; return diag_hw_read(mmio(reg));
}
static void odm_write_4byte(struct PHY_DM_STRUCT *unused, u32 reg, u32 value)
{
    (void)unused; diag_hw_write(mmio(reg),value);
}
static u32 odm_read_1byte(struct PHY_DM_STRUCT *unused, u32 reg)
{
    (void)unused; return compat_read8(mmio(reg));
}
static void odm_write_1byte(struct PHY_DM_STRUCT *unused, u32 reg, u32 value)
{
    (void)unused; compat_write8(mmio(reg),value);
}
static u32 odm_get_bb_reg(struct PHY_DM_STRUCT *d, u32 reg, u32 mask)
{
    return (odm_read_4byte(d,reg) & mask) >> shift(mask);
}
static void odm_set_bb_reg(struct PHY_DM_STRUCT *d, u32 reg, u32 mask, u32 value)
{
    if (mask != MASKDWORD)
        value = (odm_read_4byte(d,reg) & ~mask) | ((value << shift(mask)) & mask);
    odm_write_4byte(d,reg,value);
}
static void delay_us(u32 us)
{
    u32 start = foundation_now_us();
    for (u32 spins = 0; spins < 2000000; ++spins) {
        if ((u32)(foundation_now_us() - start) >= us) return;
        ++g_diag.progress;
        diag_mailbox_poll();
        foundation_poll();
    }
    access_error |= 1;
}
static void ODM_delay_ms(u32 ms)
{
    while (ms--) delay_us(1000);
}
static u32 odm_get_rf_reg(struct PHY_DM_STRUCT *d, u32 path, u32 reg, u32 mask)
{
    if (path) { access_error |= 2; return 0; }
    u32 value = odm_read_4byte(d,0x2800u+((reg & 255) << 2)) & RFREGOFFSETMASK;
    if (value == RFREGOFFSETMASK) access_error |= 4;
    return (value & mask) >> shift(mask);
}
static void odm_set_rf_reg(struct PHY_DM_STRUCT *d, u32 path, u32 reg, u32 mask, u32 value)
{
    if (path) { access_error |= 2; return; }
    if (mask != RFREGOFFSETMASK)
        value = (odm_get_rf_reg(d,path,reg,RFREGOFFSETMASK) & ~mask) | ((value << shift(mask)) & mask);
    odm_write_4byte(d,0xc90,((reg & 255) << 20) | (value & RFREGOFFSETMASK));
    delay_us(13); /* rtw8821c SIPI write settling time */
}
static u32 odm_get_current_time(struct PHY_DM_STRUCT *unused)
{
    (void)unused;return foundation_now_us();
}
static u32 odm_get_progressing_time(struct PHY_DM_STRUCT *unused,u32 start)
{
    (void)unused;
    u32 elapsed = foundation_now_us()-start, quotient = 0, remainder = 0;
    /* Avoid the unqualified compressed DIVU instruction. */
    for (u32 bit = 32; bit; --bit) {
        remainder = (remainder << 1) | ((elapsed >> (bit-1)) & 1);
        quotient <<= 1;
        if (remainder >= 1000) { remainder -= 1000; quotient |= 1; }
    }
    return quotient;
}
#include "iqk_vendor.inc"

/* Decode the host's completed channel setup, rather than selecting channels in
 * firmware. RF18 masks/values match rtw8821c.h; PHYDM bandwidth enums are
 * 0/1/2 for 20/40/80 MHz. Center-channel geometry is bounded to the channels
 * exposed by this rtw88 reference. Linux retains regulatory/DFS/primary-channel
 * policy. Accepting a configuration here is not RF qualification of that mode.
 */
static bool iqk_channel_geometry(u32 channel, u32 width, u32 is_5g)
{
    if (!is_5g) {
        if (width == 0) return channel >= 1 && channel <= 14;
        return width == 1 && channel >= 3 && channel <= 11;
    }
    if (width == 0)
        return ((channel >= 36 && channel <= 64) && !(channel & 3)) ||
               ((channel >= 100 && channel <= 144) && !(channel & 3)) ||
               ((channel >= 149 && channel <= 165) && ((channel & 3) == 1));
    if (width == 1)
        return ((channel >= 38 && channel <= 62) && ((channel & 7) == 6)) ||
               ((channel >= 102 && channel <= 142) && ((channel & 7) == 6)) ||
               channel == 151 || channel == 159;
    return channel == 42 || channel == 58 || channel == 106 ||
           channel == 122 || channel == 138 || channel == 155;
}

static bool iqk_decode_channel(u32 rf18)
{
    u32 channel = rf18 & 255;
    u32 band_bits = rf18 & 0x10300;
    u32 width_bits = rf18 & 0xc00;
    if (band_bits != 0 && band_bits != 0x10100) return false;
    band = band_bits ? ODM_BAND_5G : ODM_BAND_2_4G;
    if (width_bits == 0xc00) bandwidth = 0;
    else if (width_bits == 0x800) bandwidth = 1;
    else if (width_bits == 0x400) bandwidth = 2;
    else return false;
    if (!iqk_channel_geometry(channel, bandwidth, band)) return false;
    u32 rfsi = !band || channel < 100 ? 0 : channel <= 140 ? 0x20000 : 0x40000;
    if ((rf18 & 0x60000) != rfsi) return false;
    /* 5/10 MHz reuse RF18's 20-MHz value; their distinct ADCCLK bits must not
     * be silently calibrated as 20 MHz. Check RF/BB width agreement as well.
     */
    u32 adc_clock = odm_read_4byte(&dm, 0x8ac);
    if ((adc_clock & 0xc0) || (adc_clock & 3) != bandwidth) return false;
    /* A 5-GHz request must already use the WLA frontend, not BTG. */
    if (band && (odm_read_4byte(&dm, 0xcb8) & BIT(16))) return false;
    return true;
}

u32 station_iqk(u32 clear)
{
    access_error = 0;
    u32 channel = odm_get_rf_reg(&dm,0,0x18,RFREGOFFSETMASK);
    g_broader.iqk_rf18 = channel;
    g_broader.iqk_adc_clock = odm_read_4byte(&dm,0x8ac);
    g_broader.iqk_frontend = odm_read_4byte(&dm,0xcb8);
    if (!iqk_decode_channel(channel)) return access_error | 8;
    dm.p_band_type = &band; dm.p_band_width = &bandwidth;
    dm.support_interface = 2; dm.cut_version = g_diag.chip.cut;
    dm.is_linked = false;
    _iq_calibrate_8821c_init(&dm);
    _phy_iq_calibrate_8821c(&dm,!!clear);
    if (access_error) {
        /* Do not reload partial/timed-out results as a later calibration. */
        dm.IQK_info.iqk_channel[0] = 0;
        dm.IQK_info.iqk_channel[1] = 0;
    }
    if (!access_error) {
        /* Completion, not a success claim: 1BF0 retains actual TX/RX failure
         * bits for the host. The real calibration and register restoration
         * above must finish before the RF_DTXLOK completion marker is written.
         */
        odm_set_rf_reg(&dm,0,8,RFREGOFFSETMASK,0xabcde);
    }
    return access_error;
}

/* RF doze toggles BB reset without changing the host's RF18 channel key.
 * Restore only an existing successful calibration. A cache miss must never
 * fall into a long calibration while a beacon wake deadline is approaching.
 */
u32 station_iqk_doze_restore(void)
{
    access_error = 0;
    u32 channel = odm_get_rf_reg(&dm,0,0x18,RFREGOFFSETMASK);
    ++g_deep_iqk.attempts;g_deep_iqk.rf18=channel;
    g_deep_iqk.adc=odm_read_4byte(&dm,0x8ac);
    g_deep_iqk.frontend=odm_read_4byte(&dm,0xcb8);
    g_deep_iqk.completed=g_station.iqk_completed;
    g_deep_iqk.key0=dm.IQK_info.iqk_channel[0];g_deep_iqk.key1=dm.IQK_info.iqk_channel[1];
    g_deep_iqk.fail0=dm.IQK_info.IQK_fail_report[0][0][0]|(dm.IQK_info.IQK_fail_report[0][0][1]<<1);
    g_deep_iqk.fail1=dm.IQK_info.IQK_fail_report[1][0][0]|(dm.IQK_info.IQK_fail_report[1][0][1]<<1);
    g_deep_iqk.result=access_error|64u;
    if (!g_station.iqk_completed || !iqk_decode_channel(channel)) return access_error | 64u;
    uint32_t selected = 2;
    for (uint32_t i = 0; i < 2; ++i)
        if (channel && dm.IQK_info.iqk_channel[i] == channel &&
            !dm.IQK_info.IQK_fail_report[i][0][0] &&
            !dm.IQK_info.IQK_fail_report[i][0][1]) { selected = i; break; }
    if (selected == 2) return access_error | 64u;
    /* The general reload helper visits every matching key. Select exactly
     * one successful slot so a duplicate failed slot cannot overwrite it.
     */
    _iqk_reload_iqk_setting_8821c(&dm,selected,2);
    _iqk_fill_iqk_report_8821c(&dm,selected);
    g_deep_iqk.result=access_error;
    return access_error;
}
