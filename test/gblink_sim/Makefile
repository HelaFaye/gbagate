# The GB-Link personality of the gbagate firmware, built for a host.
#
# The vendored application layer (third_party/gblink-firmware) compiles with
# no source changes against port/gblink's Zephyr shim, so this is GB-Link's
# own firmware logic running under test, not a re-implementation of it.
ROOT    := ../..
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
PSRC_CPP := $(P)/serial_layer.cpp $(P)/hardware_port.cpp $(P)/gblink_glue.cpp $(P)/gbagate_ext.cpp
PSRC_C   := $(P)/zshim_common.c $(P)/link_layer_core.c $(ROOT)/src/gblink_frame.c $(ROOT)/src/chipgate.c
SSRC_C   := zshim_posix.c sim_port.c sim_gba.c sim_main.c

all: $(OUT)/gblink_sim $(OUT)/usbdesc_dump

$(OUT)/usbdesc_dump: usbdesc_dump.c $(ROOT)/src/gblink_usbdesc.c $(ROOT)/src/gblink_usbdesc.h
	@mkdir -p $(OUT)
	@$(CC) -std=c11 $(WARN) -I$(ROOT)/src -o $@ usbdesc_dump.c $(ROOT)/src/gblink_usbdesc.c

$(OUT)/gblink_sim: $(VSRC_CPP) $(VSRC_C) $(PSRC_CPP) $(PSRC_C) $(SSRC_C) $(wildcard *.h) $(wildcard $(P)/*.h)
	@mkdir -p $(OUT)/obj
	@set -e; objs=""; \
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
.PHONY: all clean
