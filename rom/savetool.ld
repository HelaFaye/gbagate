/*
 * savetool.ld -- the Save Tool blob, run where it is loaded: SVT_BASE in
 * EWRAM (rom/savetool.h), the control block first. Nothing is relocated or
 * zeroed at start: the board writes the whole image, .bss included as zeros.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(svt_main)

SECTIONS {
    . = 0x02001000;
    .text : {
        KEEP(*(.svt_head))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
    }
    /* below the save buffer */
    ASSERT(. <= 0x02010000, "the Save Tool blob runs into its save buffer")
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.ARM.attributes) }
}
