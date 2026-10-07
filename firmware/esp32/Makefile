ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
UPSTREAM ?= $(ROOT)/.tmp/onchip-upstream
BUILD ?= $(ROOT)/.tmp/onchip-MeshCore
CONFIG ?=
ENV ?= Xiao_S3_WIO_onchip
include $(ROOT)/release/versions.mk
PIN := $(MESHCORE_RELEASE_SHA)
.DEFAULT_GOAL := test
.PHONY: node-backup-test
.PHONY: observer-config-test
observer-config-test: prepare
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p "$(TEST_BUILD)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-DONCHIP_OBSERVER_CONFIG_TEST=1 \
		-DONCHIP_MQTT_URI='"mqtt://user:secret%2F%40%3Apass@test-broker:11883"' \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(CRYPTO) \
		tests/observer_config.cpp ObserverConfig.cpp $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		-o "$(TEST_BUILD)/observer-config"
	"$(TEST_BUILD)/observer-config"
node-backup-test:
	@mkdir -p "$(TEST_BUILD)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) -I$(CRYPTO) -DMESHCORE_NODE_BACKUP=1 \
		tests/node_backup_archive.cpp ../runtime/NodeBackup.cpp $(addprefix $(CRYPTO)/,AES128.cpp AESCommon.cpp BlockCipher.cpp SHA256.cpp Hash.cpp Crypto.cpp) \
		-o "$(TEST_BUILD)/node-backup"
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$(ROOT)" NODE_BACKUP_RUNNER="$(TEST_BUILD)/node-backup" \
		python3 -m unittest -v test_support.test_node_backup
PUBLIC_UPDATE_SOURCE ?= EspFieldUpdate.cpp
.PHONY: public-update-test
public-update-test:
	@mkdir -p "$(TEST_BUILD)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-DARDUINO_ARCH_ESP32 -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_ESP_UPDATE_TEST \
		-DMESHCORE_PUBLIC_PROVISIONING=1 -I. \
		tests/public_update.cpp "$(PUBLIC_UPDATE_SOURCE)" -lcrypto \
		-o "$(TEST_BUILD)/public-update"
	ulimit -c 0; "$(TEST_BUILD)/public-update"
	ulimit -c 0; "$(TEST_BUILD)/public-update" different-owner
.PHONY: public-provisioning-test
public-provisioning-test:
	$(PHY_MAKE) prepare $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/SHA256.o \
		$(PHY_BUILD)/crypto/Hash.o $(PHY_BUILD)/crypto/Crypto.o
	@mkdir -p "$(TEST_BUILD)"
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$(ROOT):.:tests python3 -m unittest -v test_public_setup
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$(ROOT):.:tests python3 -c \
		'import json, public_setup; from test_public_setup import profile; from pathlib import Path; Path("$(TEST_BUILD)/public-setup-test.bin").write_bytes(public_setup.encode_profile(json.dumps(profile())))'
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-DMESHCORE_PUBLIC_PROVISIONING=1 -DMESHCORE_ONCHIP_BOT=1 \
		-Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(UPSTREAM)/lib/ed25519 -I$(CRYPTO) \
		-I$(UPSTREAM)/examples/kiss_modem \
		-I$(LPP) \
		tests/public_provisioning.cpp Provisioning.cpp RoleProfile.cpp Lifecycle.cpp ../runtime/BotSettings.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/SHA256.o \
		$(PHY_BUILD)/crypto/Hash.o $(PHY_BUILD)/crypto/Crypto.o \
		-o "$(TEST_BUILD)/public-provisioning"
	"$(TEST_BUILD)/public-provisioning" "$(TEST_BUILD)/public-setup-test.bin"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-DONCHIP_ADMIN_PASSWORD='"private-admin"' -DONCHIP_ROOM_PASSWORD='"private-room"' \
		-DONCHIP_MAST_PASSWORD='"private-mast"' -DONCHIP_OPERATOR_PUBKEY='"private-authority"' \
		-Itests/seams tests/public_fallback.cpp RoleProfile.cpp \
		-o "$(TEST_BUILD)/public-fallback"
	"$(TEST_BUILD)/public-fallback"
