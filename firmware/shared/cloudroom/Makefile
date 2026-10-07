ROOT := $(abspath ../../..)
BUILD ?= $(ROOT)/.tmp/cloudroom-wire
CXX ?= c++
CC ?= cc
.PHONY: test
$(BUILD)/core_json.o: third_party/coreJSON/core_json.c third_party/coreJSON/core_json.h
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -O2 -DJSON_MAX_DEPTH=8 -Ithird_party/coreJSON -c $< -o $@
$(BUILD)/wire: tests/wire.cpp CloudRoomWire.cpp CloudRoomWire.h $(BUILD)/core_json.o
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I. tests/wire.cpp CloudRoomWire.cpp $(BUILD)/core_json.o -o $@
test: $(BUILD)/wire
	$(BUILD)/wire $(ROOT)/.tmp/wire-frames
