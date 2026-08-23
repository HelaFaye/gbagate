# Host test suites. No hardware, no ESP-IDF, no PlatformIO.
#
#   make            build and run all fourteen suites
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

SUITES := chipgate link bridge hw ui chipdb encoder panel slot chipview \
          render multiboot catalog variant

.PHONY: all strict clean chipdb
all: $(addprefix run-,$(SUITES))

$(BUILD):
	@mkdir -p $(BUILD)

define SUITE_rule
$(BUILD)/test_$(1): test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) | $(BUILD)
	@$$(CC) $$(CFLAGS) -o $$@ $$< $$(addprefix src/,$$(DEPS_$(1)))
run-$(1): $(BUILD)/test_$(1)
	@printf '%-14s ' $(1); ./$$< $(DB) | tail -1
.PHONY: run-$(1)
endef
$(foreach s,$(SUITES),$(eval $(call SUITE_rule,$(s))))

# Every suite in every configuration. The catalogue bug that only appeared at
# -O2 -- and passed under the sanitiser -- is why this target exists.
define STRICT_rule
strict-$(1): | $(BUILD)
	@printf '%-14s' $(1); \
	for cfg in "-O0" "-O2" "-O3 -flto" "-g -fsanitize=address,undefined"; do \
	  if $$(CC) -std=c11 -Wall -Wextra -Wpedantic -Wshadow $$$$cfg -Isrc \
	       -o $(BUILD)/s_$(1) test/test_$(1).c $$(addprefix src/,$$(DEPS_$(1))) \
	       2>/dev/null && ./$(BUILD)/s_$(1) $$(DB) >/dev/null 2>&1; \
	  then printf ' ok     '; else printf ' FAIL   '; fi; \
	done; echo
.PHONY: strict-$(1)
endef
$(foreach s,$(SUITES),$(eval $(call STRICT_rule,$(s))))

strict: $(addprefix strict-,$(SUITES))
	@echo "-O0     -O2     -O3+lto asan/ubsan"

chipdb:
	@echo "needs the mmbn-chip-tables checkout and mGBA's res/ directory:"
	@echo "  python3 tools/build_chipdb.py <tables> data/chipdb --names <mgba>/res"

clean:
	@rm -rf $(BUILD)

# ---- QEMU ---------------------------------------------------------------
# Emulation targets. See qemu/README.md for what these do and do not prove.

.PHONY: qemu-setup qemu-build qemu-smoke qemu-proto qemu-clean

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

qemu-clean:
	@rm -rf build/qemu-*
