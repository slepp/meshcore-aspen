MESHCORE_REF ?= companion-v1.17.1
.DEFAULT_GOAL := help
MESHCORE_DIR := .tmp/MeshCore
PHYLESS_DIR := .tmp/phyless-MeshCore
NRF52_DIR := .tmp/nrf52-MeshCore
NRF52_ENV ?= Xiao_nrf52_local_companion
NRF52_PORT ?=
ONCHIP_UPSTREAM := .tmp/onchip-upstream
ONCHIP_DIR := .tmp/onchip-MeshCore
ONCHIP_PORT ?=
FIRMWARE_ENV := Xiao_S3_WIO_kiss_wifi
NATIVE_LIB_DIR := $(MESHCORE_DIR)/.pio/libdeps/$(FIRMWARE_ENV)
NATIVE_USB_LIB_DIR := .tmp/native-test-deps
PHYLESS_ENV ?= Phyless_Xiao_S3_repeater
UPLOAD_PORT ?= /dev/ttyACM0
RADIO_HOST ?= meshcore-radio.local
LIVE_TEST_SECONDS ?= 60
LIVE_CLIENTS ?= 4
DASHBOARD_CLIENTS ?= 2
GO ?= go
ESPTOOL ?= $(HOME)/.platformio/packages/tool-esptoolpy/esptool.py
HOST_CONFIG ?= meshcore-host.json
HOST_TEST_PACKAGES ?= ./...
PARITY_BUILD ?= $(CURDIR)/.tmp/parity-oracle
PEER_RADIO ?=
STOCK_SERIAL ?=
STOCK_DEVICE_MAC ?=
STOCK_CLI_ARGS ?= infos
BACKUP_DIR ?=
FIRMWARE_BACKUP ?=
FLASH_SIZE ?= 0x800000
FLASH_BAUD ?= 921600

.PHONY: help check test-native-smoke
help:
	@printf '%s\n' \
		'Contributor checks (no radio, credentials or PlatformIO required):' \
		'  make check              Python tests, Go race tests, host build and native smoke tests' \
		'  make test-python        Python monitor and broker tests' \
		'  make host-test          Go race tests (live tests require explicit environment opt-ins)' \
		'  make test-native-smoke  Local C++ firmware and storage tests' \
		'  make host-build         Build the host and check commands into bin/' \
		'  make hew-base           Build the separate native Base candidate (see experiments/hew-roles/BASE.md)' \
		'  make hew-base-test      Run synthetic Base differential/TCP/source/state checks' \
		'  make aspen-config       Create the ESP32 standalone build configuration' \
		'  make aspen-firmware     Build the configured ESP32 standalone radio' \
		'  make pine-firmware      Build the selected nRF52840 on-device profile' \
		'  make remote-radio-config  Create the ESP32 TCP role adapter configuration' \
		'  make remote-radio-firmware  Build the selected ESP32 TCP role adapter' \
		'' \
		'Radio operation is opt-in. See HOST_GUIDE.md for setup and live commands.' \
		'  make run                Start the serial monitor' \
		'  make test               Extended tests; fetch upstream/native dependencies on first use'

check:
	$(MAKE) test-python
	TMPDIR=$(CURDIR)/.tmp python3 -m unittest -v test_support.test_operator_inventory
	MKISS_DEVICE_ADDRESS= MESHCORE_OBSERVER_INTERNAL_TEST=0 MESHCORE_ASPEN_PRIVATE_VALIDATOR=0 \
		BOT_NATIVE_WORKER= BOT_NATIVE_CLOCK_TEST_WORKER= MKISS_NATIVE_HARNESS= \
		MESHCORE_POLICY_ORACLE= MESHCORE_COMPANION_ORACLE= MESHCORE_OBSERVER_FIXTURES= \
		$(MAKE) host-test HOST_TEST_PACKAGES=./...
	$(MAKE) host-build test-native-smoke
	GOOS=darwin GOARCH=arm64 CGO_ENABLED=0 $(GO) build ./internal/state

test-native-smoke:
	@mkdir -p .tmp
	$(MAKE) -f test_support/phy_parity/Makefile network-config wifi-recovery-test
	TMPDIR=$(CURDIR)/.tmp python3 -m unittest -v test_support.phy_parity.test_firmware_identity test_support.phy_parity.test_source_layout
	python3 -m unittest discover -s test_support/resource_budget -v
	$(MAKE) -C internal/nativebot test spiffs-test

