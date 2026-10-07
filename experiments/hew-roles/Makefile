HEW ?= hew
.DEFAULT_GOAL := all
CC ?= cc
CXX ?= c++
export TMPDIR := $(CURDIR)/build
LINK = --link-lib $(CURDIR)/build/crypto_bridge.o --link-lib $(CURDIR)/build/storage_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcrypto --link-lib -l:libsodium.so.23
OBSERVER_LINK = --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
HOSTLIB = $(wildcard hostlib/*.hew)
MESHPROTO = $(wildcard meshproto/*.hew)

.PHONY: broker broker-test broker-symbols
BROKER_LINK = --link-lib $(CURDIR)/build/listener_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcrypto
broker: compiler-version build/hew-broker-release
broker-symbols: compiler-version build/hew-broker-symbols
build/listener_bridge.o: listener_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O2 -c $< -o $@
build/hew-broker: broker_main.hew broker.hew hostlib/listener.hew hostlib/mqtt_server.hew hostlib/mqtt_properties.hew hostlib/mqtt.hew hostlib/binary.hew build/listener_bridge.o
	$(HEW) build broker_main.hew -o $@ $(BROKER_LINK)
build/hew-broker-release: broker_main.hew broker.hew hostlib/listener.hew hostlib/mqtt_server.hew hostlib/mqtt_properties.hew hostlib/mqtt.hew hostlib/binary.hew build/listener_bridge.o
	$(HEW) build broker_main.hew --release -o $@ $(BROKER_LINK)
build/hew-broker-symbols: broker_main.hew broker.hew hostlib/listener.hew hostlib/mqtt_server.hew hostlib/mqtt_properties.hew hostlib/mqtt.hew hostlib/binary.hew build/listener_bridge.o
	$(HEW) build broker_main.hew -g --opt-level 2 -o $@ $(BROKER_LINK)
build/broker-checks: broker_checks.hew broker.hew hostlib/listener.hew hostlib/mqtt_server.hew hostlib/mqtt_properties.hew hostlib/mqtt.hew hostlib/binary.hew build/listener_bridge.o
	$(HEW) build broker_checks.hew -o $@ $(BROKER_LINK)
build/broker-checks-release: broker_checks.hew broker.hew hostlib/listener.hew hostlib/mqtt_server.hew hostlib/mqtt_properties.hew hostlib/mqtt.hew hostlib/binary.hew build/listener_bridge.o
	$(HEW) build broker_checks.hew --release -o $@ $(BROKER_LINK)
build/hew-broker build/hew-broker-release build/hew-broker-symbols build/broker-checks build/broker-checks-release: hostlib/events.hew
.PHONY: role-timer-test
role-timer-test: build/role-timers build/role-timers-release build/management-checks build/management-checks-release
	./build/role-timers
	./build/role-timers-release
	./build/management-checks
	./build/management-checks-release
build/role-timers: tests/role_timers.hew roles.hew wire.hew storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< -o $@ $(LINK)
build/role-timers-release: tests/role_timers.hew roles.hew wire.hew storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< --release -o $@ $(LINK)
build/rx-parse-checks: tests/rx_parse.hew roles.hew wire.hew storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< -o $@ $(LINK)
build/rx-parse-checks-release: tests/rx_parse.hew roles.hew wire.hew storage.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< --release -o $@ $(LINK)
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

build/companion-checks: companion_checks.hew companion.hew companion_radio.hew $(MESHPROTO) $(HOSTLIB) wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build companion_checks.hew -o $@ $(LINK)
build/companion-checks-release: companion_checks.hew companion.hew companion_radio.hew $(MESHPROTO) $(HOSTLIB) wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build companion_checks.hew --release -o $@ $(LINK)
.PHONY: base-local-test
base-local-test: build/companion-checks build/companion-checks-release
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BASE_BIN=$(CURDIR)/build/companion-checks go test ../../internal/companion -run '^TestHewBase(Local|RF|ReceivePolicy)Differential$$' -count=1 -timeout=120s
	TMPDIR=$(CURDIR)/build MESHCORE_HEW_BASE_BIN=$(CURDIR)/build/companion-checks-release go test ../../internal/companion -run '^TestHewBase(Local|RF|ReceivePolicy)Differential$$' -count=1 -timeout=120s
BASE_INPUTS = base_service.hew companion.hew companion_radio.hew companion_connection.hew companion_telemetry.hew companion_lifecycle.hew $(MESHPROTO) session.hew $(HOSTLIB) wire.hew storage.hew transport.hew
BASE_LINK = $(LINK) --link-lib $(CURDIR)/build/listener_bridge.o
build/hew-base: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build base_service.hew -o $@ $(BASE_LINK)
build/hew-base-release: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build base_service.hew --release -o $@ $(BASE_LINK)
build/hew-base-symbols: $(BASE_INPUTS) build/listener_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build base_service.hew -g --opt-level 2 -o $@ $(BASE_LINK)
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
.PHONY: combined-service-test
combined-service-test: broker base-service build/hew-dashboard-release build/hew-host-release build/oracle
	PYTHONDONTWRITEBYTECODE=1 MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release python3 tests/combined_service.py
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
build/region-checks: region_checks.hew meshproto/regions.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< -o $@ $(LINK)
build/region-checks-release: region_checks.hew meshproto/regions.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< --release -o $@ $(LINK)
.PHONY: dashboard dashboard-test
dashboard: compiler-version build/hew-dashboard-release
DASHBOARD_DEPS = dashboard_main.hew dashboard.hew host_admin.hew owner_client.hew $(HOSTLIB) build/listener_bridge.o build/outbound_bridge.o build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
DASHBOARD_LINK = $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/listener_bridge.o --link-lib $(CURDIR)/build/owner_bridge.o
build/hew-dashboard: $(DASHBOARD_DEPS)
	$(HEW) build dashboard_main.hew -o $@ $(DASHBOARD_LINK)
build/hew-dashboard-release: $(DASHBOARD_DEPS)
	$(HEW) build dashboard_main.hew --release -o $@ $(DASHBOARD_LINK)
dashboard-test: build/hew-dashboard build/hew-dashboard-release build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/dashboard.py
admin-test: build/admin-checks build/admin-checks-release
	./build/admin-checks
	./build/admin-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/admin_policy.py
build/admin-checks: admin_checks.hew host_admin.hew | build
	$(HEW) build $< -o $@
build/admin-checks-release: admin_checks.hew host_admin.hew | build
	$(HEW) build $< --release -o $@
owner-service-test: build/hew-host-release build/oracle build/owner-client-checks build/owner-client-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/owner_service.py
owner-isolation-test: build/owner-isolation-checks build/owner-isolation-checks-release build/owner-client-checks
	PYTHONDONTWRITEBYTECODE=1 python3 tests/owner_isolation.py
build/owner-isolation-checks: owner_isolation_checks.hew owner_control.hew host_admin.hew host_health.hew $(HOSTLIB) $(MESHPROTO) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o build/outbound_bridge.o
	$(HEW) build $< -o $@ $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-isolation-checks-release: owner_isolation_checks.hew owner_control.hew host_admin.hew host_health.hew $(HOSTLIB) $(MESHPROTO) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o build/outbound_bridge.o
	$(HEW) build $< --release -o $@ $(LINK) $(OBSERVER_LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-client-checks: owner_client_checks.hew owner_client.hew $(HOSTLIB) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< -o $@ $(LINK) --link-lib $(CURDIR)/build/owner_bridge.o
build/owner-client-checks-release: owner_client_checks.hew owner_client.hew $(HOSTLIB) build/owner_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< --release -o $@ $(LINK) --link-lib $(CURDIR)/build/owner_bridge.o
.PHONY: reconciliation-test
reconciliation-test: build/hew-host-release build/roles-release build/oracle build/arithmetic.wasm
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release MESHCORE_HEW_BIN=$(CURDIR)/build/roles-release PYTHONDONTWRITEBYTECODE=1 python3 tests/reconciliation.py
observer-service-test: build/hew-host-release build/oracle
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_service.py
FORCE:
worker_identity.hew: FORCE | build
	PYTHONDONTWRITEBYTECODE=1 python3 build_worker.py
native-worker: worker_identity.hew
all: compiler-version build/roles build/hew-host
compiler-version:
	@test "$$($(HEW) --version)" = 'hew 0.6.0-rc7' || { echo 'Willow requires hew 0.6.0-rc7 (FFI ABI).'; exit 1; }
build/hew-host build/hew-host-release build/hew-host-symbols \
build/hew-host-supervision build/hew-host-supervision-release \
build/hew-base build/hew-base-release build/hew-base-symbols \
build/hew-dashboard build/hew-dashboard-release \
build/hew-broker build/hew-broker-release build/hew-broker-symbols: | compiler-version
build:
	mkdir -p build
build/crypto_bridge.o: crypto_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/owner_bridge.o: owner_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/identity-bindings: tests/identity_bindings.c crypto_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -lcrypto -l:libsodium.so.23
build/storage_bridge.o: storage_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/storage_interrupt.o: storage_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DMC_STORAGE_TEST_INTERRUPT -c $< -o $@
build/roles-interrupt: main.hew roles.hew storage.hew wire.hew modem.hew build/crypto_bridge.o build/storage_interrupt.o build/transport_bridge.o
	$(HEW) build main.hew -o $@ $(subst storage_bridge.o,storage_interrupt.o,$(LINK))
build/transport_bridge.o: transport_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/event_bridge.o: event_bridge.c event_bridge.h | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/event-boundary: tests/event_boundary.hew hostlib/events.hew hostlib/duplex.hew hostlib/listener.hew build/event_bridge.o build/transport_bridge.o build/listener_bridge.o
	$(HEW) build tests/event_boundary.hew --release -o $@ --link-lib $(CURDIR)/build/event_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/listener_bridge.o --link-lib -lcrypto
.PHONY: event-boundary-test
build/bot-signal-checks: tests/bot_signal.cpp ../../firmware/runtime/BotSignal.h | build
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -DMESHCORE_HOST_BOT_SOURCE=1 $< -pthread -o $@
.PHONY: bot-signal-test
bot-signal-test: build/bot-signal-checks
	timeout -k 5 10 ./build/bot-signal-checks
build/event-native-checks: tests/event_boundary.c event_bridge.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -pthread
event-boundary-test: compiler-version build/event-boundary build/event-native-checks
	timeout -k 5 10 ./build/event-native-checks
	timeout -k 5 10 ./build/event-boundary
build/transport_supervision.o: transport_bridge.c build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DMC_SUPERVISION_TEST -c $< -o $@
build/hew-host-supervision: service_main.hew service.hew session.hew transport.hew wire.hew roles.hew storage.hew worker_identity.hew $(HOSTLIB) build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build service_main.hew -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
build/hew-host-supervision-release: service_main.hew service.hew session.hew transport.hew wire.hew roles.hew storage.hew worker_identity.hew $(HOSTLIB) build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build service_main.hew --release -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK))
build/roles: main.hew wire.hew roles.hew storage.hew modem.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build main.hew -o $@ $(LINK)
build/hew-host: service_main.hew service.hew session.hew transport.hew wire.hew roles.hew storage.hew worker_identity.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build service_main.hew -o $@ $(LINK)
build/session-checks: session_checks.hew session.hew transport.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build session_checks.hew -o $@ $(LINK)
build/protocol-state-checks: tests/protocol_state.hew companion_telemetry.hew wire.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< -o $@ $(LINK)
build/protocol-state-checks-release: tests/protocol_state.hew companion_telemetry.hew wire.hew $(HOSTLIB) $(MESHPROTO) build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build $< --release -o $@ $(LINK)
.PHONY: protocol-state-test
protocol-state-test: build/session-checks build/session-checks-release build/protocol-state-checks build/protocol-state-checks-release
	./build/session-checks
	./build/session-checks-release
	./build/protocol-state-checks
	./build/protocol-state-checks-release
build/worker-checks: worker_checks.hew transport.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build worker_checks.hew -o $@ $(LINK)
build/roles build/roles-release build/roles-interrupt build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release build/session-checks build/worker-checks: $(HOSTLIB) $(MESHPROTO)
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: observer_runtime.hew observer_client.hew observer_queue.hew build/outbound_bridge.o
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: LINK += $(OBSERVER_LINK)
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: owner_control.hew host_admin.hew host_health.hew build/owner_bridge.o
build/hew-host build/hew-host-release build/hew-host-supervision build/hew-host-supervision-release: LINK += --link-lib $(CURDIR)/build/owner_bridge.o
build/management-checks: management_checks.hew $(HOSTLIB) $(MESHPROTO) | build
	$(HEW) build management_checks.hew -o $@
build/management-checks-release: management_checks.hew $(HOSTLIB) $(MESHPROTO) | build
	$(HEW) build management_checks.hew --release -o $@
build/mqtt-checks: mqtt_checks.hew hostlib/mqtt.hew hostlib/binary.hew | build
	$(HEW) build mqtt_checks.hew -o $@
build/mqtt-checks-release: mqtt_checks.hew hostlib/mqtt.hew hostlib/binary.hew | build
	$(HEW) build mqtt_checks.hew --release -o $@
build/observation-checks: observation_checks.hew meshproto/observation.hew hostlib/mqtt.hew hostlib/binary.hew | build
	$(HEW) build observation_checks.hew -o $@
build/observation-checks-release: observation_checks.hew meshproto/observation.hew hostlib/mqtt.hew hostlib/binary.hew | build
	$(HEW) build observation_checks.hew --release -o $@
observation-test: build/observation-checks build/observation-checks-release
	MESHCORE_HEW_OBSERVATION_BIN=$(CURDIR)/build/observation-checks go test ../../internal/observer -run '^TestHewObservation' -count=1
	MESHCORE_HEW_OBSERVATION_BIN=$(CURDIR)/build/observation-checks-release go test ../../internal/observer -run '^TestHewObservation' -count=1
build/observer-auth-checks: observer_auth_checks.hew meshproto/observer_auth.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build observer_auth_checks.hew -o $@ $(LINK)
build/observer-auth-checks-release: observer_auth_checks.hew meshproto/observer_auth.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build observer_auth_checks.hew --release -o $@ $(LINK)
observer-auth-test: build/observer-auth-checks build/observer-auth-checks-release
	MESHCORE_HEW_OBSERVER_AUTH_BIN=$(CURDIR)/build/observer-auth-checks go test ../../internal/observer -run '^TestHewObserverIdentity' -count=1
	MESHCORE_HEW_OBSERVER_AUTH_BIN=$(CURDIR)/build/observer-auth-checks-release go test ../../internal/observer -run '^TestHewObserverIdentity' -count=1
build/outbound_bridge.o: outbound_bridge.c event_bridge.h build/event_bridge.o | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $$(pkg-config --cflags libcurl) -c $< -o $@
build/outbound-checks: outbound_checks.hew hostlib/outbound.hew hostlib/events.hew hostlib/binary.hew build/outbound_bridge.o build/transport_bridge.o
	$(HEW) build outbound_checks.hew -o $@ --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcurl --link-lib -lcrypto
build/outbound-checks-release: outbound_checks.hew hostlib/outbound.hew hostlib/events.hew hostlib/binary.hew build/outbound_bridge.o build/transport_bridge.o
	$(HEW) build outbound_checks.hew --release -o $@ --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib $(CURDIR)/build/transport_bridge.o --link-lib $(CURDIR)/build/event_bridge.o --link-lib -lcurl --link-lib -lcrypto
build/observer-client-checks: observer_client_checks.hew observer_client.hew observer_queue.hew $(HOSTLIB) $(MESHPROTO) build/outbound_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build observer_client_checks.hew -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK)) --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
build/observer-client-checks-release: observer_client_checks.hew observer_client.hew observer_queue.hew $(HOSTLIB) $(MESHPROTO) build/outbound_bridge.o build/crypto_bridge.o build/storage_bridge.o build/transport_supervision.o
	$(HEW) build observer_client_checks.hew --release -o $@ $(subst transport_bridge.o,transport_supervision.o,$(LINK)) --link-lib $(CURDIR)/build/outbound_bridge.o --link-lib -lcurl
build/observer-queue-checks: observer_queue_checks.hew observer_queue.hew $(HOSTLIB) $(MESHPROTO) build/event_bridge.o | build
	$(HEW) build observer_queue_checks.hew -o $@ --link-lib $(CURDIR)/build/event_bridge.o
build/observer-queue-checks-release: observer_queue_checks.hew observer_queue.hew $(HOSTLIB) $(MESHPROTO) build/event_bridge.o | build
	$(HEW) build observer_queue_checks.hew --release -o $@ --link-lib $(CURDIR)/build/event_bridge.o
observer-client-test: build/observer-client-checks build/observer-client-checks-release build/observer-queue-checks build/observer-queue-checks-release build/outbound-checks build/outbound-checks-release
	./build/observer-queue-checks
	./build/observer-queue-checks-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_client.py
	MESHCORE_HEW_OBSERVER_CLIENT_BIN=$(CURDIR)/build/observer-client-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_client.py
observer-test: mqtt-test outbound-test observation-test observer-auth-test observer-client-test
build/endpoint-checks: endpoint_checks.hew meshproto/mqtt_endpoint.hew | build
	$(HEW) build endpoint_checks.hew -o $@
build/endpoint-checks-release: endpoint_checks.hew meshproto/mqtt_endpoint.hew | build
	$(HEW) build endpoint_checks.hew --release -o $@
observer-test: build/endpoint-checks build/endpoint-checks-release
	./build/endpoint-checks
	./build/endpoint-checks-release
build/std-tls-deadline-probe: tests/std_tls_deadline_probe.hew | build
	$(HEW) build $< -o $@
build/std-tls-deadline-probe-release: tests/std_tls_deadline_probe.hew | build
	$(HEW) build $< --release -o $@
mqtt-test: build/mqtt-checks build/mqtt-checks-release
	MESHCORE_HEW_MQTT_BIN=$(CURDIR)/build/mqtt-checks go test ../../internal/observer -run '^TestHewMQTT' -count=1
	MESHCORE_HEW_MQTT_BIN=$(CURDIR)/build/mqtt-checks-release go test ../../internal/observer -run '^TestHewMQTT' -count=1
outbound-test: build/outbound-checks build/outbound-checks-release build/std-tls-deadline-probe build/std-tls-deadline-probe-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/outbound.py
	MESHCORE_HEW_OUTBOUND_BIN=$(CURDIR)/build/outbound-checks-release MESHCORE_HEW_STD_TLS_BIN=$(CURDIR)/build/std-tls-deadline-probe-release PYTHONDONTWRITEBYTECODE=1 python3 tests/outbound.py
build/library-fixture.o: tests/library_fixture.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
build/child-fixture: tests/child_fixture.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@
build/library-checks: library_checks.hew $(HOSTLIB) build/library-fixture.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build library_checks.hew -o $@ $(LINK) --link-lib $(CURDIR)/build/library-fixture.o
build/library-checks-release: library_checks.hew $(HOSTLIB) build/library-fixture.o build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build library_checks.hew --release -o $@ $(LINK) --link-lib $(CURDIR)/build/library-fixture.o
build/runtime-contract: tests/runtime_contract.hew | build
	$(HEW) build $< -o $@
build/runtime-contract-release: tests/runtime_contract.hew | build
	$(HEW) build $< --release -o $@
build/supervised-resource-contract: tests/supervised_resource_contract.hew | build
	$(HEW) build $< -o $@
build/supervised-resource-contract-release: tests/supervised_resource_contract.hew | build
	$(HEW) build $< --release -o $@
build/std-tcp-write-probe: tests/std_tcp_write_probe.hew | build
	$(HEW) build $< -o $@
build/std-tcp-write-probe-release: tests/std_tcp_write_probe.hew | build
	$(HEW) build $< --release -o $@
build/std-net-select-probe: tests/std_net_select_probe.hew | build
	$(HEW) build $< -o $@
build/std-net-select-probe-release: tests/std_net_select_probe.hew | build
	$(HEW) build $< --release -o $@
build/roles-release: main.hew wire.hew roles.hew storage.hew modem.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build main.hew --release -o $@ $(LINK)
build/hew-host-release: service_main.hew service.hew session.hew transport.hew wire.hew roles.hew storage.hew worker_identity.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build service_main.hew --release -o $@ $(LINK)
build/oracle: tests/oracle/main.go | build
	TMPDIR=$(CURDIR)/build go build -o $@ ./tests/oracle
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
	TMPDIR=$(CURDIR)/build clang --target=wasm32 -Oz -nostdlib -fno-builtin \
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
build/session-checks-release: session_checks.hew session.hew transport.hew wire.hew build/crypto_bridge.o build/storage_bridge.o build/transport_bridge.o
	$(HEW) build session_checks.hew --release -o $@ $(LINK)
modem-diagnostics-test: all build/oracle build/session-checks build/session-checks-release build/hew-host-release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py
	MESHCORE_HEW_HOST=$(CURDIR)/build/hew-host-release MESHCORE_HEW_SESSION_CHECKS=$(CURDIR)/build/session-checks-release PYTHONDONTWRITEBYTECODE=1 python3 tests/modem_diagnostics.py
