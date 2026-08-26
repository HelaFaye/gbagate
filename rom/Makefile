# rom/Makefile -- build the Battle Chip Gate tester ROM.
#
#   make                 build tester.gba (needs a bare arm-none-eabi toolchain)
#   make mb              build tester.mb, the same tester linked for multiboot
#   make DONOR=game.gba  fix the header's logo + complement from a ROM you own
#   make clean
#
# `mb` is deliberately not part of `all`. The cartridge image is the one the
# QEMU harness uses and it is known to build; keeping the newer multiboot link
# on its own target means a mistake there cannot break the working path.
#
# No devkitPro required: a plain arm-none-eabi-gcc / objcopy is enough. The
# header is finished with the repo's own tools/fix_header.py, so no second
# header tool is introduced. A donor is optional -- mGBA runs the ROM without a
# valid Nintendo logo -- but if one is given (or found in .reference), the logo
# and complement byte are made correct too.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

ROOT    := ..
FIXHDR  := $(ROOT)/tools/fix_header.py

# A ROM you own, to copy Nintendo's logo bytes from. Defaults to the reference
# dump if it is present; override with `make DONOR=/path/to/rom.gba`.
DONOR ?= $(firstword $(wildcard $(ROOT)/.reference/*.gba))

ARCH    := -mcpu=arm7tdmi -marm
CFLAGS  := $(ARCH) -O2 -ffreestanding -fno-builtin -std=c11 \
           -Wall -Wextra -I.
LDFLAGS := $(ARCH) -nostdlib -nostartfiles -T tester.ld \
           -Wl,--gc-sections -Wl,-Map,tester.map

# The multiboot image runs from EWRAM rather than the cartridge, so it needs
# its own linker script and its own startup: multiboot header entries at
# 0xC0..0xE3, and no .data copy. tester.o is shared between the two links --
# the C is identical and only the addresses differ.
MBLDFLAGS := $(ARCH) -nostdlib -nostartfiles -T multiboot.ld \
             -Wl,--gc-sections -Wl,-Map,tester_mb.map

OBJS    := crt0.o tester.o
MB_OBJS := crt0_mb.o tester.o

.PHONY: all mb clean fixheader fixheader-mb
all: tester.gba
mb: tester.mb

crt0.o: crt0.s
	$(CC) $(ARCH) -c -o $@ $<

tester.o: tester.c handshake.h
	$(CC) $(CFLAGS) -c -o $@ $<

tester.elf: $(OBJS) tester.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

tester.gba: tester.elf
	$(OBJCOPY) -O binary $< $@
	@wc -c < $@ | awk '{printf "tester.gba: %d bytes\n", $$1}'
	@$(MAKE) --no-print-directory fixheader

# ---- multiboot image ----------------------------------------------------

crt0_mb.o: crt0_mb.s
	$(CC) $(ARCH) -c -o $@ $<

tester_mb.elf: $(MB_OBJS) multiboot.ld
	$(CC) $(MBLDFLAGS) -o $@ $(MB_OBJS)

# The size bounds mirror MB_MIN_SIZE (0x1C0) and MB_MAX_SIZE (0x40000) in
# src/gba_multiboot.h, which are in turn GBATEK's payload range for SWI 25h
# (100h..3FF40h, excluding the 0xC0 header). An image outside them is one that
# mb_send() would refuse with MB_ERR_ARGS and rom_validate() would mark
# ROM_BAD_SIZE, so it is worth failing the build here rather than discovering
# it on the device.
tester.mb: tester_mb.elf
	$(OBJCOPY) -O binary $< $@
	@sz=`wc -c < $@`; \
	 printf 'tester.mb: %s bytes\n' "$$sz"; \
	 if [ "$$sz" -lt 448 ]; then \
	     echo "tester.mb: smaller than MB_MIN_SIZE (0x1C0 = 448 bytes)"; \
	     exit 1; \
	 fi; \
	 if [ "$$sz" -gt 262144 ]; then \
	     echo "tester.mb: larger than EWRAM (0x40000 = 262144 bytes)"; \
	     exit 1; \
	 fi
	@$(MAKE) --no-print-directory fixheader-mb
	@python3 $(ROOT)/tools/check_multiboot.py $@ || true

# Fix the header complement (always) and the logo (if a donor is available).
fixheader: tester.gba
	@if [ -n "$(DONOR)" ] && [ -f "$(DONOR)" ]; then \
	    echo "fixing header using donor: $(DONOR)"; \
	    python3 $(FIXHDR) --donor "$(DONOR)" --in-place tester.gba || true; \
	else \
	    echo "no donor ROM found; fixing complement only (logo left blank)"; \
	    echo "  (mGBA runs the ROM regardless; pass DONOR=you-own.gba for a valid logo)"; \
	    python3 $(FIXHDR) --in-place tester.gba || true; \
	fi
	@python3 $(FIXHDR) --check tester.gba || true

# The same repair for the multiboot image. Spelled out rather than shared with
# the target above: the recipes differ in the one place that matters, and a
# clever pattern rule here would be harder to check than nine duplicated lines.
#
# The difference is the warning. A blank logo is genuinely harmless for
# tester.gba -- mGBA runs it either way -- but it is fatal for tester.mb on real
# hardware, because the console verifies the logo itself during a multiboot
# download and rejects the image *after* the whole transfer, with no
# diagnostic. The build still succeeds without a donor so that the image can be
# used against emulators and against the simulated GBA in
# test/test_multiboot.c, but it should not go near a console like that.
fixheader-mb: tester.mb
	@if [ -n "$(DONOR)" ] && [ -f "$(DONOR)" ]; then \
	    echo "fixing header using donor: $(DONOR)"; \
	    python3 $(FIXHDR) --donor "$(DONOR)" --in-place tester.mb || true; \
	else \
	    echo "no donor ROM found; fixing complement only (logo left blank)"; \
	    echo "  WARNING: a real GBA rejects a multiboot image with a blank"; \
	    echo "  logo, after the entire transfer and without saying why."; \
	    echo "  Pass DONOR=you-own.gba before using this on hardware."; \
	    python3 $(FIXHDR) --in-place tester.mb || true; \
	fi
	@python3 $(FIXHDR) --check tester.mb || true

clean:
	rm -f $(OBJS) crt0_mb.o tester.elf tester_mb.elf tester.gba tester.mb \
	      tester.gba.bak tester.mb.bak tester.map tester_mb.map
