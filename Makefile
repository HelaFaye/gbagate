# Host test suites. No hardware, no ESP-IDF, no PlatformIO.
#
#   make            build and run the unit suites, the lint pass and the
#                   GB-Link simulator suites (make gblink)
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
DEPS_render    = ui_theme.c ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c
DEPS_multiboot = gba_multiboot.c
DEPS_catalog   = rom_catalog.c gba_multiboot.c gbagent_image.c
DEPS_variant   = variant.c
DEPS_ir        = ir_cdz.c
# The pin map lives in port/esp32s3/board.h and the two boards are checked in
# separate translation units, so both sets of macros can exist in one run.
DEPS_board     =
EXTRA_board    = -Iport/esp32s3 test/test_board_supermini.c
DEPS_controls  = controls.c ui_input.c
DEPS_input     = ui_input.c
DEPS_uiset     = ui_theme.c ui_settings.c settings.c chipgate.c ui_input.c
DEPS_settings  = settings.c chipgate.c ui_input.c
DEPS_romview   = ui_romview.c rom_catalog.c gba_multiboot.c
DEPS_mcp       = mcp23017.c chip_slot.c chip_reader.c
DEPS_mbe2e     = rom_catalog.c gba_multiboot.c

# Most suites are handed the chip database. This one takes the real tester ROM
# instead: using an actual file, with an actual unpadded length, is the point
# of it -- that is what found the alignment bug the synthetic fixtures missed.
ARG_mbe2e      = rom/tester.gba
DEPS_menu      = ui_theme.c ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c ui_shell.c hw_caps.c chipgate.c
DEPS_modes     = app_modes.c ui_shell.c hw_caps.c chipgate.c
DEPS_frame     = gblink_frame.c
DEPS_usbdesc   = gblink_usbdesc.c
DEPS_peer      = gbl_peer.c
DEPS_events    = input_events.c controls.c ui_input.c
DEPS_wificfg   = wifi_cfg.c settings.c chipgate.c ui_input.c
DEPS_json      = json_lite.c
DEPS_celio     = celio.c json_lite.c
DEPS_mbagent   = mbagent.c
DEPS_applets   = applet.c app_modes.c ui_shell.c hw_caps.c chipgate.c chipdb.c \
                 ui_screens.c ui_theme.c ui_render.c ui_font.c ui_chipview.c ui_list.c \
                 ir_cdz.c gbl_peer.c link_bridge.c link_proto.c link_session.c
