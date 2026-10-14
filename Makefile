# Host test suites. No hardware, no ESP-IDF, no PlatformIO.
#
#   make            build and run all fifteen suites
#   make strict     also at -O0, -O3 -flto and under ASan/UBSan
#   make chipdb     regenerate the packed databases
#   make clean
#
# The ESP-IDF glue in port/ is not built here: it cannot be, and pretending
# otherwise would hide which quarter of the project is untested. See AUDIT.md.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -O2 -Isrc
BUILD   := build
DB      := data/chipdb/bn6g.chdb

# suite : modules it needs
DEPS_chipgate  = chipgate.c
DEPS_link      = link_proto.c link_session.c
DEPS_bridge    = link_bridge.c link_proto.c link_session.c
DEPS_hw        = nrf24_probe.c hw_caps.c
DEPS_ui        = chip_reader.c ui_list.c
DEPS_chipdb    = chipdb.c
DEPS_encoder   = input_encoder.c ui_list.c
DEPS_panel     = st7735_panel.c
DEPS_slot      = chip_slot.c chip_reader.c
DEPS_chipview  = ui_chipview.c ui_shell.c chipdb.c hw_caps.c chipgate.c
DEPS_render    = ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c
DEPS_multiboot = gba_multiboot.c
DEPS_catalog   = rom_catalog.c gba_multiboot.c
DEPS_variant   = variant.c
DEPS_ir        = ir_cdz.c
# The pin map lives in port/esp32s3/board.h and the two boards are checked in
# separate translation units, so both sets of macros can exist in one run.
DEPS_board     =
EXTRA_board    = -Iport/esp32s3 test/test_board_supermini.c
DEPS_controls  = controls.c ui_input.c
DEPS_input     = ui_input.c
DEPS_uiset     = ui_settings.c settings.c chipgate.c ui_input.c
DEPS_settings  = settings.c chipgate.c ui_input.c
DEPS_romview   = ui_romview.c rom_catalog.c gba_multiboot.c
DEPS_mcp       = mcp23017.c chip_slot.c chip_reader.c
DEPS_mbe2e     = rom_catalog.c gba_multiboot.c

# Most suites are handed the chip database. This one takes the real tester ROM
# instead: using an actual file, with an actual unpadded length, is the point
# of it -- that is what found the alignment bug the synthetic fixtures missed.
ARG_mbe2e      = rom/tester.gba
DEPS_menu      = ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c ui_shell.c hw_caps.c chipgate.c
DEPS_modes     = app_modes.c ui_shell.c hw_caps.c chipgate.c

SUITES := chipgate link bridge hw ui chipdb encoder panel slot chipview \
          render multiboot catalog variant menu mbe2e mcp romview settings uiset ir input controls board modes

.PHONY: all strict lint clean chipdb
all: $(addprefix run-,$(SUITES))
	@./qemu/lint.sh

$(BUILD):
	@mkdir -p $(BUILD)

# One line per suite on success; on failure, the suite's full output and a
# non-zero exit.
#
# This used to be `./$< $(DB) | tail -1`. A pipeline's status is that of its LAST
# command, so `tail` -- which always succeeds -- discarded every suite's exit
# code, and `make` reported success even when a suite printed FAILURES. That was
# observed on 26 Aug: a deliberately broken build printed "multiboot FAILURES"
# and make exited 0 without an Error line. Each suite's main() does
# `return g_fail != 0`, so the status was always correct; only the pipe lost it.
define SUITE_rule
$(BUILD)/test_$(1): test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) | $(BUILD)
	@$$(CC) $$(CFLAGS) $$(EXTRA_$(1)) -o $$@ $$< $$(addprefix src/,$$(DEPS_$(1)))
run-$(1): $(BUILD)/test_$(1)
	@printf '%-14s ' $(1); \
	if ./$$< $$(ARG_$(1)) > $(BUILD)/$(1).out 2>&1; then \
	  tail -1 $(BUILD)/$(1).out; \
	else \
	  echo 'FAILURES -- full output follows'; \
	  cat $(BUILD)/$(1).out; \
	  exit 1; \
	fi
.PHONY: run-$(1)
endef
# Every suite takes the chip database unless it says otherwise.
$(foreach s,$(SUITES),$(eval ARG_$(s) ?= $(DB)))
$(foreach s,$(SUITES),$(eval $(call SUITE_rule,$(s))))

