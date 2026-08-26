/*
 * multiboot.ld -- link a GBA multiboot image.
 *
 * The counterpart to tester.ld. There, the image lives in the cartridge at
 * 0x08000000 and only .data is copied down into RAM. Here the *whole* image is
 * delivered into EWRAM by the BIOS download routine, so code, rodata and data
 * all have their run addresses inside the 256 KB at 0x02000000 and nothing
 * needs relocating at startup.
 *
 * Three constraints are enforced here at link time rather than left to a
 * comment, because each one fails silently at the far end if it is wrong:
 *
 *   .data LMA == VMA        crt0_mb.s deliberately has no .data copy loop, on
 *                           the grounds that the BIOS already put .data where
 *                           it belongs. If a future edit gives .data a load
 *                           address of its own, that assumption breaks and the
 *                           program starts with uninitialised globals.
 *
 *   image below 0x0203FF00  tester.c publishes its result block there. An
 *                           image large enough to reach it would overwrite its
 *                           own verdict, and the only symptom would be a
 *                           tester that reports nonsense.
 *
 *   .bss below 0x0203FF00   same address, same reason, but .bss is NOLOAD and
 *                           so does not show up in the image size at all.
 *
 * The file-size limits (MB_MIN_SIZE 0x1C0 and MB_MAX_SIZE 0x40000, both in
 * src/gba_multiboot.h) are checked by rom/Makefile on the finished binary
 * instead: they are properties of the file on disk, not of the link.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
}

__stack_top   = ORIGIN(iwram) + LENGTH(iwram) - 0x100;

/* Must match RESULT_ADDR in tester.c. */
__result_addr = 0x0203FF00;

SECTIONS {
    .text : {
        KEEP(*(.gba_crt0))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > ewram

    .data : {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > ewram
    __data_lma__ = LOADADDR(.data);

    /*
     * The last loadable bytes in the image, and never zero. See crt0_mb.s:
     * rom_effective_size() in src/rom_catalog.c strips trailing 0x00/0xFF
     * before transferring, so an image that happened to end in alignment
     * padding would be sent short.
     */
    .mbfoot : {
        . = ALIGN(4);
        KEEP(*(.mbfoot))
        __image_end__ = .;
    } > ewram

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

ASSERT(__data_lma__ == __data_start__,
       "multiboot .data must load at its run address (crt0_mb.s does not copy it)")
ASSERT(__image_end__ <= __result_addr,
       "multiboot image reaches the result block at 0x0203FF00")
ASSERT(__bss_end__ <= __result_addr,
       "multiboot .bss reaches the result block at 0x0203FF00")