.PHONY: esp-field-update-test
esp-field-update-test:
	@mkdir -p "$(ROOT)/.tmp/onchip-esp-field-update-tests"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-DARDUINO_ARCH_ESP32 -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_ESP_UPDATE_TEST \
		-DONCHIP_OPERATOR_PUBKEY='"3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29"' \
		tests/esp_update.cpp EspFieldUpdate.cpp -lcrypto \
		-o "$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update"
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update"
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" unsupported-bootloader
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" recovery
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" automatic-recovery
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" running-hash
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" running-hash-error
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" rollback-error
	"$(ROOT)/.tmp/onchip-esp-field-update-tests/esp-update" automatic-rollback-error
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$(ROOT):.:tests python3 -m unittest -v test_esp_field_update tools.hardware.tests.test_esp32_slot
.PHONY: prepare firmware buildfs provision-fs test local-test python-test mast-phy-test lifecycle-test clock-network-test
prepare:
	$(MAKE) -C $(ROOT) onchip-upstream
	python3 prepare.py --upstream $(UPSTREAM) --target $(BUILD) --check-target
	@test "$$(git -C $(UPSTREAM) rev-parse HEAD)" = "$(PIN)"
	@install -d -m 700 "$(BUILD)"
	git -C $(UPSTREAM) archive HEAD | tar -x -C $(BUILD)
	git -C $(BUILD) apply $(ROOT)/firmware/shared/radio-reconfigure.patch
	git -C $(BUILD) apply $(ROOT)/firmware/shared/queued-dispatch.patch
	cp $(ROOT)/firmware/esp32/wifi_kiss_main.cpp $(BUILD)/examples/kiss_modem/main.cpp
	cp $(ROOT)/firmware/shared/WifiKissMultiplexer.* $(ROOT)/firmware/shared/QueuedTxProtocol.h \
		$(ROOT)/firmware/shared/RadioDashboard.* $(ROOT)/firmware/shared/RadioDashboardPage.h \
		$(ROOT)/firmware/shared/RadioFirmwareIdentity.h \
		$(ROOT)/firmware/shared/RadioNetwork.h $(BUILD)/examples/kiss_modem/
	python3 prepare.py --upstream $(UPSTREAM) --target $(BUILD)
	python3 -B prepare_wasm.py --target $(BUILD) --enabled $(ONCHIP_BOT_WASM) --config-only
	@if test -n "$(CONFIG)"; then install -m 600 "$(CONFIG)" "$(BUILD)/platformio.local.ini"; \
	elif ! test -f "$(BUILD)/platformio.local.ini"; then \
		install -m 600 platformio.ini.example "$(BUILD)/platformio.local.ini"; fi
firmware: prepare
	cd $(BUILD) && TMPDIR=$(ROOT)/.tmp pio run -e $(ENV)
