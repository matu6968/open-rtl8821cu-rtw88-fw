/* SPDX-License-Identifier: GPL-2.0-only */
#include "diag.h"

static volatile uint32_t initialized_word = 0x31415926u;
static volatile uint32_t zero_words[4];
static volatile uint32_t arguments[6] = {0x1234, 0x55, 0x789, 0xab, 0xcdef, 0x2468};
volatile uint32_t g_abi_results[6];

__attribute__((noinline, noclone))
static uint32_t six_arguments(uint32_t a, uint32_t b, uint32_t c,
                              uint32_t d, uint32_t e, uint32_t f)
{
    return (((a + b) ^ c) + d) ^ e ^ f;
}

__attribute__((noinline, noclone))
static uint32_t nested_arguments(uint32_t seed)
{
    uint32_t saved = seed + 0x11111111u;
    uint32_t result = six_arguments(arguments[0], arguments[1], arguments[2],
                                   arguments[3], arguments[4], arguments[5]);
    return result ^ saved;
}

uint32_t diag_abi_test(void)
{
    uint32_t passed = 0;
    volatile union { uint32_t word; uint16_t halves[2]; uint8_t bytes[4]; } widths;
    volatile uint32_t shift_input = 0xfedcba98u;
    volatile int32_t signed_input = -123456;
    g_abi_results[0] = initialized_word;
    if (initialized_word == 0x31415926u) {
        initialized_word ^= 0x01010101u;
        if (initialized_word == 0x30405827u)
            passed |= 1;
    }
    g_abi_results[1] = zero_words[0] | zero_words[1] | zero_words[2] | zero_words[3];
    if (!g_abi_results[1])
        passed |= 2;
    widths.word = 0x12345678u;
    if (widths.bytes[0] == 0x78 && widths.halves[1] == 0x1234) {
        widths.bytes[1] = 0xa5;
        widths.halves[1] = 0xbeef;
        if (widths.word == 0xbeefa578u)
            passed |= 4;
    }
    g_abi_results[2] = widths.word;
    g_abi_results[3] = nested_arguments(0x12345678u);
    if (g_abi_results[3] == (0xfc2cu ^ (0x12345678u + 0x11111111u)))
        passed |= 8;
    g_abi_results[4] = (shift_input >> 9) ^ (shift_input << 7);
    if (g_abi_results[4] == 0x6e22225du && signed_input < 0 &&
        (uint32_t)(signed_input >> 3) == 0xffffc3b8u)
        passed |= 16;
    g_abi_results[5] = diag_read_sp();
    if (!(g_abi_results[5] & 7) && g_abi_results[5] > DIAG_STACK_BOTTOM + 32 &&
        g_abi_results[5] <= DIAG_STACK_TOP)
        passed |= 32;
    return passed;
}
