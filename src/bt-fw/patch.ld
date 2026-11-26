/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_ARCH(mips)
ENTRY(bt_publish_patch_version)

SECTIONS {
    . = 0x8010a000;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.* .rodata .rodata.*)
    }
    bt_version_address = DEFINED(bt_rom1_uart) ? 0x8011303c :
                         DEFINED(bt_rom1_layout) ? 0x80112860 : ALIGN(4);
    .patch_version bt_version_address : {
        bt_patch_version = .;
        LONG(0)
        bt_patch_config = .;
    }
    ASSERT((bt_publish_patch_version & ~1) == 0x8010a000, "Bluetooth patch entry must be first")
    ASSERT(ADDR(.text) + SIZEOF(.text) <= bt_version_address, "Bluetooth patch overlaps version")
    .unexpected : { *(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(SIZEOF(.unexpected) == 0, "Bluetooth patch fragment requires no separate data or BSS")
    /DISCARD/ : {
        *(.reginfo .MIPS.abiflags .MIPS.stubs .pdr .comment .gnu.attributes)
        *(.mdebug* .note* .eh_frame*)
    }
}
