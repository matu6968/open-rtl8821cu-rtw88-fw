/* SPDX-License-Identifier: GPL-2.0-only */
/* Keep retention probes outside all live data/BSS and the qualified stack. */
INCLUDE src/diagnostic/linker.ld
ASSERT(__bss_end <= 0x80203000, "deep canary overlaps BSS")
ASSERT(__stack_bottom == 0x80205000, "deep bank probe overlaps stack")
ASSERT(__stack_top == 0x80206000, "deep bank probe overlaps stack")
