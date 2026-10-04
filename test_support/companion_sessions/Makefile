ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
UPSTREAM ?= $(ROOT)/.tmp/parity-upstream
BUILD ?= $(ROOT)/.tmp/companion-sessions
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread
CPPFLAGS := -DCOMPANION_SESSIONS_HOST -I$(ROOT)/test_support/companion_sessions -I$(UPSTREAM)/src -I$(ROOT)/firmware/esp32
SOURCES := $(ROOT)/firmware/esp32/CompanionSessions.cpp $(ROOT)/test_support/companion_sessions/behaviour.cpp

.PHONY: test
.DEFAULT_GOAL := test
test: $(BUILD)/behaviour
	$(BUILD)/behaviour

$(BUILD)/behaviour: $(SOURCES) $(ROOT)/firmware/esp32/CompanionSessions.h $(ROOT)/test_support/companion_sessions/Arduino.h $(UPSTREAM)/src/helpers/BaseSerialInterface.h
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -Wl,--wrap=send -o $@
