/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <string.h>

#include "ap/ap.h"
#include "broader/broader.h"
#include "doze/doze.h"
#include "powersave/lps.h"

#define MAC 0xb8640000u
#define TX 0xb8780000u
#define TCR (MAC + 0x604u)
#define MGQ (MAC + 0x41cu)
#define TXPAUSE (MAC + 0x522u)
#define FT1ISR (MAC + 0x113cu)
#define MGQ_POLL (1u << 29)
#define STA_MEDIA_UP 0x00000101u
#define AP_PEER_UP 0x00010101u
#define AP_PEER_DOWN 0x00010001u
#define STA_NULL_PAGE 0x01000000u
#define STA_SLEEP 0x01210120u

volatile struct doze_status g_doze;
static uint8_t mac[0x2000], tx[512];
static uint32_t role, writes, media_updates, rate_commands, doze_polls, ap_logs;

static uint8_t *reg(uint32_t address)
{
    if (address >= MAC && address < MAC + sizeof(mac))
        return &mac[address - MAC];
    assert(address >= TX && address < TX + sizeof(tx));
    return &tx[address - TX];
}

uint32_t diag_hw_read(uint32_t address)
{
    uint32_t value;
    memcpy(&value, reg(address), sizeof(value));
    return value;
}

void diag_hw_write(uint32_t address, uint32_t value)
{
    ++writes;
    if (address == FT1ISR)
        value = diag_hw_read(address) & ~value;
    memcpy(reg(address), &value, sizeof(value));
}

uint32_t compat_read8(uint32_t address)
{
    if (address == MAC + 0x102u) return role;
    if (address == 0xb8600002u) return 3;
    return *reg(address);
}

uint32_t compat_read16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, reg(address), sizeof(value));
    return value;
}

void compat_write8(uint32_t address, uint32_t value)
{
    ++writes;
    *reg(address) = (uint8_t)value;
}

void compat_write16(uint32_t address, uint32_t value)
{
    ++writes;
    uint16_t word = value;
    memcpy(reg(address), &word, sizeof(word));
}

uint32_t foundation_now_us(void) { return 1000; }
uint32_t station_transport_ready(void) { return 1; }
void station_rate_init(void) {}
void station_rate_poll(void) {}
void station_doze_init(void) {}
void station_doze_poll(void) { ++doze_polls; }
uint32_t station_wowlan_owned(void) { return 0; }
uint32_t radio_owned(void) { return 0; }
void fw_log_poll(void) {}

void diag_emit(uint32_t id, uint32_t arg0, uint32_t arg1)
{
    (void)id;
    (void)arg0;
    (void)arg1;
}

void station_doze_media(uint32_t connected)
{
    ++media_updates;
    g_doze.connected = connected;
}

uint32_t station_rate_h2c(uint32_t lo, uint32_t hi)
{
    (void)hi;
    if ((lo & 255u) != 1u) return 0;
    ++rate_commands;
    return 1;
}

uint32_t deep_h2c(uint32_t lo, uint32_t hi)
{
    (void)lo;
    (void)hi;
    return 0;
}

uint32_t station_wowlan_h2c(uint32_t lo, uint32_t hi)
{
    (void)lo;
    (void)hi;
    return 0;
}

uint32_t fw_log_h2c(uint32_t lo, uint32_t hi)
{
    (void)lo;
    (void)hi;
    return 0;
}

void fw_log(uint32_t component, const char *format, ...)
{
    assert(!component);
    assert(strcmp(format, "AP: port 0 active") == 0 ||
           strcmp(format, "AP: port 0 inactive") == 0);
    ++ap_logs;
}

static void check_ap_dispatch(void)
{
    role = 3;
    assert(ap_port_active());
    assert(broader_register_h2c(0, 0));
    assert(!broader_register_h2c(STA_NULL_PAGE, 0));
    assert(!broader_register_h2c(STA_SLEEP, 0));
    assert(g_lps.commands == 0 && !g_lps.page_valid);
    assert(broader_register_h2c(AP_PEER_UP, 0));
    assert(broader_register_h2c(AP_PEER_DOWN, 0));
    assert(broader_register_h2c(STA_MEDIA_UP, 0));
    assert(rate_commands == 3 && !media_updates);

    uint32_t before = writes;
    broader_poll();
    broader_poll();
    assert(writes == before && ap_logs == 1 && !doze_polls);

    role = 2;
    broader_poll();
    assert(ap_logs == 2 && doze_polls == 1);
    assert(broader_register_h2c(STA_MEDIA_UP, 0));
    assert(media_updates == 1 && g_doze.connected);
    assert(broader_register_h2c(STA_NULL_PAGE, 0));
    assert(g_lps.page_valid);
}

static void check_pending_station_tx(void)
{
    compat_write16(MAC + 0x41au, 0x10f); /* Empty data and management queues. */
    diag_hw_write(TX + 128u, 0x00300018u);
    compat_write16(TX + 128u + 48u, 0x0148); /* Port-0 station null. */
    assert(broader_register_h2c(STA_SLEEP, 0));
    station_lps_poll();
    assert(g_lps.state == LPS_WAIT_ACK);
    assert(diag_hw_read(MGQ) & MGQ_POLL);
    assert(compat_read8(TXPAUSE) == 15);

    role = 3;
    uint32_t before = writes;
    broader_poll();
    assert(ap_logs == 3 && media_updates == 2 && !g_doze.connected);
    assert(writes == before && compat_read8(TXPAUSE) == 15);
    assert(g_lps.state == LPS_WAIT_ACK);

    diag_hw_write(MGQ, 0);
    broader_poll();
    assert(g_lps.state == LPS_ACTIVE && !g_lps.page_valid && !g_lps.ap_ps);
    assert(compat_read8(TXPAUSE) == 0 && !(diag_hw_read(TCR) & 0x80));
    assert(g_lps.null1_submissions == 1 && g_lps.null1_acks == 0);
}

int main(void)
{
    mac[0x100] = 0xc0;
    broader_init();
    check_ap_dispatch();
    check_pending_station_tx();
    return 0;
}
