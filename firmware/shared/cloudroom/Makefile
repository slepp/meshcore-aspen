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
test: $(BUILD)/wire $(BUILD)/bridge $(BUILD)/opaque
	python3 $(ROOT)/services/shared-room/tools/measure_wire.py --json-only --out $(BUILD)/frames
	$(BUILD)/wire $(BUILD)/frames
	$(BUILD)/bridge
	$(BUILD)/opaque
$(BUILD)/opaque: tests/opaque.cpp OpaqueFrontend.cpp OpaqueFrontend.h RadioBridge.h CloudRoomWire.cpp CloudRoomWire.h $(BUILD)/core_json.o
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I. tests/opaque.cpp OpaqueFrontend.cpp CloudRoomWire.cpp $(BUILD)/core_json.o -o $@
$(BUILD)/bridge: tests/bridge.cpp RadioBridge.h
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I. tests/bridge.cpp -o $@
