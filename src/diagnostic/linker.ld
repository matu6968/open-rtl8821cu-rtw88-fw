/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY {
    IMEM (rx) : ORIGIN = 0x80030000, LENGTH = 0xf708
    DATA (rw) : ORIGIN = 0x80200200, LENGTH = 0x4e00
}
__stack_bottom = 0x80205000;
__stack_top = 0x80206000;
SECTIONS {
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > IMEM
    .diag_layout ALIGN(4) : { KEEP(*(.diag_layout)) } > IMEM
    .data : {
        __data_start = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > DATA
    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DATA
    ASSERT(__data_start >= 0x80200200, "ROM handoff prefix overlap")
    ASSERT(__bss_end <= __stack_bottom, "data/BSS overlaps stack")
    ASSERT(__stack_top <= 0x80207ce8, "stack exceeds proven downloaded DMEM extent")
    /DISCARD/ : {
        *(.reginfo .MIPS.abiflags .MIPS.stubs .pdr .comment .gnu.attributes)
        *(.mdebug* .note* .eh_frame*)
    }
}
