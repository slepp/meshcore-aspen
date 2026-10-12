HEW ?= hew
WASM_CC ?= clang
HEW_BINARIES = $(patsubst %.d,%,$(wildcard build/*.d)) build/roles build/roles-release build/hew-host build/hew-host-release build/hew-base build/hew-base-release build/hew-dashboard build/hew-dashboard-release build/hew-broker build/hew-broker-release
$(HEW_BINARIES): .EXTRA_PREREQS = build/toolchain.json
build/toolchain.json: FORCE | build
	PYTHONDONTWRITEBYTECODE=1 python3 tools/toolchain.py --compiler "$(HEW)" --output $@
-include $(wildcard build/*.d)
.DEFAULT_GOAL := all
CC ?= cc
CXX ?= c++
export TMPDIR := $(CURDIR)/build
LINK = --link-lib $(CURDIR)/build/crypto_bridge.o --link-lib $(CURDIR)/build/storage_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcrypto --link-lib -l:libsodium.so.23
OBSERVER_LINK = --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
HOSTLIB = $(wildcard src/hostlib/*.hew)
MESHPROTO = $(wildcard src/meshproto/*.hew)
build/roles build/roles-release build/roles-interrupt build/hew-host build/hew-host-release \
build/hew-host-symbols build/hew-host-supervision build/hew-host-supervision-release: src/release_identity.hew

.PHONY: broker broker-test broker-symbols
BROKER_LINK = --link-lib $(CURDIR)/build/listener_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcrypto
broker: compiler-version build/hew-broker-release
broker-symbols: compiler-version build/hew-broker-symbols
build/listener_bridge.o: native/listener_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O2 -c $< -o $@
build/hew-broker: src/broker_main.hew src/broker.hew src/hostlib/listener.hew src/hostlib/mqtt_server.hew src/hostlib/mqtt_properties.hew src/hostlib/mqtt.hew src/hostlib/binary.hew build/listener_bridge.o
	$(HEW) build --emit-deps $@.d src/broker_main.hew -o $@ $(BROKER_LINK)
build/hew-broker-release: src/broker_main.hew src/broker.hew src/hostlib/listener.hew src/hostlib/mqtt_server.hew src/hostlib/mqtt_properties.hew src/hostlib/mqtt.hew src/hostlib/binary.hew build/listener_bridge.o
	$(HEW) build --emit-deps $@.d src/broker_main.hew --release -o $@ $(BROKER_LINK)
build/hew-broker-symbols: src/broker_main.hew src/broker.hew src/hostlib/listener.hew src/hostlib/mqtt_server.hew src/hostlib/mqtt_properties.hew src/hostlib/mqtt.hew src/hostlib/binary.hew build/listener_bridge.o
	$(HEW) build --emit-deps $@.d src/broker_main.hew -g --opt-level 2 -o $@ $(BROKER_LINK)
build/broker-checks: tests/broker_checks.hew src/broker.hew src/hostlib/listener.hew src/hostlib/mqtt_server.hew src/hostlib/mqtt_properties.hew src/hostlib/mqtt.hew src/hostlib/binary.hew build/listener_bridge.o
	$(HEW) build --emit-deps $@.d tests/broker_checks.hew -o $@ $(BROKER_LINK)
build/broker-checks-release: tests/broker_checks.hew src/broker.hew src/hostlib/listener.hew src/hostlib/mqtt_server.hew src/hostlib/mqtt_properties.hew src/hostlib/mqtt.hew src/hostlib/binary.hew build/listener_bridge.o
	$(HEW) build --emit-deps $@.d tests/broker_checks.hew --release -o $@ $(BROKER_LINK)
build/hew-broker build/hew-broker-release build/hew-broker-symbols build/broker-checks build/broker-checks-release: src/hostlib/events.hew
.PHONY: role-timer-test
role-timer-test: build/role-timers build/role-timers-release build/management-checks build/management-checks-release
	./build/role-timers
	./build/role-timers-release
	./build/management-checks
	./build/management-checks-release
build/role-timers: tests/role_timers.hew src/roles.hew src/wire.hew src/storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK)
build/role-timers-release: tests/role_timers.hew src/roles.hew src/wire.hew src/storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK)
build/rx-parse-checks: tests/rx_parse.hew src/roles.hew src/wire.hew src/storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK)
build/rx-parse-checks-release: tests/rx_parse.hew src/roles.hew src/wire.hew src/storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK)
.PHONY: rx-parse-test
rx-parse-test: build/rx-parse-checks build/rx-parse-checks-release
	./build/rx-parse-checks
	./build/rx-parse-checks-release
build/broker-oracle: tests/broker_oracle/main.go ../../internal/observer/broker.go | build
	go build -buildvcs=false -o $@ ./tests/broker_oracle
broker-test: compiler-version build/hew-broker build/hew-broker-release build/broker-checks build/broker-checks-release build/broker-oracle
	./build/broker-checks
	./build/broker-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/broker.py

build/companion-checks: tests/companion_checks.hew src/companion.hew src/companion_radio.hew $(MESHPROTO) $(HOSTLIB) src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o src/storage.hew
	$(HEW) build --emit-deps $@.d tests/companion_checks.hew -o $@ $(LINK)
build/companion-checks-release: tests/companion_checks.hew src/companion.hew src/companion_radio.hew $(MESHPROTO) $(HOSTLIB) src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o src/storage.hew
	$(HEW) build --emit-deps $@.d tests/companion_checks.hew --release -o $@ $(LINK)
.PHONY: base-local-test
base-local-test: build/companion-checks build/companion-checks-release
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BASE_BIN=$(CURDIR)/build/companion-checks go test ../../internal/companion -run '^TestHewBase(Local|RF|ReceivePolicy)Differential$$' -count=1 -timeout=120s
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BASE_BIN=$(CURDIR)/build/companion-checks-release go test ../../internal/companion -run '^TestHewBase(Local|RF|ReceivePolicy)Differential$$' -count=1 -timeout=120s
BASE_INPUTS = src/base_service.hew src/companion.hew src/companion_radio.hew src/companion_connection.hew src/companion_telemetry.hew src/companion_lifecycle.hew $(MESHPROTO) src/session.hew $(HOSTLIB) src/wire.hew src/storage.hew src/transport.hew
BASE_LINK = $(LINK) --link-lib $(CURDIR)/build/listener_bridge.o
build/hew-base: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d src/base_service.hew -o $@ $(BASE_LINK)
build/hew-base-release: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d src/base_service.hew --release -o $@ $(BASE_LINK)
build/hew-base-symbols: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d src/base_service.hew -g --opt-level 2 -o $@ $(BASE_LINK)
.PHONY: base-service base-symbols
base-service: compiler-version build/hew-base build/hew-base-release
base-symbols: compiler-version build/hew-base-symbols
.PHONY: base-service-test
base-service-test: base-service build/oracle
	PYTHONDONTWRITEBYTECODE=1 python3 tests/base_service.py
	MESHCORE_HEW_BASE_SERVICE=$(CURDIR)/build/hew-base-release PYTHONDONTWRITEBYTECODE=1 python3 tests/base_service.py

.PHONY: all test test-release compiler-version demo native-worker migration-test readiness-test library-test supervision-test firmware-negotiation-test management-test native-login-test modem-diagnostics-test mqtt-test outbound-test observation-test observer-auth-test observer-client-test observer-test FORCE
.PHONY: observer-service-test
.PHONY: owner-service-test owner-isolation-test
.PHONY: admin-test
.PHONY: region-test region-service-test
.PHONY: clock-location-test
.PHONY: packet-log-test
.PHONY: rx-policy-test
.PHONY: source-policy-test
.PHONY: role-lifecycle-test
.PHONY: combined-service-test release acceptance-test
release: build/hew-host-release build/hew-base-release build/hew-dashboard-release build/hew-broker-release
.PHONY: check
# Run the production processes, retained fault cases and Birch comparisons.
# Missing compilers, libraries and fixture binaries are errors, never skips.
check: export HEW_WORKERS = 4
check: release acceptance-test packet-workflow-test firmware-negotiation-test observer-queue-test supervision-test build/roles-interrupt build/worker-checks build/companion-checks-release build/hew-dashboard
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dashboard.py Dashboard.test_health_refresh_is_demand_driven_shared_and_bounded
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_service.py IntegratedObserver.test_restart_retains_pending_and_queued_observations
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/worker_binding.py
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release PYTHONDONTWRITEBYTECODE=1 python3 tests/storage_recovery.py
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release go test ../../internal/roles -run '^TestHew.*Differential$$' -count=1
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BASE_BIN=$(CURDIR)/build/companion-checks-release go test ../../internal/companion -run '^TestHewBase(Local|RF|ReceivePolicy)Differential$$' -count=1 -timeout=120s
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v
# A complete private host, real companion protocol, retained state and recovery.
acceptance-test: combined-service-test build/roles-release
	HEW_WORKERS=4 MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/readiness.py Readiness.test_reconnect_and_restart_do_not_reflood_roles Readiness.test_native_worker_fault_isolated_and_old_jobs_fenced
combined-service-test: release build/oracle
	HEW_WORKERS=4 PYTHONDONTWRITEBYTECODE=1 MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release python3 tests/combined_service.py
role-lifecycle-test: build/hew-host-supervision-release build/roles build/roles-release build/oracle
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-supervision-release PYTHONDONTWRITEBYTECODE=1 python3 tests/role_lifecycle.py
source-policy-test: build/roles build/roles-release build/hew-host-release build/oracle build/management-checks build/management-checks-release build/session-checks
	./build/management-checks
	./build/management-checks-release
	./build/session-checks
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/source_policy.py

rx-policy-test: build/roles build/roles-release build/hew-host-release build/oracle build/management-checks build/management-checks-release
	./build/management-checks
	./build/management-checks-release
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/rx_policy.py

packet-log-test: build/roles build/roles-release build/hew-host-release build/oracle build/session-checks
	./build/session-checks
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/packet_logging.py

clock-location-test: build/roles build/roles-release build/oracle build/management-checks build/management-checks-release
	./build/management-checks
	./build/management-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/clock_location.py

region-service-test: build/hew-host-release build/oracle build/arithmetic.wasm
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/region_service.py
region-test: build/region-checks build/region-checks-release
	./build/region-checks
	./build/region-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/regions.py
build/region-checks: tests/region_checks.hew src/meshproto/regions.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK)
build/region-checks-release: tests/region_checks.hew src/meshproto/regions.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK)
.PHONY: dashboard dashboard-test
dashboard: compiler-version build/hew-dashboard-release
DASHBOARD_DEPS = src/dashboard_main.hew src/dashboard.hew src/host_metrics.hew src/host_admin.hew src/owner_client.hew $(HOSTLIB) build/listener_bridge.o build/outbound_bridge.o build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
DASHBOARD_LINK = $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/listener_bridge.o --link-lib $(CURDIR)/build/owner_bridge.o
build/hew-dashboard: $(DASHBOARD_DEPS)
	$(HEW) build --emit-deps $@.d src/dashboard_main.hew -o $@ $(DASHBOARD_LINK)
build/hew-dashboard-release: $(DASHBOARD_DEPS)
	$(HEW) build --emit-deps $@.d src/dashboard_main.hew --release -o $@ $(DASHBOARD_LINK)
dashboard-test: build/hew-dashboard build/hew-dashboard-release build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dashboard.py
admin-test: build/admin-checks build/admin-checks-release
	./build/admin-checks
	./build/admin-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/admin_policy.py
build/admin-checks: tests/admin_checks.hew src/host_admin.hew | build
	$(HEW) build --emit-deps $@.d $< -o $@
build/admin-checks-release: tests/admin_checks.hew src/host_admin.hew | build
	$(HEW) build --emit-deps $@.d $< --release -o $@
.PHONY: source-workflow-test
source-workflow-test: build/hew-host-release build/arithmetic.wasm build/oracle
	HEW_WORKERS=4 MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/source_workflow.py
owner-service-test: build/hew-host-release build/oracle build/owner-client-checks build/owner-client-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/owner_service.py
owner-isolation-test: build/owner-isolation-checks build/owner-isolation-checks-release build/owner-client-checks
	PYTHONDONTWRITEBYTECODE=1 python3 tests/owner_isolation.py
build/owner-isolation-checks: tests/owner_isolation_checks.hew src/owner_control.hew src/host_admin.hew src/host_health.hew $(HOSTLIB) $(MESHPROTO) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o build/outbound_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-isolation-checks-release: tests/owner_isolation_checks.hew src/owner_control.hew src/host_admin.hew src/host_health.hew $(HOSTLIB) $(MESHPROTO) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o build/outbound_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-client-checks: tests/owner_client_checks.hew src/owner_client.hew $(HOSTLIB) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-client-checks-release: tests/owner_client_checks.hew src/owner_client.hew $(HOSTLIB) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK) --link-lib $(CURDIR)/build/owner_bridge.o
.PHONY: reconciliation-test
reconciliation-test: build/hew-host-release build/roles-release build/oracle build/arithmetic.wasm
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release PYTHONDONTWRITEBYTECODE=1 python3 tests/reconciliation.py
observer-service-test: build/hew-host-release build/oracle
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_service.py
FORCE:
src/worker_identity.hew: FORCE | build
	PYTHONDONTWRITEBYTECODE=1 python3 tools/build_worker.py
native-worker: src/worker_identity.hew
.PHONY: native-contacts-test
native-contacts-test: build/hew-host-release build/oracle
	HEW_WORKERS=4 MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/native_contacts.py
all: compiler-version build/roles build/hew-host
compiler-version:
	$(HEW) --version
build/hew-host build/hew-host-release build/hew-host-symbols \
build/hew-host-supervision build/hew-host-supervision-release \
build/hew-base build/hew-base-release build/hew-base-symbols \
build/hew-dashboard build/hew-dashboard-release \
build/hew-broker build/hew-broker-release build/hew-broker-symbols: | compiler-version
build:
	mkdir -p build
build/crypto_bridge.o: native/crypto_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/owner_bridge.o: native/owner_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/identity-bindings: tests/identity_bindings.c native/crypto_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -lcrypto -l:libsodium.so.23
build/storage_bridge.o: native/storage_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/storage_interrupt.o: native/storage_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DMC_STORAGE_TEST_INTERRUPT -c $< -o $@
build/roles-interrupt: tests/main.hew src/roles.hew src/storage.hew src/wire.hew src/modem.hew build/crypto_bridge.o build/storage_interrupt.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/main.hew -o $@ $(subst storage_bridge.o,storage_interrupt.o,$(LINK))
build/transport_bridge.o: native/transport_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/event_bridge.o: native/event_bridge.c native/event_bridge.h | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/event-boundary: tests/event_boundary.hew src/hostlib/events.hew src/hostlib/duplex.hew src/hostlib/listener.hew build/event_bridge.o build/transport_bridge.o build/listener_bridge.o
	$(HEW) build --emit-deps $@.d tests/event_boundary.hew --release -o $@ --link-lib $(CURDIR)/build/event_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/listener_bridge.o --link-lib -lcrypto
.PHONY: event-boundary-test
build/bot-signal-checks: tests/bot_signal.cpp ../../firmware/runtime/BotSignal.h | build
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -DMESHCORE_HOST_BOT_SOURCE=1 $< -pthread -o $@
.PHONY: bot-signal-test
bot-signal-test: build/bot-signal-checks
	timeout -k 5 10 ./build/bot-signal-checks
build/event-native-checks: tests/event_boundary.c native/event_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -pthread
event-boundary-test: compiler-version build/event-boundary build/event-native-checks
	timeout -k 5 10 ./build/event-native-checks
	timeout -k 5 10 ./build/event-boundary
build/transport_supervision.o: native/transport_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DMC_SUPERVISION_TEST -c $< -o $@
build/hew-host-supervision: src/service_main.hew src/service.hew src/session.hew src/transport.hew src/wire.hew src/roles.hew src/storage.hew src/worker_identity.hew $(HOSTLIB) build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build --emit-deps $@.d src/service_main.hew -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
build/hew-host-supervision-release: src/service_main.hew src/service.hew src/session.hew src/transport.hew src/wire.hew src/roles.hew src/storage.hew src/worker_identity.hew $(HOSTLIB) build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build --emit-deps $@.d src/service_main.hew --release -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
build/roles: tests/main.hew src/wire.hew src/roles.hew src/storage.hew src/modem.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/main.hew -o $@ $(LINK)
build/hew-host: src/service_main.hew src/service.hew src/session.hew src/transport.hew src/wire.hew src/roles.hew src/storage.hew src/worker_identity.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d src/service_main.hew -o $@ $(LINK)
build/session-checks: tests/session_checks.hew src/session.hew src/transport.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/session_checks.hew -o $@ $(LINK)
build/protocol-state-checks: tests/protocol_state.hew src/companion_telemetry.hew src/wire.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< -o $@ $(LINK)
build/protocol-state-checks-release: tests/protocol_state.hew src/companion_telemetry.hew src/wire.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d $< --release -o $@ $(LINK)
.PHONY: protocol-state-test
protocol-state-test: build/session-checks build/session-checks-release build/protocol-state-checks build/protocol-state-checks-release
	./build/session-checks
	./build/session-checks-release
	./build/protocol-state-checks
	./build/protocol-state-checks-release
build/worker-checks: tests/worker_checks.hew src/transport.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/worker_checks.hew -o $@ $(LINK)
build/roles build/roles-release build/roles-interrupt build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release build/session-checks build/worker-checks: $(HOSTLIB) $(MESHPROTO)
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: src/observer_runtime.hew src/observer_client.hew src/observer_queue.hew build/outbound_bridge.o
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: LINK += $(OBSERVER_LINK)
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: src/owner_control.hew src/host_admin.hew src/host_health.hew build/owner_bridge.o
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: LINK += --link-lib $(CURDIR)/build/owner_bridge.o
build/management-checks: tests/management_checks.hew $(HOSTLIB) $(MESHPROTO) | build
	$(HEW) build --emit-deps $@.d tests/management_checks.hew -o $@
build/management-checks-release: tests/management_checks.hew $(HOSTLIB) $(MESHPROTO) | build
	$(HEW) build --emit-deps $@.d tests/management_checks.hew --release -o $@
build/mqtt-checks: tests/mqtt_checks.hew src/hostlib/mqtt.hew src/hostlib/binary.hew | build
	$(HEW) build --emit-deps $@.d tests/mqtt_checks.hew -o $@
build/mqtt-checks-release: tests/mqtt_checks.hew src/hostlib/mqtt.hew src/hostlib/binary.hew | build
	$(HEW) build --emit-deps $@.d tests/mqtt_checks.hew --release -o $@
build/observation-checks: tests/observation_checks.hew src/meshproto/observation.hew src/hostlib/mqtt.hew src/hostlib/binary.hew | build
	$(HEW) build --emit-deps $@.d tests/observation_checks.hew -o $@
build/observation-checks-release: tests/observation_checks.hew src/meshproto/observation.hew src/hostlib/mqtt.hew src/hostlib/binary.hew | build
	$(HEW) build --emit-deps $@.d tests/observation_checks.hew --release -o $@
observation-test: build/observation-checks build/observation-checks-release
	MESHCORE_HEW_OBSERVATION_BIN=$(CURDIR)/build/observation-checks go test ../../internal/observer -run '^TestHewObservation' -count=1
	MESHCORE_HEW_OBSERVATION_BIN=$(CURDIR)/build/observation-checks-release go test ../../internal/observer -run '^TestHewObservation' -count=1
build/observer-auth-checks: tests/observer_auth_checks.hew src/meshproto/observer_auth.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/observer_auth_checks.hew -o $@ $(LINK)
build/observer-auth-checks-release: tests/observer_auth_checks.hew src/meshproto/observer_auth.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/observer_auth_checks.hew --release -o $@ $(LINK)
observer-auth-test: build/observer-auth-checks build/observer-auth-checks-release
	MESHCORE_HEW_OBSERVER_AUTH_BIN=$(CURDIR)/build/observer-auth-checks go test ../../internal/observer -run '^TestHewObserverIdentity' -count=1
	MESHCORE_HEW_OBSERVER_AUTH_BIN=$(CURDIR)/build/observer-auth-checks-release go test ../../internal/observer -run '^TestHewObserverIdentity' -count=1
build/outbound_bridge.o: native/outbound_bridge.c native/event_bridge.h build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $$(pkg-config --cflags libcurl) -c $< -o $@
build/outbound-checks: tests/outbound_checks.hew src/hostlib/outbound.hew src/hostlib/events.hew src/hostlib/binary.hew build/outbound_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/outbound_checks.hew -o $@ --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcurl --link-lib -lcrypto
build/outbound-checks-release: tests/outbound_checks.hew src/hostlib/outbound.hew src/hostlib/events.hew src/hostlib/binary.hew build/outbound_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/outbound_checks.hew --release -o $@ --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcurl --link-lib -lcrypto
build/observer-client-checks: tests/observer_client_checks.hew src/observer_client.hew src/observer_queue.hew src/observer_store.hew $(HOSTLIB) $(MESHPROTO) build/outbound_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o src/storage.hew
	$(HEW) build --emit-deps $@.d tests/observer_client_checks.hew -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK)) --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
build/observer-client-checks-release: tests/observer_client_checks.hew src/observer_client.hew src/observer_queue.hew src/observer_store.hew $(HOSTLIB) $(MESHPROTO) build/outbound_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o src/storage.hew
	$(HEW) build --emit-deps $@.d tests/observer_client_checks.hew --release -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK)) --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
build/observer-queue-checks: tests/observer_queue_checks.hew src/observer_queue.hew src/observer_store.hew $(HOSTLIB) $(MESHPROTO) build/event_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o | build src/storage.hew
	$(HEW) build --emit-deps $@.d tests/observer_queue_checks.hew -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
build/observer-queue-checks-release: tests/observer_queue_checks.hew src/observer_queue.hew src/observer_store.hew $(HOSTLIB) $(MESHPROTO) build/event_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o | build src/storage.hew
	$(HEW) build --emit-deps $@.d tests/observer_queue_checks.hew --release -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
.PHONY: observer-queue-test
observer-queue-test: build/observer-queue-checks build/observer-queue-checks-release
	./build/observer-queue-checks
	./build/observer-queue-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_queue.py
observer-client-test: observer-queue-test build/observer-client-checks build/observer-client-checks-release build/outbound-checks build/outbound-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_client.py
	MESHCORE_HEW_OBSERVER_CLIENT_BIN=$(CURDIR)/build/observer-client-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_client.py
observer-test: mqtt-test outbound-test observation-test observer-auth-test observer-client-test
build/endpoint-checks: tests/endpoint_checks.hew src/meshproto/mqtt_endpoint.hew | build
	$(HEW) build --emit-deps $@.d tests/endpoint_checks.hew -o $@
build/endpoint-checks-release: tests/endpoint_checks.hew src/meshproto/mqtt_endpoint.hew | build
	$(HEW) build --emit-deps $@.d tests/endpoint_checks.hew --release -o $@
observer-test: build/endpoint-checks build/endpoint-checks-release
	./build/endpoint-checks
	./build/endpoint-checks-release
mqtt-test: build/mqtt-checks build/mqtt-checks-release
	MESHCORE_HEW_MQTT_BIN=$(CURDIR)/build/mqtt-checks go test ../../internal/observer -run '^TestHewMQTT' -count=1
	MESHCORE_HEW_MQTT_BIN=$(CURDIR)/build/mqtt-checks-release go test ../../internal/observer -run '^TestHewMQTT' -count=1
outbound-test: build/outbound-checks build/outbound-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/outbound.py
	MESHCORE_HEW_OUTBOUND_BIN=$(CURDIR)/build/outbound-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/outbound.py
build/library-fixture.o: tests/library_fixture.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/child-fixture: tests/child_fixture.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@
build/library-checks: tests/library_checks.hew $(HOSTLIB) build/library-fixture.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/library_checks.hew -o $@ $(LINK) --link-lib $(CURDIR)/build/library-fixture.o
build/library-checks-release: tests/library_checks.hew $(HOSTLIB) build/library-fixture.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/library_checks.hew --release -o $@ $(LINK) --link-lib $(CURDIR)/build/library-fixture.o
build/runtime-contract: tests/runtime_contract.hew | build
	$(HEW) build --emit-deps $@.d $< -o $@
build/runtime-contract-release: tests/runtime_contract.hew | build
	$(HEW) build --emit-deps $@.d $< --release -o $@
build/supervised-resource-contract: tests/supervised_resource_contract.hew | build
	$(HEW) build --emit-deps $@.d $< -o $@
build/supervised-resource-contract-release: tests/supervised_resource_contract.hew | build
	$(HEW) build --emit-deps $@.d $< --release -o $@
build/std-tcp-write-probe: tests/probes/std_tcp_write_probe.hew | build
	$(HEW) build --emit-deps $@.d $< -o $@
build/std-tcp-write-probe-release: tests/probes/std_tcp_write_probe.hew | build
	$(HEW) build --emit-deps $@.d $< --release -o $@
build/std-net-select-probe: tests/probes/std_net_select_probe.hew | build
	$(HEW) build --emit-deps $@.d $< -o $@
build/std-net-select-probe-release: tests/probes/std_net_select_probe.hew | build
	$(HEW) build --emit-deps $@.d $< --release -o $@
build/roles-release: tests/main.hew src/wire.hew src/roles.hew src/storage.hew src/modem.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/main.hew --release -o $@ $(LINK)
build/hew-host-release: src/service_main.hew src/service.hew src/session.hew src/transport.hew src/wire.hew src/roles.hew src/storage.hew src/worker_identity.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d src/service_main.hew --release -o $@ $(LINK)
build/oracle: tests/oracle/main.go | build
	TMPDIR=$(CURDIR)/build go build -buildvcs=false -o $@ ./tests/oracle
test: all build/oracle build/session-checks build/worker-checks build/roles-interrupt firmware-negotiation-test
	./build/session-checks
	PYTHONDONTWRITEBYTECODE=1 python3 tests/worker_binding.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/parity.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/management.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/native_login.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/storage_recovery.py
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BIN=$(CURDIR)/build/roles go test ../../internal/roles -run '^TestHew.*Differential$$' -count=1
test-release: build/roles-release build/hew-host-release build/oracle build/arithmetic.wasm build/roles-interrupt build/worker-checks build/session-checks-release
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release PYTHONDONTWRITEBYTECODE=1 python3 tests/parity.py
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/management.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/native_login.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release MESHCORE_HEW_SESSION_CHECKS=$(CURDIR)/build/session-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release PYTHONDONTWRITEBYTECODE=1 python3 tests/storage_recovery.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/worker_binding.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/service_demo.py
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/readiness.py
	MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/migration.py
	TMPDIR=$(CURDIR)/build MESHCORE_NATIVE_TEST_STATE_ROOT=$(abspath $(CURDIR)/../..) \
	  MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release \
	  MESHCORE_HEW_TRANSCRIPT=$(CURDIR)/build/bot-differential.json \
	  MESHCORE_HEW_SCOPE_TRANSCRIPT=$(CURDIR)/build/bot-scoped-differential.json \
	  BOT_NATIVE_WORKER=$${BOT_NATIVE_WORKER:-$(CURDIR)/build/native-worker} \
	  go test ../../internal/roles ../../internal/app -run '^TestHew.*Differential$$' -count=1
build/arithmetic.wasm: ../../firmware/runtime/wasm/examples/arithmetic.c ../../firmware/runtime/wasm/sdk/meshcore.h | build
	TMPDIR=$(CURDIR)/build $(WASM_CC) --target=wasm32 -Oz -nostdlib -fno-builtin \
	  -mno-bulk-memory -mno-reference-types -mno-multivalue \
	  -Wl,--no-entry -Wl,--strip-all -Wl,--initial-memory=65536 \
	  -Wl,--max-memory=65536 -Wl,-z,stack-size=4096 $< -o $@
demo: all build/oracle build/arithmetic.wasm
	PYTHONDONTWRITEBYTECODE=1 python3 tests/service_demo.py
	TMPDIR=$(CURDIR)/build BOT_NATIVE_WORKER=$${BOT_NATIVE_WORKER:-$(CURDIR)/build/native-worker} \
	  MESHCORE_HEW_TRANSCRIPT=$(CURDIR)/build/bot-differential.json \
	  MESHCORE_HEW_SCOPE_TRANSCRIPT=$(CURDIR)/build/bot-scoped-differential.json \
	  go test ../../internal/app -run '^TestHewNativeBot.*Differential$$' -count=1
migration-test: all build/oracle build/arithmetic.wasm
	PYTHONDONTWRITEBYTECODE=1 python3 tests/migration.py
management-test: all build/oracle build/management-checks build/management-checks-release
	./build/management-checks
	./build/management-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/management.py
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BIN=$(CURDIR)/build/roles go test ../../internal/roles -run '^TestHew(Management|Discovery)SerializedDifferential$$' -count=1
readiness-test: all build/oracle
	PYTHONDONTWRITEBYTECODE=1 python3 tests/readiness.py
library-test: all build/oracle build/library-checks build/library-checks-release build/management-checks build/management-checks-release build/runtime-contract build/runtime-contract-release build/child-fixture build/hew-host-release build/roles-release build/std-tcp-write-probe build/std-tcp-write-probe-release build/std-net-select-probe build/std-net-select-probe-release
	./build/management-checks
	./build/management-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/libraries.py
	MESHCORE_HEW_LIBRARY=$(CURDIR)/build/library-checks-release \
	  MESHCORE_HEW_STD_TCP=$(CURDIR)/build/std-tcp-write-probe-release \
	  MESHCORE_HEW_STD_SELECT=$(CURDIR)/build/std-net-select-probe-release \
	  MESHCORE_HEW_RUNTIME=$(CURDIR)/build/runtime-contract-release \
	  MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release \
	  MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release \
	  PYTHONDONTWRITEBYTECODE=1 python3 tests/libraries.py
supervision-test: all build/oracle build/hew-host-supervision build/hew-host-supervision-release build/hew-host-release build/supervised-resource-contract build/supervised-resource-contract-release
	./build/supervised-resource-contract
	./build/supervised-resource-contract-release
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-supervision PYTHONDONTWRITEBYTECODE=1 python3 tests/supervision.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-supervision-release \
	MESHCORE_HEW_NORMAL_HOST=$(CURDIR)/build/hew-host-release \
	PYTHONDONTWRITEBYTECODE=1 python3 tests/supervision.py
firmware-negotiation-test: all build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/firmware_negotiation.py --build
	PYTHONDONTWRITEBYTECODE=1 python3 tests/firmware_negotiation.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/firmware_negotiation.py
native-login-test: all build/oracle build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/native_login.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release PYTHONDONTWRITEBYTECODE=1 python3 tests/native_login.py
build/session-checks-release: tests/session_checks.hew src/session.hew src/transport.hew src/wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build --emit-deps $@.d tests/session_checks.hew --release -o $@ $(LINK)
modem-diagnostics-test: all build/oracle build/session-checks build/session-checks-release build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release MESHCORE_HEW_SESSION_CHECKS=$(CURDIR)/build/session-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py

# Shared native value layout is a dependency of every C bridge consumer.
$(patsubst %.c,build/%.o,$(notdir $(wildcard native/*_bridge.c))) build/storage_interrupt.o build/transport_supervision.o build/identity-bindings build/event-native-checks build/library-fixture.o: native/bridge_abi.h

.PHONY: packet-workflow-test
packet-workflow-test: build/hew-host build/hew-host-release build/oracle build/repeater-snr.wasm
	HEW_WORKERS=4 PYTHONDONTWRITEBYTECODE=1 python3 tests/packet_workflow.py
build/repeater-snr.wasm: ../../firmware/runtime/wasm/examples/repeater_snr.c ../../firmware/runtime/wasm/sdk/packet.h | build
	TMPDIR=$(CURDIR)/build $(WASM_CC) --target=wasm32 -Oz -nostdlib -fno-builtin -I../../firmware/shared \
	  -mno-bulk-memory -mno-reference-types -mno-multivalue \
	  -Wl,--no-entry -Wl,--strip-all -Wl,--initial-memory=65536 \
	  -Wl,--max-memory=65536 -Wl,-z,stack-size=4096 $< -o $@
