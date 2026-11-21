/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "compat/compat.h"
#include "fwlog/fwlog.h"
#include "station/station.h"

static uint8_t efuse[EFUSE_SIZE];
static uint32_t efuse_offset, efuse_timeout, unexpected_writes;
static uint32_t now, active, busy, frame_count, iqk_errors, iqk_report;
static uint8_t frames[64][14], lengths[64];

volatile struct station_status g_station;
volatile struct diag_status g_diag;

uint32_t diag_hw_read(uint32_t address)
{
    if (address == 0xb8600030u)
        return efuse_timeout ? 0 : 0x80000000u | efuse[efuse_offset];
    if (address == 0xb8641bf0u)
        return iqk_report;
    return 0;
}

void diag_hw_write(uint32_t address, uint32_t value)
{
    if (address == 0xb8600034u && !(value & 0x300u))
        return;
    if (address == 0xb8600030u && !(value & 0x80000000u)) {
        efuse_offset = (value >> 8) & 511u;
        return;
    }
    ++unexpected_writes;
}

uint32_t compat_read8(uint32_t address)
{
    (void)address;
    return 0;
}

uint32_t compat_read16(uint32_t address)
{
    return compat_read8(address);
}

void compat_write8(uint32_t address, uint32_t value)
{
    if (address != 0xb8600037u || value & 0x80u)
        ++unexpected_writes;
}

void compat_write16(uint32_t address, uint32_t value)
{
    if (address != 0xb8600080u || value != 0x8000u)
        ++unexpected_writes;
}

void diag_emit(uint32_t event, uint32_t arg0, uint32_t arg1)
{
    (void)event;
    (void)arg0;
    (void)arg1;
}

uint32_t radio_owned(void)
{
    return 0;
}

uint32_t broader_register_h2c(uint32_t lo, uint32_t hi)
{
    return fw_log_h2c(lo, hi);
}

uint32_t station_transport_active(void)
{
    return active;
}

uint32_t foundation_now_us(void)
{
    return now;
}

uint32_t station_c2h_send(const uint8_t *frame, uint32_t length)
{
    if (busy || frame_count >= 64 || length > sizeof(frames[0]))
        return 0;
    memcpy(frames[frame_count], frame, length);
    lengths[frame_count++] = length;
    return 1;
}

uint32_t station_iqk(uint32_t clear)
{
    (void)clear;
    return iqk_errors;
}

void station_tx_report_start(void)
{
}

void model_reset(void)
{
    fw_log_transport_lost();
    fw_log_h2c(0x49, 0);
    memset((void *)&g_station, 0, sizeof(g_station));
    memset((void *)&g_diag, 0, sizeof(g_diag));
    memset((void *)&g_compat, 0, sizeof(g_compat));
    g_station.c2h_seq = 254;
    efuse_timeout = unexpected_writes = 0;
    now = active = busy = frame_count = iqk_errors = iqk_report = 0;
}

void model_boot(uint32_t board, uint32_t bt_function, uint32_t valid, uint32_t timeout)
{
    const uint8_t packets[] = {
        0x0e, 0x29, valid ? 0x81 : 0xff,    /* ID at logical 0x00. */
        0x0f, 0x3e, 0xff, board,            /* Board option at 0xc1. */
        0x2f, 0x3d, 2, 0xff,               /* RFE option at 0xca. */
    };
    memset(efuse, 0xff, sizeof(efuse));
    memcpy(efuse, packets, sizeof(packets));
    efuse_timeout = timeout;
    diag_decode_chip(&g_diag.chip, 0, 0, 4u | (bt_function << 18));
    compat_init();
}

uint32_t model_ready(void)
{
    return g_compat.ready;
}

uint32_t model_bad_writes(void)
{
    return unexpected_writes;
}

void model_iqk(uint32_t errors, uint32_t report)
{
    const uint32_t packet[8] = {0x0eff01, 9};
    iqk_errors = errors;
    iqk_report = report;
    active = 1;
    station_packet(packet);
}

void model_poll(void)
{
    fw_log_poll();
    now += 1000;
}

void model_set_busy(uint32_t value)
{
    busy = value;
}

void model_emit(const char *text)
{
    fw_log(FW_LOG_ALWAYS, text);
}

void model_rate_trace(void)
{
    fw_log(FW_LOG_RA, "Rate: increased");
}

uint32_t model_frame_count(void)
{
    return frame_count;
}

uint32_t model_copy_frame(uint32_t index, uint8_t *out)
{
    memcpy(out, frames[index], lengths[index]);
    return lengths[index];
}