.PHONY: host-build host-test host-run host-config host-install host-readiness test-host-live test-host-reconnect test-host-rf

host-build:
	@mkdir -p bin .tmp
	TMPDIR=$(CURDIR)/.tmp $(GO) build -trimpath -buildvcs=false -o bin/ ./cmd/meshcore-host ./cmd/meshcore-check ./cmd/meshcore-rf-check

host-test:
	@mkdir -p .tmp
	TMPDIR=$(CURDIR)/.tmp $(GO) test -race $(HOST_TEST_PACKAGES)

.PHONY: hew-base hew-base-test hew-base-symbols
hew-base:
	$(MAKE) -C experiments/hew-roles base-service
hew-base-test:
	$(MAKE) -C experiments/hew-roles base-local-test base-service-test
hew-base-symbols:
	$(MAKE) -C experiments/hew-roles base-symbols

.PHONY: test-mkiss-device
test-mkiss-device:
	@test -n "$(MKISS_DEVICE_ADDRESS)" || { echo "Set MKISS_DEVICE_ADDRESS to the authorized mast host:port." >&2; exit 2; }
	@mkdir -p .tmp
	MKISS_DEVICE_ADDRESS="$(MKISS_DEVICE_ADDRESS)" TMPDIR=$(CURDIR)/.tmp \
		$(GO) test -race -count=1 -v ./internal/radio -run '^TestSessionDeviceEndpoints$$'

.PHONY: parity-native parity-check
parity-native: native-test-deps
	$(MAKE) -C test_support/parity test BUILD="$(PARITY_BUILD)"

parity-check: parity-native
	MESHCORE_POLICY_ORACLE="$(PARITY_BUILD)/events.jsonl" \
	MESHCORE_COMPANION_ORACLE="$(PARITY_BUILD)/events.jsonl" $(MAKE) host-test

host-run: host-build
	./bin/meshcore-host -config "$(HOST_CONFIG)"

.PHONY: mqtt-broker mqtt-install
mqtt-broker: host-build
	./bin/meshcore-host -config "$(HOST_CONFIG)" -broker-only

mqtt-install: host-build
	@test -f "$(HOST_CONFIG)" || { echo "Select a broker configuration with HOST_CONFIG." >&2; exit 1; }
	install -Dm 755 bin/meshcore-host $(HOME)/.local/libexec/meshcore-mqtt
	install -d -m 700 $(HOME)/.config/meshcore-mqtt
	@test -e $(HOME)/.config/meshcore-mqtt/config.json || install -m 600 "$(HOST_CONFIG)" $(HOME)/.config/meshcore-mqtt/config.json
	install -Dm 644 packaging/meshcore-mqtt.service $(HOME)/.config/systemd/user/meshcore-mqtt.service
	systemctl --user daemon-reload

host-config:
	@test -e "$(HOST_CONFIG)" || install -Dm 600 meshcore-host.json.example "$(HOST_CONFIG)"

host-readiness: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)"

.PHONY: onchip-readiness
onchip-readiness: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -onchip

.PHONY: test-host-airtime
test-host-airtime: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -airtime

.PHONY: test-companion-reboot test-companion-factory-reset test-role-reboot test-role-rekey
test-companion-reboot: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -reboot

test-companion-factory-reset: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -factory-reset

test-role-reboot: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -reboot-roles

test-role-rekey: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -rekey-roles

.PHONY: test-onchip-lifecycle
test-onchip-lifecycle: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -onchip -reboot -reboot-roles

test-host-live: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)" -transmit

test-host-reconnect: host-build
	./bin/meshcore-check -config "$(HOST_CONFIG)"
	$(MAKE) firmware-reset UPLOAD_PORT="$(UPLOAD_PORT)"
	timeout 90s sh -c 'until ./bin/meshcore-check -config "$$1"; do sleep 2; done' _ "$(HOST_CONFIG)"

test-host-rf: host-build
	@test -n "$(PEER_RADIO)" || { echo "Set PEER_RADIO to the independent peer's KISS address." >&2; exit 2; }
	./bin/meshcore-rf-check -config "$(HOST_CONFIG)" -peer "$(PEER_RADIO)"

