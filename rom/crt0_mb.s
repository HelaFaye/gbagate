@ crt0_mb.s -- startup and header for the multiboot (EWRAM) image.
@
@ The multiboot twin of crt0.s. They are two files rather than one file with
@ #ifdefs because crt0.s is assembled by a plain assembler with no C
@ preprocessor: switching it to -x assembler-with-cpp would make cpp read the
@ apostrophes in the prose comments as unterminated character constants, and
@ the cartridge path currently works.
@
@ Three things differ from crt0.s.
@
@  1. The multiboot header entries at 0xC0..0xE3 exist. The BIOS download
@     routine copies the whole image to 0x02000000 and jumps to 0x020000C0, so
@     the branch at 0xC0 -- not the one at 0x00 -- is the one that actually
@     runs here. The entry at 0x00 is still a valid ARM branch because both
@     tools/fix_header.py and rom_validate() in src/rom_catalog.c check that
@     byte 3 is 0xEA before they will accept the file.
@
@  2. Bytes 0xC4 and 0xC5 are initialised to zero and then *overwritten by the
@     BIOS* with the boot mode and the slave ID. That is what lets tester.c
@     tell a genuine multiboot download from an ordinary cartridge boot, which
@     is the one thing this image can prove that tester.gba cannot.
@
@  3. There is no .data copy loop. multiboot.ld links .data at its run address,
@     so the BIOS has already placed it correctly; the linker script asserts
@     LMA == VMA rather than trusting this comment to stay true.
@
@ The 156-byte Nintendo logo at 0x04 is left zeroed, exactly as in crt0.s. It
@ is Nintendo's artwork and does not ship in this tree; rom/Makefile fills it
@ in from a ROM you own via tools/fix_header.py --donor. Unlike the cartridge
@ case, a blank logo is fatal here: the console verifies the logo itself during
@ a multiboot download, so a real GBA rejects the image after the whole
@ transfer has completed, with no diagnostic.

    .section .gba_crt0, "ax"
    .arm
    .align 2
    .global _start
_start:
    b       .Lstart                 @ 0x00: ARM branch; byte 3 is 0xEA
    .space  156, 0                  @ 0x04: Nintendo logo (donor fills)
    .byte   'G','B','A','G','A','T','E','M','B',0,0,0  @ 0xA0: title (12)
    .byte   'C','G','M','B'         @ 0xAC: game code (4)
    .byte   '0','0'                 @ 0xB0: maker code (2)
    .byte   0x96                    @ 0xB2: fixed value
    .byte   0x00                    @ 0xB3: main unit code
    .byte   0x00                    @ 0xB4: device type
    .space  7, 0                    @ 0xB5: reserved (7)
    .byte   0x00                    @ 0xBC: software version
    .byte   0x00                    @ 0xBD: complement (fix_header computes)
    .space  2, 0                    @ 0xBE: reserved (2)

    @ ---- additional multiboot header entries ---------------------------
    b       .Lstart                 @ 0xC0: RAM entry point; the BIOS jumps here
    .byte   0x00                    @ 0xC4: boot mode  -- BIOS overwrites this
    .byte   0x00                    @ 0xC5: slave ID   -- BIOS overwrites this
    .space  26, 0                   @ 0xC6: not used (26)
    b       .Lstart                 @ 0xE0: JOYBUS entry point

    @ ---- four bytes of deliberate padding ------------------------------
    @ This exists solely to move .Lstart from 0xE4 to 0xE8, and removing it
    @ makes the image undetectable as multiboot by mGBA. See the note below.
    .space  4, 0                    @ 0xE4: padding -- do not delete

    .align 2
.Lstart:                            @ 0xE8, and it must not be 0xE4

@ Why .Lstart is at 0xE8 and not at 0xE4
@ -------------------------------------
@ mGBA decides whether a file is a multiboot image or a cartridge by decoding
@ the word at 0xC0 and looking at the branch displacement -- GBAIsMB() in
@ src/gba/gba.c. Three displacements are rejected:
@
@     if (info.op1.immediate <= 0)  return false;
@     else if (info.op1.immediate == 28) return false;  // "ancient toolchain"
@     else if (info.op1.immediate != 24) return true;
@
@ The displacement is in bytes and excludes the ARM pipeline's +8, because the
@ decoder computes it as (opcode << 8) >> 6 (src/arm/decoder-arm.c).
@
@ With .Lstart at 0xE4 the branch at 0xC0 assembles to 0xEA000007, whose
@ displacement is 0xE4 - 0xC0 - 8 = 28 -- the value mGBA rejects outright. The
@ file would then be loaded by GBALoadROM() at 0x08000000 instead of by
@ GBALoadMB() into EWRAM, and code linked for 0x02000000 does not run there.
@ The symptom is a black screen with no error, because nothing in the chain
@ considers the load to have failed.
@
@ At 0xE8 the displacement is 32, which is none of the three rejected values.
@ tools/check_multiboot.py re-implements the same test so this cannot regress
@ silently, and multiboot.ld is indifferent to where the label lands.
    @ Interrupts stay off; the tester polls VCOUNT rather than using IRQs.
    ldr     sp, =__stack_top

    @ Clear .bss. It is NOLOAD, so it is not part of the transferred image and
    @ EWRAM holds whatever the BIOS happened to leave there.
    ldr     r0, =__bss_start__
    ldr     r1, =__bss_end__
    mov     r2, #0
.Lbss:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     .Lbss

    @ No .data copy: multiboot.ld links .data at its run address and asserts
    @ that it did. See point (3) above.

    ldr     r0, =main
    bx      r0                      @ into main() (ARM)
.Lhang:
    b       .Lhang                  @ main() does not return, but just in case

@ ---- image footer ------------------------------------------------------
@ Placed last and kept by multiboot.ld. The bytes are 'G','B','M','B', so the
@ final byte of the image is 0x42 and never 0x00 or 0xFF -- which is what stops
@ rom_effective_size() in src/rom_catalog.c from stripping real content as if
@ it were cartridge padding. tools/check_multiboot.py verifies it is present.

    .section .mbfoot, "a"
    .align 2
    .word   0x424D4247              @ "GBMB", little-endian
