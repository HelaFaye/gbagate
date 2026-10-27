# The GB-Link personality of the gbagate firmware, built for a host.
#
# The vendored application layer (third_party/gblink-firmware) compiles with
# no source changes against port/gblink's Zephyr shim, so this is GB-Link's
# own firmware logic running under test, not a re-implementation of it.
ROOT    := ../..
.DEFAULT_GOAL := all
V       := $(ROOT)/third_party/gblink-firmware/src
P       := $(ROOT)/port/gblink
OUT     ?= $(ROOT)/build/gblink_sim

CC      ?= cc
CXX     ?= c++
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
INC     := -I$(P) -I$(V) -I$(ROOT)/src -I.
DEFS    := -DCONFIG_SECTIONS_USE_MASTER_MODE=1
CFLAGS  ?= -O1 -g
CXXFLAGS?= -O1 -g
SAN     ?=

VSRC_CPP := $(V)/layers/packetLayer.cpp $(V)/layers/transport.cpp \
            $(wildcard $(V)/callbacks/*.cpp) $(wildcard $(V)/payloads/*.cpp) \
            $(wildcard $(V)/sections/*.cpp) $(wildcard $(V)/module/*.cpp) \
            $(V)/persist.cpp
VSRC_C   := $(wildcard $(V)/payloads/*.c) $(V)/linkWrongPinWatchdog.c
PSRC_CPP := $(P)/serial_layer.cpp $(P)/hardware_port.cpp $(P)/gblink_glue.cpp $(P)/gbagate_ext.cpp $(P)/peer_glue.cpp $(P)/celio_glue.cpp
PSRC_C   := $(P)/zshim_common.c $(P)/link_layer_core.c $(P)/rfu_link_core.c $(ROOT)/src/gblink_frame.c $(ROOT)/src/chipgate.c $(ROOT)/src/gbl_peer.c \
            $(ROOT)/src/celio.c $(ROOT)/src/json_lite.c
SSRC_C   := zshim_posix.c sim_port.c sim_gba.c sim_rfu.c sim_main.c
SSRC_CPP := rfu_game.cpp

# pret's librfu, when fetch_librfu.sh has put it here: the wireless adapter
# tests' game. Built as C++ against gba_shim/ (whose register types are the
# hardware model's hooks) from copies port_librfu.py makes, with the few
# mechanical edits it lists; everything that talks to the adapter is pret's.
LIBRFU  ?= $(ROOT)/build/ref/pokefirered
GAME_FLAGS := -Igba_shim
ifneq ($(wildcard $(LIBRFU)/src/librfu_rfu.c),)
LIBRFU_SRC := $(OUT)/librfu/librfu_stwi.cpp $(OUT)/librfu/librfu_intr.cpp $(OUT)/librfu/librfu_sio32id.cpp $(OUT)/librfu/librfu_rfu.cpp
GAME_FLAGS += -DSIM_LIBRFU -I$(LIBRFU)/include -D__STDC_VERSION__=202311L
endif

all: $(OUT)/gblink_sim $(OUT)/usbdesc_dump $(OUT)/party_check $(OUT)/link_core_check

# The flags the simulator was built with: a change (SAN, CFLAGS) rebuilds it,
# so a sanitized build is never reused as a plain one or the other way round.
FLAGS_NOW := $(CC) $(CXX) $(CFLAGS) $(CXXFLAGS) $(SAN)
$(OUT)/.flags: FORCE
	@mkdir -p $(OUT)
	@[ "$$(cat $@ 2>/dev/null)" = '$(FLAGS_NOW)' ] || printf '%s' '$(FLAGS_NOW)' > $@
FORCE:

# The link layer core alone, always sanitized (test_link_core.c).
$(OUT)/link_core_check: test_link_core.c $(P)/link_layer_core.c $(P)/zshim_common.c zshim_posix.c $(wildcard $(P)/*.h)
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN) $(INC) $(DEFS) -o $@ test_link_core.c $(P)/link_layer_core.c $(P)/zshim_common.c zshim_posix.c -lpthread

# The trade emulator's party store alone, always sanitized (test_party.cpp).
$(OUT)/party_check: test_party.cpp $(V)/payloads/pokemon.cpp $(V)/payloads/pokemon.hpp
	@mkdir -p $(OUT)
	@$(CXX) -std=gnu++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(INC) $(DEFS) -o $@ test_party.cpp $(V)/payloads/pokemon.cpp

$(OUT)/usbdesc_dump: usbdesc_dump.c $(ROOT)/src/gblink_usbdesc.c $(ROOT)/src/gblink_usbdesc.h
	@mkdir -p $(OUT)
	@$(CC) -std=c11 $(WARN) -I$(ROOT)/src -o $@ usbdesc_dump.c $(ROOT)/src/gblink_usbdesc.c

$(LIBRFU_SRC) &: $(wildcard $(LIBRFU)/src/librfu_*.c) port_librfu.py
	@python3 -I port_librfu.py $(LIBRFU)/src $(OUT)/librfu

$(OUT)/gblink_sim: $(OUT)/.flags $(VSRC_CPP) $(VSRC_C) $(PSRC_CPP) $(PSRC_C) $(SSRC_C) $(SSRC_CPP) $(LIBRFU_SRC) $(wildcard *.h) $(wildcard gba_shim/*.h) $(wildcard $(P)/*.h) $(wildcard $(ROOT)/src/*.h) $(wildcard $(V)/*.hpp $(V)/*/*.hpp $(V)/*/*.h)
	@mkdir -p $(OUT)/obj
	@set -e; objs=""; \
	for f in $(LIBRFU_SRC); do \
	  o=$(OUT)/obj/$$(basename $$f .cpp).o; \
	  $(CXX) -std=gnu++20 $(CXXFLAGS) $(SAN) -w -fpermissive $(GAME_FLAGS) -c $$f -o $$o; objs="$$objs $$o"; \
	done; \
	for f in $(SSRC_CPP); do \
	  o=$(OUT)/obj/$$(echo $$f | tr '/.' '__').o; \
	  $(CXX) -std=gnu++20 $(CXXFLAGS) $(SAN) $(WARN) $(INC) $(GAME_FLAGS) -c $$f -o $$o; objs="$$objs $$o"; \
	done; \
	for f in $(VSRC_C) $(PSRC_C) $(SSRC_C); do \
	  o=$(OUT)/obj/$$(echo $$f | tr '/.' '__').o; \
	  $(CC) -std=gnu11 $(CFLAGS) $(SAN) $(WARN) $(INC) $(DEFS) -c $$f -o $$o; objs="$$objs $$o"; \
	done; \
	for f in $(VSRC_CPP) $(PSRC_CPP); do \
	  o=$(OUT)/obj/$$(echo $$f | tr '/.' '__').o; \
	  $(CXX) -std=gnu++20 $(CXXFLAGS) $(SAN) $(WARN) $(INC) $(DEFS) -c $$f -o $$o; objs="$$objs $$o"; \
	done; \
	$(CXX) $(SAN) -o $@ $$objs -lpthread

clean:
	rm -rf $(OUT)
.PHONY: all clean FORCE