.PHONY: stock-firmware stock-firmware-upload stock-firmware-boot stock-peer-build stock-peer-probe test-stock-rf
stock-firmware:
	$(MAKE) -C test_support/stock_peer build

stock-firmware-upload:
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the MeshCore companion radio's USB device." >&2; exit 2; }
	$(MAKE) -C test_support/stock_peer upload UPLOAD_PORT="$(STOCK_SERIAL)" DEVICE_MAC="$(STOCK_DEVICE_MAC)"

stock-firmware-boot:
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the MeshCore companion radio's USB device." >&2; exit 2; }
	$(MAKE) -C test_support/stock_peer boot UPLOAD_PORT="$(STOCK_SERIAL)" DEVICE_MAC="$(STOCK_DEVICE_MAC)"

stock-peer-build:
	@mkdir -p bin .tmp
	TMPDIR=$(CURDIR)/.tmp $(GO) build -trimpath -buildvcs=false -o bin/meshcore-stock-check ./cmd/meshcore-stock-check

stock-peer-probe:
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the MeshCore companion radio's USB device." >&2; exit 2; }
	python3 test_support/stock_peer/probe.py --port "$(STOCK_SERIAL)" $(if $(filter 1,$(STOCK_RESET)),--reset)

.PHONY: stock-cli
stock-cli:
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the companion's stable serial path." >&2; exit 2; }
	meshcli -j -s "$(STOCK_SERIAL)" $(STOCK_CLI_ARGS)

test-stock-rf: stock-peer-build
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the stock peer's explicit USB device." >&2; exit 2; }
	./bin/meshcore-stock-check -config "$(HOST_CONFIG)" -serial "$(STOCK_SERIAL)"

.PHONY: test-onchip-rf test-queued-peer repair-peer-contact
test-onchip-rf: stock-peer-build
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the stock peer's explicit USB device." >&2; exit 2; }
	./bin/meshcore-stock-check -config "$(HOST_CONFIG)" -serial "$(STOCK_SERIAL)" -onchip

test-queued-peer: stock-peer-build
	@test -n "$(STOCK_SERIAL)" || { echo "Set STOCK_SERIAL to the PHY-less companion's explicit USB device." >&2; exit 2; }
	./bin/meshcore-stock-check -config "$(HOST_CONFIG)" -serial "$(STOCK_SERIAL)" -onchip -queued-peer

repair-peer-contact: stock-peer-build
	@test -n "$(STOCK_SERIAL)" -a -n "$(CONTACT_KEY)" || { echo "Set STOCK_SERIAL and the explicit CONTACT_KEY to repair." >&2; exit 2; }
	./bin/meshcore-stock-check -config "$(HOST_CONFIG)" -serial "$(STOCK_SERIAL)" -repair-contact "$(CONTACT_KEY)"

.PHONY: host-install-binaries
host-install-binaries: host-build
	install -Dm 755 bin/meshcore-host $(HOME)/.local/bin/meshcore-host
	install -Dm 755 bin/meshcore-check $(HOME)/.local/bin/meshcore-check

host-install: host-install-binaries host-config
	install -d -m 700 $(HOME)/.config/meshcore-host $(HOME)/.local/state/meshcore-host
	@test -e $(HOME)/.config/meshcore-host/config.json || install -m 600 "$(HOST_CONFIG)" $(HOME)/.config/meshcore-host/config.json
	install -Dm 644 packaging/meshcore-host.service $(HOME)/.config/systemd/user/meshcore-host.service
	systemctl --user daemon-reload

.PHONY: host-backup host-restore firmware-backup firmware-restore
host-backup:
	@test -n "$(BACKUP_DIR)" || { echo "Set BACKUP_DIR to a new private backup directory." >&2; exit 2; }
	python3 packaging/host-backup.py backup --directory "$(BACKUP_DIR)"

host-restore:
	@test -n "$(BACKUP_DIR)" || { echo "Set BACKUP_DIR to the matching host backup." >&2; exit 2; }
	python3 packaging/host-backup.py restore --directory "$(BACKUP_DIR)"

firmware-backup:
	@test -n "$(FIRMWARE_BACKUP)" || { echo "Set FIRMWARE_BACKUP to a new private flash-image path." >&2; exit 2; }
	@test ! -e "$(FIRMWARE_BACKUP)" || { echo "Refusing to overwrite an existing flash backup." >&2; exit 2; }
	umask 077; python3 "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --baud "$(FLASH_BAUD)" --after hard_reset read_flash 0 "$(FLASH_SIZE)" "$(FIRMWARE_BACKUP)"