# The example applets live outside src/, like any applet would.
EXTRA_applets  = -Iapplets $(wildcard applets/*.c)
DEPS_screens   = ui_screens.c ui_theme.c ui_render.c ui_font.c ui_chipview.c ui_list.c chipdb.c \
                 ui_shell.c hw_caps.c chipgate.c ir_cdz.c gbl_peer.c link_bridge.c \
                 link_proto.c link_session.c ui_settings.c settings.c ui_input.c \
                 controls.c ui_romview.c rom_catalog.c gba_multiboot.c applet.c app_modes.c
EXTRA_screens  = -Iapplets $(wildcard applets/*.c)

SUITES := chipgate link bridge hw ui chipdb encoder panel slot chipview \
          render multiboot catalog variant menu mbe2e mcp romview settings uiset ir input controls board modes frame usbdesc peer screens applets events wificfg json celio mbagent

.PHONY: all strict lint clean chipdb gblink gblink-asan compat screens agent-emu agent-image
all: $(addprefix run-,$(SUITES)) gblink agent-emu
	@./qemu/lint.sh

# The memory agent's compiled image (rom/agent.mb, what the board sends) on
# an emulated ARM7, driven by the board's own client through ctypes. Needs
# the unicorn module; without it the test skips, unless REQUIRE_EMU is set
# (CI sets it). AGENT_IMAGE= tests another build of it.
AGENT_IMAGE ?= rom/agent.mb
agent-emu: | $(BUILD)
	@$(CC) -std=c11 -O1 -shared -fPIC -Isrc src/mbagent.c test/agent_emu/shim.c -o $(BUILD)/libmbagent_shim.so
	@python3 -I test/agent_emu/agent_emu.py $(BUILD)/libmbagent_shim.so $(AGENT_IMAGE); st=$$?; \
	 if [ $$st -eq 77 ] && [ -z "$$REQUIRE_EMU" ]; then exit 0; fi; exit $$st

# After a change to rom/agent.c or agent_proto.h: rebuild the image and the
# C array the firmware embeds (lint checks the two are current).
agent-image:
	@$(MAKE) -s -C rom agent $(if $(TOOLCHAIN),TOOLCHAIN=$(TOOLCHAIN))
	@python3 tools/mb_embed.py rom/agent.mb src/gbagent_image.c

# The GB-Link personality: GB-Link's own firmware (third_party/) on the
# gbagate shim, driven end to end the way the GB-Link host apps drive it.
# The Wireless Adapter tests run pret's decompiled librfu as the game; it is
# fetched into build/ref the first time, and without it those tests skip --
# unless GBLINK_NO_SKIP is set (CI sets it), when a test that cannot run, or
# a fetch that fails, is a failure.
#
# A test script's last line on success; all of it, and a failing make, if not.
# (Not `| tail -1`, which would lose the exit status: see SUITE_rule below.)
# A subshell, so a failure can be followed by more of the recipe line.
RUNPY = (out=$$($(1) python3 -I test/gblink_sim/$(2) 2>&1); st=$$?; \
	if [ $$st -eq 0 ]; then printf '%s\n' "$$out" | tail -1; else printf '%s\n' "$$out"; exit $$st; fi)
GBLINK_SUITES := test_gblink.py test_peer.py test_rfu.py test_bench.py
FETCH_LIBRFU = test/gblink_sim/fetch_librfu.sh 2>/dev/null || \
	{ echo "librfu: not fetched (offline?); Wireless Adapter tests will skip"; [ -z "$$GBLINK_NO_SKIP" ]; }

gblink:
	@$(FETCH_LIBRFU)
	@$(MAKE) -s -C test/gblink_sim
	@$(foreach t,$(GBLINK_SUITES),$(call RUNPY,,$(t)) &&) true

# GB-Link's own host software -- the netplay bridge engine, Celio-Server and
# Celio-Client, the e-Reader, Pokemon and distributor web apps, the launcher,
# GB-Link-Switch-LDN's link task -- unmodified, against the firmware. Needs
# node and npm, and network access the first time (it clones the GB-Link
# repos, at the commits run.sh pins, into build/; GBLINK_SRC uses checkouts).
compat:
	@test/gblink_compat/run.sh

# The same suites against a simulator built with ASan and UBSan, in a build
# directory of its own (so the plain one is never left sanitized). Every
# report is fatal: UBSan's are not by default, and a recovered one would
# print and pass. A simulator that aborts fails the test it was in.
ASAN_SIM := $(BUILD)/gblink_sim_asan
gblink-asan:
	@$(FETCH_LIBRFU)
	@$(MAKE) -s -C test/gblink_sim OUT=$(abspath $(ASAN_SIM)) \
	    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
	@$(foreach t,$(GBLINK_SUITES),$(call RUNPY,GBLINK_SIM=$(abspath $(ASAN_SIM))/gblink_sim ASAN_OPTIONS=detect_leaks=0:abort_on_error=0,$(t)) &&) true

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
# Headers are dependencies too: without them a header-only change (a constant,
# a struct layout) leaves every test binary stale, and a suite reports on code
# that no longer exists. Found when a corrected descriptor length kept failing.
$(BUILD)/test_$(1): test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) $(wildcard src/*.h) $(wildcard applets/*) | $(BUILD)
	@$$(CC) $$(CFLAGS) $$(EXTRA_$(1)) -o $$@ $$< $$(addprefix src/,$$(DEPS_$(1))) -lm
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
	  if ! $$(CC) -std=c11 -Wall -Wextra -Wpedantic -Wshadow $$$$cfg -Isrc $$(EXTRA_$(1)) \
	       -o $(BUILD)/s_$(1) test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) -lm \
	       >> $(BUILD)/s_$(1).log 2>&1; \
	  then printf ' BUILD  '; \
	  elif ./$(BUILD)/s_$(1) $$(ARG_$(1)) >> $(BUILD)/s_$(1).log 2>&1; \
	  then printf ' ok     '; else printf ' FAIL   '; fi; \
	done; echo
.PHONY: strict-$(1)
endef
$(foreach s,$(SUITES),$(eval $(call STRICT_rule,$(s))))

# Every screen as a picture: build/screens/<name>.ppm, and gallery.png with
# all of them side by side. doc/screens-all.png is a copy of that gallery.
screens: $(BUILD)/test_screens
	@mkdir -p $(BUILD)/screens
	@./$< $(DB) $(BUILD)/screens | tail -1
	@python3 tools/ppm2png.py $(BUILD)/screens/gallery.ppm $(BUILD)/screens/gallery.png
	@echo "$(BUILD)/screens/gallery.png"

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
