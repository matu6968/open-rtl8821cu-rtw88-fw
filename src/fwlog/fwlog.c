/* SPDX-License-Identifier: GPL-2.0-only */
#include "fwlog.h"
#include "../station/station.h"
#include "../broader/broader.h"
#include <stdarg.h>

#define CHUNK_BYTES 11u
#define MESSAGE_BYTES 55u
#define QUEUE_SIZE 32u
#define POLL_INTERVAL_US 1000u

struct log_message {
    uint8_t text[MESSAGE_BYTES];
    uint8_t bytes;
    uint8_t sequence;
    uint8_t fragments_left;
} __attribute__((aligned(64))); /* Keep queue indexing to a MIPS16 shift. */

/* All producers and the drain run in the station foreground loop. */
static struct log_message queue[QUEUE_SIZE];
static uint32_t read_index, queued, component_mask, next_poll;
static uint32_t sequence, offset, transport_started;

uint32_t fw_log_h2c(uint32_t lo, uint32_t hi)
{
    if ((lo & 255u) != 0x49u) return 0;
    /* Vendor PHYDM 3081 command: enable, LE32 components, mode, MACID. */
    component_mask = ((lo >> 8) & 255u) ? (lo >> 16) | (hi << 16) : 0;
    return 1;
}

void fw_log(uint32_t component, const char *format, ...)
{
    static const char hex[] = "0123456789ABCDEF";
    if (component && !(component_mask & component)) return;

    /* Stock consumes a four-bit message sequence even if transport loses it. */
    sequence = (sequence + 1) & 15u;
    if (queued == QUEUE_SIZE) return;
    struct log_message *message = &queue[(read_index + queued) & (QUEUE_SIZE - 1)];
    uint32_t bytes = 0, fragments = 0;
    va_list args;
    va_start(args, format);
    while (*format && bytes < MESSAGE_BYTES) {
        if (*format != '%') {
            message->text[bytes++] = *format++;
            continue;
        }
        ++format;
        if (!*format) break;
        uint32_t digits = 8;
        uint32_t value = va_arg(args, uint32_t);
        if ((*format == 'b' || *format == 'w') &&
            (format[1] == 'x' || format[1] == 'X')) {
            digits = *format++ == 'b' ? 2 : 4;
        }
        /* The stock formatter consumes an argument for unsupported conversions
         * and leaves the conversion letter in the literal text. */
        if (*format != 'x' && *format != 'X') continue;
        uint32_t lower = *format++ == 'x' ? 0x20u : 0;
        while (digits && bytes < MESSAGE_BYTES) {
            --digits;
            message->text[bytes++] = hex[(value >> (digits << 2)) & 15u] | lower;
        }
    }
    va_end(args);

    for (uint32_t left = bytes; left >= CHUNK_BYTES; left -= CHUNK_BYTES)
        ++fragments;
    /* Exact multiples of 11 end with a header-only fragment. */
    message->bytes = bytes;
    message->sequence = sequence;
    message->fragments_left = fragments;
    ++queued;
}

void fw_log_transport_start(void)
{
    if (!transport_started && station_transport_active()) {
        transport_started = 1;
        next_poll = foundation_now_us();
    }
}

void fw_log_transport_lost(void)
{
    transport_started = 0;
    offset = queued = read_index = 0;
}

void fw_log_poll(void)
{
    if (!transport_started) return;
    if (!station_transport_active()) {
        fw_log_transport_lost();
        return;
    }
    if (!queued) return;
    uint32_t now = foundation_now_us();
    if ((int32_t)(now - next_poll) < 0) return;
    next_poll = now + POLL_INTERVAL_US;

    struct log_message *message = &queue[read_index];
    uint32_t bytes = message->fragments_left ? CHUNK_BYTES : message->bytes - offset;
    uint8_t frame[3 + CHUNK_BYTES];
    frame[0] = 0x00;
    frame[1] = g_station.c2h_seq;
    frame[2] = (message->sequence << 4) | message->fragments_left;
    for (uint32_t i = 0; i < bytes; ++i) frame[3 + i] = message->text[offset + i];
    if (!station_c2h_send(frame, 3 + bytes)) return;
    ++g_station.c2h_seq;
    if (message->fragments_left) {
        --message->fragments_left;
        offset += CHUNK_BYTES;
    } else {
        read_index = (read_index + 1) & (QUEUE_SIZE - 1);
        --queued;
        offset = 0;
    }
}