firmware-restore:
	@test -n "$(FIRMWARE_BACKUP)" -a -f "$(FIRMWARE_BACKUP)" || { echo "Set FIRMWARE_BACKUP to the matching original flash image." >&2; exit 2; }
	python3 "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --baud "$(FLASH_BAUD)" --after hard_reset write_flash 0 "$(FIRMWARE_BACKUP)"

.PHONY: run run-tcp run-broker test test-python test-live-multiclient test-live-dashboard test-live-reception firmware-check firmware-config firmware-prepare firmware firmware-upload firmware-clean phyless-config phyless-prepare phyless-firmware phyless-upload

run:
	python3 meshcore_kiss_monitor.py

run-tcp:
	python3 meshcore_kiss_monitor.py --tcp $(RADIO_HOST)

run-broker:
	python3 meshcore_kiss_broker.py --upstream tcp://$(RADIO_HOST):8001

test: test-python firmware-check host-test

test-python:
	@mkdir -p .tmp
	TMPDIR=$(CURDIR)/.tmp python3 -m unittest -v

test-live-multiclient:
	python3 test_support/hardware/live_multiclient.py $(RADIO_HOST):8001 --duration $(LIVE_TEST_SECONDS) --clients $(LIVE_CLIENTS)

test-live-dashboard:
	node test_support/live_dashboard.mjs "$(RADIO_HOST)" "$(LIVE_TEST_SECONDS)" "$(DASHBOARD_CLIENTS)"

test-live-reception:
	python3 test_support/hardware/live_multiclient.py $(RADIO_HOST):8001 --receive-only --duration $(LIVE_TEST_SECONDS)

firmware-check: native-test-deps
	@mkdir -p .tmp
	$(MAKE) -f test_support/phy_parity/Makefile test

firmware-config:
	@test -e firmware/platformio.local.ini || cp firmware/esp32/platformio.modem.ini.example firmware/platformio.local.ini
	@echo "Edit firmware/platformio.local.ini, then run: make firmware"

$(MESHCORE_DIR)/.git $(PHYLESS_DIR)/.git $(NRF52_DIR)/.git $(ONCHIP_UPSTREAM)/.git:
	@mkdir -p .tmp
	git clone --depth 1 --branch $(MESHCORE_REF) https://github.com/meshcore-dev/MeshCore.git "$(@D)"

.PHONY: native-test-deps
native-test-deps: $(MESHCORE_DIR)/.git
	@mkdir -p "$(NATIVE_LIB_DIR)" "$(NATIVE_USB_LIB_DIR)"
	@test -f "$(NATIVE_LIB_DIR)/Crypto/AES128.cpp" -a -f "$(NATIVE_LIB_DIR)/CayenneLPP/src/CayenneLPP.cpp" || \
		pio pkg install -g --storage-dir "$(CURDIR)/$(NATIVE_LIB_DIR)" \
			-l 'rweather/Crypto@0.4.0' -l 'electroniccats/CayenneLPP@1.6.1' --no-save
	@test -f "$(NATIVE_USB_LIB_DIR)/base64/src/base64.hpp" || \
		pio pkg install -g --storage-dir "$(CURDIR)/$(NATIVE_USB_LIB_DIR)" \
			-l 'densaugeo/base64@1.4.0' --no-save

firmware-prepare: $(MESHCORE_DIR)/.git
	@test -f firmware/platformio.local.ini || { echo "Run 'make firmware-config' and set WiFi credentials first." >&2; exit 1; }
	@git -C $(MESHCORE_DIR) apply --reverse --check "$(CURDIR)/firmware/shared/radio-reconfigure.patch" >/dev/null 2>&1 || \
		git -C $(MESHCORE_DIR) apply "$(CURDIR)/firmware/shared/radio-reconfigure.patch"
	cp firmware/esp32/wifi_kiss_main.cpp $(MESHCORE_DIR)/examples/kiss_modem/main.cpp
	cp firmware/shared/WifiKissMultiplexer.h firmware/shared/WifiKissMultiplexer.cpp firmware/shared/QueuedTxProtocol.h \
		firmware/shared/RadioDashboard.h firmware/shared/RadioDashboard.cpp firmware/shared/RadioDashboardPage.h \
		firmware/shared/RadioNetwork.h firmware/shared/RadioFirmwareIdentity.h \
		firmware/shared/SntpConfig.h firmware/shared/EspSntpClock.h $(MESHCORE_DIR)/examples/kiss_modem/
	cp firmware/platformio.local.ini $(MESHCORE_DIR)/platformio.local.ini

