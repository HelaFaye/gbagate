@ crt0.s -- minimal GBA cartridge startup and header.
@
@ Deliberately toolchain-agnostic: it needs only a bare arm-none-eabi assembler
@ and linker, no devkitPro crt0 or specs file. It lays out the 192-byte GBA
@ header, then sets up the stack, clears .bss, copies .data into RAM and calls
@ main().
@
@ The 156-byte Nintendo logo at 0x04 is left zeroed here -- it is Nintendo's
@ copyrighted artwork and does not ship in this tree. rom/Makefile fills it in
@ (and fixes the header complement) from a ROM you own, using the repo's own
@ tools/fix_header.py --donor. mGBA runs the ROM without it either way.

    .section .gba_crt0, "ax"
    .arm
    .align 2
    .global _start
_start:
    b       .Lstart                 @ 0x00: ARM branch; byte 3 is 0xEA
    .space  156, 0                  @ 0x04: Nintendo logo (donor fills)
    .byte   'G','B','A','G','A','T','E','T','S','T',0,0  @ 0xA0: title (12)
    .byte   'C','G','T','E'         @ 0xAC: game code (4)
    .byte   '0','0'                 @ 0xB0: maker code (2)
    .byte   0x96                    @ 0xB2: fixed value
    .byte   0x00                    @ 0xB3: main unit code
    .byte   0x00                    @ 0xB4: device type
    .space  7, 0                    @ 0xB5: reserved (7)
    .byte   0x00                    @ 0xBC: software version
    .byte   0x00                    @ 0xBD: complement (fix_header computes)
    .space  2, 0                    @ 0xBE: reserved (2)

    .align 2
.Lstart:
    @ Interrupts stay off; the tester polls VCOUNT rather than using IRQs.
    ldr     sp, =__stack_top

    @ Clear .bss.
    ldr     r0, =__bss_start__
    ldr     r1, =__bss_end__
    mov     r2, #0
.Lbss:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     .Lbss

    @ Copy .data from its load address in ROM to its run address in RAM.
    ldr     r0, =__data_lma__
    ldr     r1, =__data_start__
    ldr     r3, =__data_end__
.Ldata:
    cmp     r1, r3
    ldrlo   r2, [r0], #4
    strlo   r2, [r1], #4
    blo     .Ldata

    ldr     r0, =main
    bx      r0                      @ into main() (ARM)
.Lhang:
    b       .Lhang                  @ main() does not return, but just in case
