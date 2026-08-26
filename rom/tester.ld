/*
 * tester.ld -- link a GBA cartridge image.
 *
 * Code and read-only data execute from the cartridge at 0x08000000. Writable
 * data lives in EWRAM (256 KB at 0x02000000) and is copied there by crt0.s; the
 * stack is at the top of IWRAM (32 KB at 0x03000000). The result signature that
 * tester.c publishes sits high in EWRAM (0x0203FF00), well clear of .data/.bss.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
}

__stack_top = ORIGIN(iwram) + LENGTH(iwram) - 0x100;

SECTIONS {
    .text : {
        KEEP(*(.gba_crt0))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom

    .data : {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > ewram AT > rom
    __data_lma__ = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > ewram

    /DISCARD/ : { *(.comment) *(.note.*) *(.ARM.attributes) }
}