firmware: firmware-prepare
	cd $(MESHCORE_DIR) && pio run -e $(FIRMWARE_ENV)

firmware-upload: firmware-prepare
	cd $(MESHCORE_DIR) && pio run -e $(FIRMWARE_ENV) -t upload --upload-port "$(UPLOAD_PORT)"

.PHONY: firmware-reset
firmware-reset:
	cd $(MESHCORE_DIR) && pio pkg exec --package tool-esptoolpy -- esptool.py --chip esp32s3 --port "$(UPLOAD_PORT)" --after hard_reset chip_id

firmware-clean:
	@test ! -d $(MESHCORE_DIR) || cd $(MESHCORE_DIR) && pio run -e $(FIRMWARE_ENV) -t clean

phyless-config:
	@test -e firmware/platformio.phyless.ini || cp firmware/remote-radio/esp32/platformio.ini.example firmware/platformio.phyless.ini
	@echo "Edit firmware/platformio.phyless.ini, then run: make phyless-firmware"

phyless-prepare: $(PHYLESS_DIR)/.git
	@test -f firmware/platformio.phyless.ini || { echo "Run 'make phyless-config' first." >&2; exit 1; }
	@git -C $(PHYLESS_DIR) apply --reverse --check "$(CURDIR)/firmware/shared/queued-dispatch.patch" >/dev/null 2>&1 || \
		{ git -C $(PHYLESS_DIR) archive HEAD src/Dispatcher.h src/Dispatcher.cpp src/Packet.h src/Mesh.cpp \
		    src/helpers/StatsFormatHelper.h examples/simple_repeater/MyMesh.cpp \
		    examples/simple_room_server/MyMesh.cpp examples/companion_radio/MyMesh.cpp | tar -x -C $(PHYLESS_DIR); \
		  git -C $(PHYLESS_DIR) apply "$(CURDIR)/firmware/shared/queued-dispatch.patch"; }
	mkdir -p $(PHYLESS_DIR)/src/helpers/remote
	cp firmware/shared/RemoteKissRadio.h firmware/shared/RemoteKissRadio.cpp firmware/remote-radio/esp32/Esp32TcpKissLink.h \
		firmware/shared/QueuedTxProtocol.h firmware/shared/RadioNetwork.h $(PHYLESS_DIR)/src/helpers/remote/
	mkdir -p $(PHYLESS_DIR)/variants/phyless_xiao_s3
	cp -R firmware/remote-radio/esp32/target.* $(PHYLESS_DIR)/variants/phyless_xiao_s3/
	cp firmware/platformio.phyless.ini $(PHYLESS_DIR)/platformio.local.ini

phyless-firmware: phyless-prepare
	cd $(PHYLESS_DIR) && pio run -e $(PHYLESS_ENV)

phyless-upload: phyless-prepare
	cd $(PHYLESS_DIR) && pio run -e $(PHYLESS_ENV) -t upload --upload-port "$(UPLOAD_PORT)"

.PHONY: onchip-config onchip-prepare onchip-firmware onchip-test onchip-upload onchip-buildfs onchip-provision-fs
onchip-config:
	@test -e firmware/platformio.onchip.ini || install -m 600 firmware/esp32/platformio.ini.example firmware/platformio.onchip.ini
	@echo "Configure firmware/platformio.onchip.ini or its referenced environment variables before building."

onchip-prepare: $(ONCHIP_UPSTREAM)/.git
	@test -f firmware/platformio.onchip.ini || { echo "Run 'make onchip-config' and configure the standalone radio first." >&2; exit 1; }
	$(MAKE) -C firmware/esp32 prepare UPSTREAM="$(CURDIR)/$(ONCHIP_UPSTREAM)" BUILD="$(CURDIR)/$(ONCHIP_DIR)"
	install -m 600 firmware/platformio.onchip.ini $(ONCHIP_DIR)/platformio.local.ini