# Every suite in every configuration. The catalogue bug that only appeared at
# -O2 -- and passed under the sanitiser -- is why this target exists.
#
# BUILD and FAIL are reported separately: they used to be one column, so a suite
# that would not COMPILE under -O3 -flto was indistinguishable from one that
# compiled and failed its assertions. Output from all four configurations
# accumulates in build/s_<suite>.log, since a matrix cell cannot say why.
define STRICT_rule
strict-$(1): | $(BUILD)
	@printf '%-14s' $(1); : > $(BUILD)/s_$(1).log; \
	for cfg in "-O0" "-O2" "-O3 -flto" "-g -fsanitize=address,undefined"; do \
	  echo "=== $$$$cfg ===" >> $(BUILD)/s_$(1).log; \
	  if ! $$(CC) -std=c11 -Wall -Wextra -Wpedantic -Wshadow $$$$cfg -Isrc \
	       -o $(BUILD)/s_$(1) test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) \
	       >> $(BUILD)/s_$(1).log 2>&1; \
	  then printf ' BUILD  '; \
	  elif ./$(BUILD)/s_$(1) $$(ARG_$(1)) >> $(BUILD)/s_$(1).log 2>&1; \
	  then printf ' ok     '; else printf ' FAIL   '; fi; \
	done; echo
.PHONY: strict-$(1)
endef
$(foreach s,$(SUITES),$(eval $(call STRICT_rule,$(s))))

# Compile-check the ESP-IDF-only sources against stub headers. Cheap, and it
# covers the files no suite can reach.
lint:
	@./qemu/lint.sh

# A compile database, so an editor resolves the same includes the build does.
# Without it clangd cannot find src/*.h and reports cascading "unknown type
# uint16_t" errors that look like broken code.
compile-commands:
	@python3 tools/gen_compile_commands.py
.PHONY: compile-commands

strict: $(addprefix strict-,$(SUITES))
	@echo "-O0     -O2     -O3+lto asan/ubsan"

chipdb:
	@echo "needs the mmbn-chip-tables checkout and mGBA's res/ directory:"
	@echo "  python3 tools/build_chipdb.py <tables> data/chipdb --names <mgba>/res"

clean:
	@rm -rf $(BUILD)

# ---- QEMU ---------------------------------------------------------------
# Emulation targets. See qemu/README.md for what these do and do not prove.

.PHONY: qemu-setup qemu-build qemu-smoke qemu-proto qemu-rom-selftest \
        qemu-rom qemu-display rom rom-mb rom-mb-check rom-clean qemu-clean

qemu-setup:
	@./qemu/setup.sh

qemu-build:
	@./qemu/build.sh link && ./qemu/build.sh gate && ./qemu/build.sh full

qemu-smoke:
	@./qemu/smoke.sh

# The one QEMU-related check that needs no ESP-IDF and no emulator: that the
# broker frames packets exactly as the firmware does.
qemu-proto:
	@python3 qemu/test_lp.py
	@python3 qemu/test_gate_drive.py
	@python3 qemu/test_rom_selftest.py
	@python3 qemu/test_viewer.py

# The firmware's gate as a host process: same chipgate.c and link_proto.c,
# minus ESP-IDF. Lets the ROM/mGBA/Lua/broker chain be proven before any
# emulator or toolchain is involved.
build/gate_sim: qemu/gate_sim.c src/chipgate.c src/link_proto.c | $(BUILD)
	@$(CC) $(CFLAGS) -o $@ $^
gate-sim: build/gate_sim
	@echo "run: ./build/gate_sim 5556 bn6"
.PHONY: gate-sim

# Tool-free proof of the tester ROM's handshake logic (against the real
# chipgate.c) and of the framebuffer capture path. Needs only cc and python3.
qemu-rom-selftest:
	@python3 qemu/test_rom_selftest.py
	@python3 qemu/test_viewer.py

# Build the custom GBA tester ROM (needs a bare arm-none-eabi toolchain).
rom:
	@$(MAKE) -C rom

# The same tester linked as a multiboot image: it runs from EWRAM at
# 0x02000000 instead of from a cartridge, which is what makes it a payload
# mb_send() can actually deliver. See rom/multiboot.ld and rom/crt0_mb.s.
rom-mb:
	@$(MAKE) -C rom mb

# Structural check on the built image. Needs only python3 -- no ARM toolchain,
# no emulator -- so it is worth running on its own after any change to the
# multiboot link.
rom-mb-check:
	@python3 tools/check_multiboot.py rom/tester.mb

rom-clean:
	@$(MAKE) -C rom clean

# The fully automated end-to-end gate test: custom ROM in mGBA against the gate
# firmware in QEMU, with a screenshot of the gate's screen. Runs the tool-free
# self-test first, then skips the live run cleanly if any heavyweight tool is
# absent. See qemu/README.md.
qemu-rom:
	@bash ./qemu/test_rom.sh

# Boot the gate under QEMU and grab a PPM of its screen over UART2. Needs a
# built gate image and a running QEMU (see qemu/README.md).
qemu-display:
	@python3 qemu/fbgrab.py --one --out screen_qemu.ppm

qemu-clean:
	@rm -rf build/qemu-* build/rom-test
	@$(MAKE) -C rom clean 2>/dev/null || true