.PHONY: field-rebuild
field-rebuild:
	python3 prepare.py --upstream "$(UPSTREAM)" --target "$(BUILD)" --check-target
	@test -f "$(BUILD)/platformio.local.ini" -a -f "$(BUILD)/examples/kiss_modem/onchip/BuildClock.h" || \
		{ echo "A previously prepared field project is required." >&2; exit 1; }
	@if test -n "$(CONFIG)"; then install -m 600 "$(CONFIG)" "$(BUILD)/platformio.local.ini"; fi
	cp $(filter-out wifi_kiss_main.cpp,$(wildcard *.cpp *.h)) ../runtime/*.cpp ../runtime/*.h "$(BUILD)/examples/kiss_modem/onchip/"
	cp ../shared/RadioDashboard.cpp ../shared/RadioDashboard.h ../shared/RadioDashboardPage.h "$(BUILD)/examples/kiss_modem/"
	python3 prepare.py --upstream "$(UPSTREAM)" --target "$(BUILD)" --field-network
	TMPDIR="$(ROOT)/.tmp" python3 -B prepare_wasm.py --target "$(BUILD)" --enabled $(ONCHIP_BOT_WASM)
	cd "$(BUILD)" && TMPDIR="$(ROOT)/.tmp" pio run -e "$(ENV)"
buildfs:
	python3 prepare.py --upstream "$(UPSTREAM)" --target "$(BUILD)" --check-target
	@test -f "$(BUILD)/platformio.local.ini" -a -f "$(BUILD)/onchip-data/onchip-layout" || \
		{ echo "Run the parent onchip-prepare target with the private operator profile first." >&2; exit 1; }
	cd "$(BUILD)" && TMPDIR="$(ROOT)/.tmp" PLATFORMIO_DATA_DIR="$(BUILD)/onchip-data" pio run -e "$(ENV)" -t buildfs
provision-fs:
	@test "$(CONFIRM_FS_ERASE)" = "erase-spiffs" -a -n "$(UPLOAD_PORT)" || \
		{ echo "Provisioning erases all SPIFFS role data. Set CONFIRM_FS_ERASE=erase-spiffs and UPLOAD_PORT explicitly." >&2; exit 1; }
	$(MAKE) buildfs
	cd "$(BUILD)" && TMPDIR="$(ROOT)/.tmp" PLATFORMIO_DATA_DIR="$(BUILD)/onchip-data" pio run -e "$(ENV)" -t uploadfs --upload-port "$(UPLOAD_PORT)"
TEST_FLAGS ?=
override TEST_FLAGS += -I$(ROOT)/firmware/runtime -I$(ROOT)/firmware/esp32 -I$(ROOT)/firmware/shared
PHY_TEST_FLAGS := $(filter -fsanitize=% -fno-%,$(TEST_FLAGS))
# Pinned ref10 C deliberately shifts signed carries; keep all other checks.
PHY_C_FLAGS := $(PHY_TEST_FLAGS) -fno-sanitize=shift-base
TEST_VARIANT := $(shell printf '%s' '$(subst ','"'"',$(CC) $(CXX) $(TEST_FLAGS) $(PHY_C_FLAGS))' | sha256sum | cut -c1-12)
TEST_BUILD := $(ROOT)/.tmp/onchip-tests/$(TEST_VARIANT)
PHY_BUILD := $(ROOT)/.tmp/onchip-phy/$(TEST_VARIANT)
PHY_MAKE = $(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile BUILD=$(PHY_BUILD) \
	CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder -ffunction-sections -fdata-sections $(PHY_TEST_FLAGS)' \
	CC='$(CC) $(PHY_C_FLAGS)'
STREAM_BUILD := $(ROOT)/.tmp/onchip-radio-stream-tests
DASHBOARD_BUILD := $(ROOT)/.tmp/onchip-dashboard-tests
CRYPTO := $(ROOT)/.tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi/Crypto
LPP := $(ROOT)/.tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi/CayenneLPP/src
BASE64 ?= $(ROOT)/.tmp/native-test-deps/base64/src
test: local-test lifecycle-test clock-network-test syslog-test
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile BUILD=$(STREAM_BUILD) \
		CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder -ffunction-sections -fdata-sections -DKISS_MAX_TCP_CLIENTS=4 -DONCHIP_COMPANION_MAX_CLIENTS=2' \
		stream-test
	@mkdir -p $(DASHBOARD_BUILD)
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile BUILD=$(DASHBOARD_BUILD) \
		CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder -ffunction-sections -fdata-sections -DKISS_MAX_TCP_CLIENTS=4 -DMESHCORE_ONCHIP=1 -DONCHIP_COMPANION_MAX_CLIENTS=2' \
		$(DASHBOARD_BUILD)/dashboard-http
	$(DASHBOARD_BUILD)/dashboard-http
python-test: bot-host-runner
	PYTHONDONTWRITEBYTECODE=1 BOT_HOST_RUNNER="$(BOT_HOST_RUNNER)" python3 -m unittest discover -s tests -p 'test_*.py' -v
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_*.py' -v

local-test: prepare mast-phy-test python-test
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	$(PHY_BUILD)/combined
	@mkdir -p $(TEST_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder $(TEST_FLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -DKISS_LOCAL_SOURCES=4 \
		-DKISS_MAX_TCP_CLIENTS=4 -DKISS_STREAM_ENDPOINT=1 \
		-I$(ROOT)/test_support/phy_parity/seams \
		-I$(BUILD)/src -I$(UPSTREAM)/examples/kiss_modem -I$(UPSTREAM)/lib/ed25519 \
		-I$(BUILD)/examples/kiss_modem/onchip \
		-I$(CRYPTO) -I$(LPP) -I$(ROOT)/firmware/shared -I$(ROOT)/test_support \
		tests/behaviour.cpp $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp \
		$(ROOT)/firmware/shared/RadioDashboard.cpp $(UPSTREAM)/examples/kiss_modem/KissModem.cpp \
		$(BUILD)/src/Dispatcher.cpp $(BUILD)/src/Packet.cpp \
		$(BUILD)/src/helpers/StaticPoolPacketManager.cpp \
		$(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		$(PHY_BUILD)/ed/*.o $(PHY_BUILD)/CayenneLPP.o $(PHY_BUILD)/CayenneLPPPolyline.o \
		-o $(TEST_BUILD)/behaviour
	$(TEST_BUILD)/behaviour

mast-phy-test: prepare
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(TEST_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder $(TEST_FLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-DMESHCORE_ONCHIP=1 -DKISS_LOCAL_SOURCES=4 -DKISS_MAX_TCP_CLIENTS=4 \
		-I$(ROOT)/test_support/phy_parity/seams \
		-I$(BUILD)/src -I$(UPSTREAM)/examples/kiss_modem -I$(UPSTREAM)/lib/ed25519 \
		-I$(BUILD)/examples/kiss_modem/onchip \
		-I$(CRYPTO) -I$(LPP) -I$(ROOT)/firmware/shared -I$(ROOT)/test_support \
		tests/mast_phy.cpp $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp \
		$(ROOT)/firmware/shared/RadioDashboard.cpp $(UPSTREAM)/examples/kiss_modem/KissModem.cpp \
		$(BUILD)/src/Dispatcher.cpp $(BUILD)/src/Packet.cpp \
		$(BUILD)/src/helpers/StaticPoolPacketManager.cpp \
		$(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		$(PHY_BUILD)/ed/*.o $(PHY_BUILD)/CayenneLPP.o $(PHY_BUILD)/CayenneLPPPolyline.o \
		-o $(TEST_BUILD)/mast-phy
	$(TEST_BUILD)/mast-phy

NATIVE := $(BUILD)/examples/kiss_modem/onchip
NATIVE_HELPERS := AdvertDataHelpers BaseChatMesh ClientACL CommonCLI ConfigSerializer \
	IdentityStore RegionMap TransportKeyStore TxtDataHelpers
clock-network-test: prepare
	@mkdir -p $(TEST_BUILD)
	@for server in ntp.test.invalid ''; do \
		$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
			-DARDUINO_ARCH_ESP32 -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
			-DONCHIP_SNTP_SERVER="\"$$server\"" \
			-Itests/seams -I$(ROOT)/test_support/phy_parity/seams/http \
			-I$(BUILD)/src -I$(NATIVE) \
			tests/clock_network.cpp $(NATIVE)/Clock.cpp \
			-o $(TEST_BUILD)/clock-network && $(TEST_BUILD)/clock-network || exit; \
	done
.PHONY: radio-time-test
syslog-test:
	@mkdir -p $(TEST_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror tests/syslog.cpp -o $(TEST_BUILD)/syslog
	$(TEST_BUILD)/syslog
.PHONY: syslog-test
radio-time-test:
	@mkdir -p $(TEST_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra tests/radio_time_protocol.cpp -o $(TEST_BUILD)/radio-time
	$(TEST_BUILD)/radio-time
.PHONY: radio-time-native-test
radio-time-native-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 RADIO_TIME_ONLY=1 ONCHIP_BOT_WASM=0 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
.PHONY: admin-cli-core-test automatic-adverts-test
automatic-adverts-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 AUTOMATIC_ADVERTS_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
admin-cli-core-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 ADMIN_CLI_CORE_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
.PHONY: runtime-config-admin-test
runtime-config-admin-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 ADMIN_CLI_CORE_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1 -DONCHIP_RUNTIME_CONFIG_ADMIN_TEST=1'
lifecycle-test: prepare $(if $(BOT_INTEGRATION),bot-lua) $(if $(ADMIN_INTEGRATION),$(if $(or $(ADAPTIVE_ADMIN_ONLY),$(ADMIN_CLI_CORE_ONLY),$(AUTOMATIC_ADVERTS_ONLY)),,$(if $(filter 1,$(ONCHIP_BOT_WASM)),bot-wasm-examples)))
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(TEST_BUILD)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/rf_vector.py $(TEST_BUILD)/python-signer.bin
	PYTHONDONTWRITEBYTECODE=1 python3 tests/rf_vector.py query $(TEST_BUILD)/python-query.bin
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder $(TEST_FLAGS) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -pthread \
		-DESP32 -DMESHCORE_ONCHIP=1 -DONCHIP_OBSERVER_CONFIG_TEST=1 -DCOMPANION_SESSIONS_HOST -DKISS_LOCAL_SOURCES=$(if $(BOT_INTEGRATION),5,4) -DKISS_MAX_TCP_CLIENTS=4 \
		-DCONFIG_LWIP_MAX_SOCKETS=16 \
		-DMAX_CONTACTS=256 -DMAX_GROUP_CHANNELS=8 \
		-DONCHIP_ADMIN_PASSWORD='"test-admin"' -DONCHIP_ROOM_PASSWORD='"test-room"' \
		-DONCHIP_MAST_PASSWORD='"mast-pass-12"' -DONCHIP_MAST_WEB_HOST='"mast.test"' \
		-DONCHIP_SERVICE_REGION='"#beta-lab"' \
		-DONCHIP_TRUSTED_COMPANION_PUBKEY='"03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"' \
		-DONCHIP_MQTT_URI='"mqtt://test-broker"' \
		-DONCHIP_RADIO_FREQ_MHZ=912.525 -DONCHIP_RADIO_BW_KHZ=250 \
		-DONCHIP_RADIO_SF=7 -DONCHIP_RADIO_CR=5 -DONCHIP_RADIO_TX_POWER=2 \
		-DLORA_FREQ=912.525 -DLORA_BW=250 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=2 \
		-DFIRMWARE_VERSION='"test"' -DFIRMWARE_BUILD_DATE='"2026-01-01"' \
		-DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(BUILD)/src -I$(UPSTREAM)/examples/kiss_modem -I$(UPSTREAM)/lib/ed25519 \
		-I$(NATIVE) -I$(CRYPTO) -I$(LPP) -I$(ROOT)/firmware/shared -I$(ROOT)/test_support \
		-I$(BASE64) $(BOT_LIFECYCLE_FLAGS) \
		$(if $(SIGNED_INTEGRATION),tests/lifecycle.cpp,$(if $(ADMIN_INTEGRATION),tests/beta_native.cpp,$(if $(BOT_INTEGRATION),tests/bot_native.cpp,tests/lifecycle.cpp))) $(BOT_LIFECYCLE_SOURCES) \
		$(NATIVE)/Repeater.cpp $(NATIVE)/Room.cpp $(NATIVE)/Companion.cpp \
		$(NATIVE)/Lifecycle.cpp $(NATIVE)/RoleIdentity.cpp $(NATIVE)/RoleProfile.cpp $(NATIVE)/Provisioning.cpp $(NATIVE)/Management.cpp \
		$(NATIVE)/CompanionSessions.cpp $(NATIVE)/Runtime.cpp $(NATIVE)/Syslog.cpp \
		$(NATIVE)/Observer.cpp $(NATIVE)/ObserverConfig.cpp $(NATIVE)/ObserverWire.cpp $(NATIVE)/Clock.cpp \
		$(NATIVE)/MastAdmin.cpp $(NATIVE)/MastSource.cpp $(NATIVE)/MastWeb.cpp \
		$(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp \
		$(BUILD)/src/Dispatcher.cpp $(BUILD)/src/Mesh.cpp $(BUILD)/src/Packet.cpp \
		$(addprefix $(BUILD)/src/helpers/,$(addsuffix .cpp,$(NATIVE_HELPERS))) \
		$(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		$(PHY_BUILD)/ed/*.o $(PHY_BUILD)/CayenneLPP.o $(PHY_BUILD)/CayenneLPPPolyline.o \
		-o $(TEST_BUILD)/lifecycle
	BOT_WASM_CREDENTIAL_MODULE="$(ROOT)/.tmp/wasm-examples/c-admin.wasm" \
		$(TEST_BUILD)/lifecycle $(if $(AUTOMATIC_ADVERTS_ONLY),--automatic-adverts-test,$(if $(ADMIN_CLI_CORE_ONLY),--admin-cli-core-test,$(if $(RADIO_TIME_ONLY),--radio-time-test,$(if $(OBSERVER_PUBLIC_ONLY),--observer-public-test,$(if $(ADAPTIVE_ADMIN_ONLY),--adaptive-admin-test,$(if $(NATIVE_DISCOVERY_ONLY),--native-discovery-test,$(TEST_BUILD)/roles.json $(TEST_BUILD)/python-signer.bin $(TEST_BUILD)/native-receipt.bin $(TEST_BUILD)/python-query.bin $(TEST_BUILD)/native-status.bin))))))
ifeq ($(OBSERVER_PUBLIC_ONLY),)
ifeq ($(if $(SIGNED_INTEGRATION),,$(BOT_INTEGRATION)),)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/rf_vector.py verify $(TEST_BUILD)/python-signer.bin $(TEST_BUILD)/native-receipt.bin
	PYTHONDONTWRITEBYTECODE=1 python3 tests/rf_vector.py verify-status $(TEST_BUILD)/python-query.bin $(TEST_BUILD)/native-status.bin 2
ifeq ($(SIGNED_INTEGRATION),)
	node tests/dashboard_roles.js $(ROOT)/firmware/shared/RadioDashboardPage.h $(TEST_BUILD)/roles.json
endif
endif
endif

.PHONY: observer-public-test
observer-public-test:
	$(MAKE) lifecycle-test OBSERVER_PUBLIC_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DONCHIP_CLOCK_TEST_BUSY -DONCHIP_MQTT_FORMAT=1 -DONCHIP_MQTT_IATA="\"YYC\"" -DONCHIP_MQTT_AUDIENCE="\"internal-observer-test\""'
	$(MAKE) lifecycle-test OBSERVER_PUBLIC_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DONCHIP_CLOCK_TEST_BUSY -DONCHIP_MQTT_FORMAT=2 -DONCHIP_MQTT_IATA="\"YYC\"" -DONCHIP_MQTT_AUDIENCE="\"internal-observer-test\""'

.PHONY: observer-wire-test
observer-wire-test:
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(TEST_BUILD)/observer-wire
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-I$(ROOT)/test_support/phy_parity/seams -I$(UPSTREAM)/src \
		-I$(UPSTREAM)/lib/ed25519 -I$(BASE64) -I$(CRYPTO) \
		tests/observer_wire.cpp ObserverWire.cpp $(UPSTREAM)/src/Packet.cpp \
		$(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		$(PHY_BUILD)/ed/*.o -o $(TEST_BUILD)/observer-wire/test
	$(TEST_BUILD)/observer-wire/test $(TEST_BUILD)/observer-wire "$$(date +%s)"
	PYTHONDONTWRITEBYTECODE=1 python3 tests/observer_upstream_contract.py $(TEST_BUILD)/observer-wire
	cd $(ROOT) && TMPDIR="$(ROOT)/.tmp" \
		MESHCORE_OBSERVER_FIXTURES="$(TEST_BUILD)/observer-wire" \
		go test ./internal/observer -run TestNativeObserverFixture -count=1

.PHONY: beta-signed-test
beta-signed-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION= ADMIN_INTEGRATION=
	$(MAKE) lifecycle-test TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DONCHIP_SIGNED_ONLY_TEST=1' BOT_INTEGRATION=1 ADMIN_INTEGRATION= SIGNED_INTEGRATION=1

include bot.mk

.PHONY: telemetry-test telemetry-endpoint-test
telemetry-test:
	@mkdir -p $(TEST_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(TEST_FLAGS) \
		-DMESHCORE_ONCHIP=1 -DMESHCORE_ONCHIP_BOT=1 -DKISS_MAX_TCP_CLIENTS=3 \
		-I. -I../runtime -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		tests/telemetry.cpp Telemetry.cpp TelemetrySettings.cpp TelemetryAdmin.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp \
		-o $(TEST_BUILD)/telemetry
	$(TEST_BUILD)/telemetry
telemetry-endpoint-test: telemetry-test
	@test -n "$(TELEMETRY_WRITE_URL)" || { echo "Set TELEMETRY_WRITE_URL to an authorized HTTPS write endpoint." >&2; exit 1; }
	$(TEST_BUILD)/telemetry --fixture > $(TEST_BUILD)/telemetry.influx
	PYTHONDONTWRITEBYTECODE=1 python3 tests/telemetry_endpoint.py "$(TELEMETRY_WRITE_URL)" $(TEST_BUILD)/telemetry.influx

.PHONY: telemetry-https-test telemetry-deployment-test
telemetry-https-test: prepare bot-prepare-phy
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(TEST_BUILD)
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_JSON_INCLUDE) \
		tests/telemetry_https.cpp TelemetryEndpoint.cpp TelemetryHttps.cpp ../runtime/BotHttps.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o \
		$(BOT_JSON_LIBS) -o $(TEST_BUILD)/telemetry-https
	$(TEST_BUILD)/telemetry-https
.PHONY: telemetry-storage-test
telemetry-storage-test: telemetry-https-test
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 -DONCHIP_TELEMETRY_TEST=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_JSON_INCLUDE) \
		tests/telemetry_storage.cpp TelemetryEndpoint.cpp ../runtime/BotHttps.cpp ../runtime/BotStore.cpp ../runtime/BotTypes.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) \
		-o $(TEST_BUILD)/telemetry-storage
	$(TEST_BUILD)/telemetry-storage
telemetry-deployment-test:
	@mkdir -p $(ROOT)/.tmp
	TMPDIR=$(ROOT)/.tmp PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_esp32_update.py' -v