onchip-firmware: onchip-prepare
	cd $(ONCHIP_DIR) && TMPDIR="$(CURDIR)/.tmp" pio run -e Xiao_S3_WIO_onchip

onchip-test: $(ONCHIP_UPSTREAM)/.git
	$(MAKE) -C firmware/esp32 test UPSTREAM="$(CURDIR)/$(ONCHIP_UPSTREAM)" BUILD="$(CURDIR)/$(ONCHIP_DIR)"
	$(MAKE) -C test_support/companion_sessions test

onchip-upload: onchip-firmware
	@test -n "$(ONCHIP_PORT)" || { echo "Set ONCHIP_PORT to the standalone radio's explicit serial device." >&2; exit 1; }
	cd $(ONCHIP_DIR) && pio run -e Xiao_S3_WIO_onchip -t upload --upload-port "$(ONCHIP_PORT)"

onchip-buildfs:
	$(MAKE) -C firmware/esp32 buildfs UPSTREAM="$(CURDIR)/$(ONCHIP_UPSTREAM)" BUILD="$(CURDIR)/$(ONCHIP_DIR)"

onchip-provision-fs:
	@test -n "$(ONCHIP_PORT)" || { echo "Set ONCHIP_PORT to the standalone radio's explicit serial device." >&2; exit 1; }
	$(MAKE) -C firmware/esp32 provision-fs UPSTREAM="$(CURDIR)/$(ONCHIP_UPSTREAM)" BUILD="$(CURDIR)/$(ONCHIP_DIR)" \
		UPLOAD_PORT="$(ONCHIP_PORT)" CONFIRM_FS_ERASE="$(CONFIRM_FS_ERASE)"

PEER_SERIAL ?=
.PHONY: peer-diagnostics
peer-diagnostics:
	@test -n "$(PEER_SERIAL)" || { echo "Set PEER_SERIAL to the queued companion's USB device." >&2; exit 1; }
	python3 firmware/remote-radio/esp32/diagnostics.py --port "$(PEER_SERIAL)"

.PHONY: nrf52-config nrf52-prepare nrf52-firmware nrf52-upload
nrf52-config:
	@test -e firmware/platformio.nrf52.ini || install -m 600 firmware/nrf52840/platformio.companion.ini.example firmware/platformio.nrf52.ini
	@echo "Configure firmware/platformio.nrf52.ini for the onboard radio before building."

nrf52-prepare: $(NRF52_DIR)/.git
	@test -f firmware/platformio.nrf52.ini || { echo "Run 'make nrf52-config' first." >&2; exit 1; }
	install -m 600 firmware/platformio.nrf52.ini $(NRF52_DIR)/platformio.local.ini

nrf52-firmware: nrf52-prepare
	cd $(NRF52_DIR) && TMPDIR="$(CURDIR)/.tmp" pio run -e "$(NRF52_ENV)"

nrf52-upload: nrf52-firmware
	@test -n "$(NRF52_PORT)" || { echo "Set NRF52_PORT to the explicit nRF52 device." >&2; exit 1; }
	cd $(NRF52_DIR) && pio run -e "$(NRF52_ENV)" -t upload --upload-port "$(NRF52_PORT)"

.PHONY: onchip-upstream
onchip-upstream: $(ONCHIP_UPSTREAM)/.git

.PHONY: aspen-config aspen-prepare aspen-firmware aspen-test aspen-upload aspen-buildfs aspen-provision-fs aspen-readiness
aspen-config: onchip-config
aspen-prepare: onchip-prepare
aspen-firmware: onchip-firmware
aspen-test: onchip-test
aspen-upload: onchip-upload
aspen-buildfs: onchip-buildfs
aspen-provision-fs: onchip-provision-fs
aspen-readiness: onchip-readiness

.PHONY: remote-radio-config remote-radio-prepare remote-radio-firmware remote-radio-upload
remote-radio-config: phyless-config
remote-radio-prepare: phyless-prepare
remote-radio-firmware: phyless-firmware
remote-radio-upload: phyless-upload

.PHONY: pine-prepare pine-firmware pine-test
pine-prepare:
	$(MAKE) -C firmware/nrf52840 prepare
pine-firmware:
	$(MAKE) -C firmware/nrf52840 build
pine-test:
	$(MAKE) -C firmware/nrf52840 test
