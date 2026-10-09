ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
UPSTREAM := $(ROOT)/.tmp/parity-upstream
BUILD := $(ROOT)/.tmp/phy-parity
CRYPTO := $(ROOT)/.tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi/Crypto
LPP := $(ROOT)/.tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi/CayenneLPP/src
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder -ffunction-sections -fdata-sections
CPPFLAGS := -I$(ROOT)/test_support/phy_parity/seams -I$(UPSTREAM)/examples/kiss_modem -I$(UPSTREAM)/src \
 -I$(UPSTREAM)/lib/ed25519 -I$(CRYPTO) -I$(LPP) -I$(ROOT)/firmware/shared -I$(ROOT)/firmware/esp32 -I$(ROOT)/firmware/remote-radio/esp32 -I$(ROOT)/test_support
CRYPTO_UNITS := AES128 AESCommon BlockCipher Crypto Hash SHA256 SHA512 Ed25519 Curve25519 BigNumberUtil
ED_UNITS := add_scalar fe ge key_exchange keypair sc sha512 sign verify
QUEUED_PATCH_FILES := src/Dispatcher.h src/Dispatcher.cpp src/Mesh.cpp src/Packet.h src/helpers/StatsFormatHelper.h \
 examples/simple_repeater/MyMesh.cpp examples/simple_room_server/MyMesh.cpp examples/companion_radio/MyMesh.cpp
OBJECTS := $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o $(BUILD)/RadioDashboard.o $(BUILD)/Identity.o $(BUILD)/Utils.o \
 $(BUILD)/CayenneLPP.o $(BUILD)/CayenneLPPPolyline.o \
 $(addprefix $(BUILD)/crypto/,$(addsuffix .o,$(CRYPTO_UNITS))) \
 $(addprefix $(BUILD)/ed/,$(addsuffix .o,$(ED_UNITS)))
NATIVE_CORE_OBJECTS := $(filter $(BUILD)/Identity.o $(BUILD)/Utils.o $(BUILD)/crypto/%.o $(BUILD)/ed/%.o,$(OBJECTS))
.DEFAULT_GOAL := test
USB_HELPERS := AdvertDataHelpers BaseChatMesh ConfigSerializer IdentityStore TransportKeyStore TxtDataHelpers StaticPoolPacketManager ArduinoSerialInterface
USB_BASE64 ?= $(ROOT)/.tmp/native-test-deps/base64/src
USB_LIB_OBJECTS = $(patsubst $(BUILD)/%,$(BUILD)/usb/%,\
	$(filter-out $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o,$(OBJECTS)))
USB_CPPFLAGS = -DESP32 -DENABLE_USB_INTERFACE -DQUEUED_ADAPTER_TEST \
	-DMAX_CONTACTS=64 -DMAX_GROUP_CHANNELS=8 \
	-DLORA_FREQ=910.525 -DLORA_BW=62.5 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=20 \
	$(USB_INCLUDES)
USB_INCLUDES = -I$(ROOT)/test_support/phy_parity/seams/usb \
	-I$(BUILD)/usb-seams -I$(USB_BASE64) -I$(ROOT)/.tmp/queued-core/src \
	-I$(UPSTREAM)/examples/companion_radio $(CPPFLAGS)
.PHONY: remote-usb-test
remote-usb-test: queued-prepare
	mkdir -p $(BUILD)/usb-seams $(BUILD)/usb
	cp $(addprefix $(ROOT)/firmware/esp32/tests/seams/,FS.h FSImpl.h SPIFFS.h RTClib.h) $(BUILD)/usb-seams/
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile BUILD=$(BUILD)/usb \
		CPPFLAGS='$(USB_INCLUDES)' $(USB_LIB_OBJECTS)
	$(CXX) $(CXXFLAGS) $(USB_CPPFLAGS) -DSerial=ModemSerial -c \
		$(ROOT)/firmware/shared/WifiKissMultiplexer.cpp -o $(BUILD)/usb-modem-mux.o
	$(CXX) $(CXXFLAGS) $(USB_CPPFLAGS) -DSerial=ModemSerial -c \
		$(UPSTREAM)/examples/kiss_modem/KissModem.cpp -o $(BUILD)/usb-modem-core.o
	$(CXX) $(CXXFLAGS) -Wl,--gc-sections $(USB_CPPFLAGS) \
		$(ROOT)/test_support/phy_parity/remote_usb.cpp \
		$(ROOT)/.tmp/queued-core/examples/companion_radio/MyMesh.cpp \
		$(UPSTREAM)/examples/companion_radio/DataStore.cpp \
		$(ROOT)/.tmp/queued-core/src/Dispatcher.cpp $(ROOT)/.tmp/queued-core/src/Mesh.cpp \
		$(ROOT)/.tmp/queued-core/src/Packet.cpp \
		$(addprefix $(ROOT)/.tmp/queued-core/src/helpers/,$(addsuffix .cpp,$(USB_HELPERS))) \
		$(ROOT)/firmware/shared/RemoteKissRadio.cpp $(BUILD)/usb-modem-mux.o $(BUILD)/usb-modem-core.o \
		$(USB_LIB_OBJECTS) -o $(BUILD)/remote-usb
	ulimit -c 0; $(BUILD)/remote-usb
	$(CXX) $(CXXFLAGS) -Werror -DENABLE_USB_INTERFACE $(USB_INCLUDES) \
		$(ROOT)/test_support/phy_parity/remote_diagnostics.cpp -o $(BUILD)/remote-diagnostics-usb
	$(BUILD)/remote-diagnostics-usb
	$(CXX) $(CXXFLAGS) -Werror $(USB_INCLUDES) \
		$(ROOT)/test_support/phy_parity/remote_diagnostics.cpp -o $(BUILD)/remote-diagnostics-console
	$(BUILD)/remote-diagnostics-console
.PHONY: esp32-link-test
esp32-link-test: prepare queued-prepare
	$(CXX) $(CXXFLAGS) -Werror -DARDUINO_ARCH_ESP32 -I$(ROOT)/test_support/phy_parity/seams/esp32-link -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) $(ROOT)/test_support/phy_parity/esp32_link.cpp -o $(BUILD)/esp32-link
	$(BUILD)/esp32-link
	$(CXX) $(CXXFLAGS) -Werror -DARDUINO_ARCH_ESP32 -DKISS_HOSTNAME='"field-node"' -DEXPECTED_HOSTNAME='"field-node"' -DKISS_WIFI_TX_POWER=WIFI_POWER_5dBm -DEXPECTED_POWER=WIFI_POWER_5dBm -I$(ROOT)/test_support/phy_parity/seams/esp32-link -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) $(ROOT)/test_support/phy_parity/esp32_link.cpp -o $(BUILD)/esp32-link-override
	$(BUILD)/esp32-link-override
	$(CXX) $(CXXFLAGS) -Werror -DARDUINO_ARCH_ESP32 -DWIFI_HOSTNAME='"existing-node"' -DEXPECTED_HOSTNAME='"existing-node"' -I$(ROOT)/test_support/phy_parity/seams/esp32-link -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) $(ROOT)/test_support/phy_parity/esp32_link.cpp -o $(BUILD)/esp32-link-existing-hostname
	$(BUILD)/esp32-link-existing-hostname
queued-edit-prepare: prepare
	mkdir -p $(ROOT)/.tmp/queued-core
	git -C $(UPSTREAM) archive HEAD $(QUEUED_PATCH_FILES) | tar -x -C $(ROOT)/.tmp/queued-core
queued-patch:
	@{ for file in $(QUEUED_PATCH_FILES); do diff -u --label a/$$file --label b/$$file $(UPSTREAM)/$$file $(ROOT)/.tmp/queued-core/$$file || test $$? -eq 1; done; } > $(ROOT)/firmware/shared/queued-dispatch.patch
queued-prepare: prepare
	mkdir -p $(ROOT)/.tmp/queued-core
	git -C $(UPSTREAM) archive HEAD src $(filter examples/%,$(QUEUED_PATCH_FILES)) | tar -x -C $(ROOT)/.tmp/queued-core
	cd $(ROOT)/.tmp/queued-core && patch -p1 < $(ROOT)/firmware/shared/queued-dispatch.patch
.PHONY: prepare test go-test format firmware-prepare firmware-build provenance dashboard-browser network-config stream-test stream-firmware-build wifi-recovery-test
wifi-recovery-test:
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Werror -I$(ROOT)/firmware/shared $(ROOT)/test_support/phy_parity/wifi_recovery.cpp -o $(BUILD)/wifi-recovery
	$(BUILD)/wifi-recovery
	sed -n '/^void serviceWifi() {/,/^void setup(/p' $(ROOT)/firmware/esp32/wifi_kiss_main.cpp | sed '$$d' > $(BUILD)/wifi-service.inc
	$(CXX) $(CXXFLAGS) -Werror -DMESHCORE_ONCHIP=1 -I$(ROOT)/firmware/shared -I$(BUILD) $(ROOT)/test_support/phy_parity/wifi_service.cpp -o $(BUILD)/wifi-service
	$(BUILD)/wifi-service
prepare:
	$(MAKE) -C $(ROOT)/test_support/parity prepare
	@mkdir -p $(BUILD)
$(BUILD)/KissModem.o: $(UPSTREAM)/examples/kiss_modem/KissModem.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/WifiKissMultiplexer.o: $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -MMD -MP -c $< -o $@
$(BUILD)/RadioDashboard.o: $(ROOT)/firmware/shared/RadioDashboard.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -MMD -MP -c $< -o $@
$(BUILD)/%.o: $(UPSTREAM)/src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: $(LPP)/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include cstdint -include cstddef -MMD -MP -c $< -o $@
$(BUILD)/crypto/%.o: $(CRYPTO)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/ed/%.o: $(UPSTREAM)/lib/ed25519/%.c
	@mkdir -p $(@D)
	$(CC) -O1 -g -DED25519_NO_SEED -ffunction-sections -fdata-sections -MMD -MP -c $< -o $@
$(BUILD)/combined: $(OBJECTS) $(ROOT)/test_support/phy_parity/combined.cpp $(ROOT)/test_support/phy_parity/stream_cases.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -Wl,--gc-sections $(filter-out %.h,$^) -o $@
.PHONY: mkiss-test
$(BUILD)/mkiss-session: $(ROOT)/test_support/phy_parity/mkiss_session.cpp $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp $(filter-out $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o $(BUILD)/RadioDashboard.o,$(OBJECTS))
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -Wl,--gc-sections \
		-DMESHCORE_ONCHIP=1 -DKISS_LOCAL_SOURCES=4 -DKISS_MAX_TCP_CLIENTS=4 \
		$(ROOT)/test_support/phy_parity/mkiss_session.cpp \
		$(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp \
		$(UPSTREAM)/examples/kiss_modem/KissModem.cpp \
		$(filter-out $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o $(BUILD)/RadioDashboard.o,$(OBJECTS)) \
		-o $(BUILD)/mkiss-session
mkiss-test: prepare $(BUILD)/mkiss-session
	$(BUILD)/mkiss-session
.PHONY: mkiss-keepalive-test
mkiss-keepalive-test: prepare $(BUILD)/mkiss-session
	$(BUILD)/mkiss-session --keepalive
.PHONY: mkiss-go-test
mkiss-go-test: mkiss-test
	MKISS_NATIVE_HARNESS=$(BUILD)/mkiss-session go test -race $(ROOT)/internal/radio -run '^TestSessionNativeFirmwareGoConnect$$' -count=1
$(BUILD)/legacy-multiplexer: $(BUILD)/WifiKissMultiplexer.o $(BUILD)/RadioDashboard.o $(BUILD)/Utils.o $(ROOT)/test_support/test_wifi_kiss_multiplexer.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -Wl,--gc-sections $^ -o $@
$(BUILD)/native_scores.inc: $(ROOT)/test_support/phy_parity/native_scores.py $(ROOT)/testdata/parity/native-events.jsonl
	python3 $< $(ROOT)/testdata/parity/native-events.jsonl $@
$(BUILD)/remote: $(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/RemoteKissRadio.h $(ROOT)/test_support/test_remote_kiss_radio.cpp $(BUILD)/native_scores.inc
	$(CXX) $(CXXFLAGS) -Werror -Wl,--gc-sections -DPHY_NATIVE_SCORE_VECTORS -I$(ROOT)/.tmp/queued-core/src -I$(BUILD) $(CPPFLAGS) \
		$(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/test_support/test_remote_kiss_radio.cpp -o $@
$(BUILD)/dashboard: $(BUILD)/RadioDashboard.o $(ROOT)/test_support/phy_parity/dashboard.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -pthread $^ -o $@
$(BUILD)/dashboard-contacts-bridge.inc: $(ROOT)/firmware/esp32/prepare.py
	@mkdir -p $(BUILD)
	python3 -c 'import runpy; print(runpy.run_path("$(ROOT)/firmware/esp32/prepare.py")["COMPANION_DASHBOARD_BRIDGE"])' > $@
$(BUILD)/dashboard-contacts: $(ROOT)/test_support/phy_parity/dashboard_contacts.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp $(ROOT)/firmware/shared/RadioDashboard.h $(ROOT)/firmware/esp32/DashboardContacts.h $(BUILD)/dashboard-contacts-bridge.inc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -DMESHCORE_ONCHIP=1 -DESP32=1 -I$(BUILD) \
		$(ROOT)/firmware/shared/RadioDashboard.cpp $< -o $@
$(BUILD)/dashboard-budget-general: DASHBOARD_BUDGET_FLAGS = -UMESHCORE_ONCHIP -UESP32 -UNRF52_PLATFORM
$(BUILD)/dashboard-budget-onchip: DASHBOARD_BUDGET_FLAGS = -DMESHCORE_ONCHIP=1 -UESP32 -UNRF52_PLATFORM
$(BUILD)/dashboard-budget-pine: DASHBOARD_BUDGET_FLAGS = -DMESHCORE_ONCHIP=1 -DNRF52_PLATFORM=1 -UESP32
$(BUILD)/dashboard-budget-esp32: DASHBOARD_BUDGET_FLAGS = -DMESHCORE_ONCHIP=1 -DESP32=1 -UNRF52_PLATFORM
$(BUILD)/dashboard-budget-general $(BUILD)/dashboard-budget-onchip $(BUILD)/dashboard-budget-pine $(BUILD)/dashboard-budget-esp32: $(ROOT)/test_support/phy_parity/dashboard_budget.cpp $(ROOT)/firmware/shared/RadioDashboard.h
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DASHBOARD_BUDGET_FLAGS) -Werror $< -o $@
.PHONY: dashboard-budget-test
dashboard-budget-test: $(BUILD)/dashboard-budget-general $(BUILD)/dashboard-budget-onchip $(BUILD)/dashboard-budget-pine $(BUILD)/dashboard-budget-esp32
	$(BUILD)/dashboard-budget-general
	$(BUILD)/dashboard-budget-onchip
	$(BUILD)/dashboard-budget-pine
	$(BUILD)/dashboard-budget-esp32
$(BUILD)/dashboard-sample: $(BUILD)/RadioDashboard.o $(ROOT)/test_support/phy_parity/dashboard_sample.cpp $(ROOT)/firmware/esp32/wifi_kiss_main.cpp
	sed -n '/^void publishDashboard() {/,/^void halt(/p' $(ROOT)/firmware/esp32/wifi_kiss_main.cpp | sed '$$d' > $(BUILD)/dashboard-sample.inc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Werror -I$(BUILD) $(BUILD)/RadioDashboard.o $(ROOT)/test_support/phy_parity/dashboard_sample.cpp -o $@
$(BUILD)/queued-remote-sanitize: SANITIZE_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
$(BUILD)/queued-remote $(BUILD)/queued-remote-sanitize: $(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/RemoteKissRadio.h $(ROOT)/firmware/shared/queued-dispatch.patch $(ROOT)/test_support/phy_parity/queued_remote.cpp $(NATIVE_CORE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) -Werror -Wno-error=sign-compare -Wno-error=format -Wno-error=unused-variable -Wl,--gc-sections -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) \
		$(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/.tmp/queued-core/src/Dispatcher.cpp \
		$(ROOT)/.tmp/queued-core/src/Mesh.cpp $(ROOT)/.tmp/queued-core/src/Packet.cpp $(ROOT)/.tmp/queued-core/src/helpers/StaticPoolPacketManager.cpp \
		$(ROOT)/test_support/phy_parity/queued_remote.cpp $(NATIVE_CORE_OBJECTS) -o $@
.PHONY: queued-test-build queued-rx-test queued-mesh-test queued-sanitize-test
queued-test-build: prepare queued-prepare
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile $(BUILD)/queued-remote
queued-rx-test: queued-test-build
	$(BUILD)/queued-remote rx-metadata
queued-mesh-test: queued-test-build
	$(BUILD)/queued-remote mesh-reflection
queued-sanitize-test: prepare queued-prepare
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile $(BUILD)/queued-remote-sanitize
	# Upstream packet pools allocate for process lifetime; check bounds/UB here.
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/queued-remote-sanitize rx-metadata
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/queued-remote-sanitize mesh-reflection
$(BUILD)/queued-combined: $(OBJECTS) $(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/RemoteKissRadio.h $(ROOT)/firmware/shared/queued-dispatch.patch $(ROOT)/test_support/phy_parity/combined.cpp $(ROOT)/test_support/phy_parity/stream_cases.h
	$(CXX) $(CXXFLAGS) -Wl,--gc-sections -DQUEUED_ADAPTER_TEST -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) \
		$(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp \
		$(UPSTREAM)/examples/kiss_modem/KissModem.cpp $(ROOT)/test_support/phy_parity/combined.cpp \
		$(filter-out $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o,$(OBJECTS)) -o $@
$(BUILD)/dashboard-http: $(ROOT)/test_support/phy_parity/dashboard_http.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp $(ROOT)/firmware/shared/RadioDashboard.h $(ROOT)/firmware/shared/RadioDashboardPage.h $(wildcard $(ROOT)/test_support/phy_parity/seams/http/*.h $(ROOT)/test_support/phy_parity/seams/http/freertos/*.h)
	$(CXX) -DARDUINO_ARCH_ESP32 -I$(ROOT)/test_support/phy_parity/seams/http $(CPPFLAGS) $(CXXFLAGS) -Werror -Wl,--wrap=send $< -o $@
$(BUILD)/stream-combined: STREAM_LOCALS = 0
$(BUILD)/stream-local-combined: STREAM_LOCALS = 3
$(BUILD)/stream-combined $(BUILD)/stream-local-combined: $(OBJECTS) $(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/RemoteKissRadio.h $(ROOT)/firmware/shared/WifiKissMultiplexer.h $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/queued-dispatch.patch $(ROOT)/test_support/phy_parity/combined.cpp $(ROOT)/test_support/phy_parity/stream_cases.h
	$(CXX) $(CXXFLAGS) -Werror -Wl,--gc-sections -DQUEUED_ADAPTER_TEST -DKISS_STREAM_ENDPOINT=1 -DKISS_LOCAL_SOURCES=$(STREAM_LOCALS) -I$(ROOT)/.tmp/queued-core/src $(CPPFLAGS) \
		$(ROOT)/firmware/shared/RemoteKissRadio.cpp $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp \
		$(UPSTREAM)/examples/kiss_modem/KissModem.cpp $(ROOT)/test_support/phy_parity/combined.cpp \
		$(filter-out $(BUILD)/KissModem.o $(BUILD)/WifiKissMultiplexer.o,$(OBJECTS)) -o $@
stream-test: prepare queued-prepare
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile $(BUILD)/combined $(BUILD)/stream-combined $(BUILD)/stream-local-combined
	$(BUILD)/combined
	$(BUILD)/stream-combined
	$(BUILD)/stream-local-combined
.PHONY: dashboard-test
dashboard-test: prepare
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile $(BUILD)/dashboard $(BUILD)/dashboard-http $(BUILD)/dashboard-sample $(BUILD)/dashboard-contacts
	$(BUILD)/dashboard $(BUILD)/dashboard.json
	$(BUILD)/dashboard-http
	$(BUILD)/dashboard-sample
	$(BUILD)/dashboard-contacts
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile dashboard-budget-test
	python3 $(ROOT)/test_support/phy_parity/dashboard_check.py $(BUILD)/dashboard.json $(ROOT)/firmware/shared/RadioDashboardPage.h
	node $(ROOT)/test_support/phy_parity/dashboard_lifecycle.js $(ROOT)/firmware/shared/RadioDashboardPage.h $(BUILD)/dashboard.json
	node $(ROOT)/test_support/phy_parity/dashboard_activity.js $(ROOT)/firmware/shared/RadioDashboardPage.h $(BUILD)/dashboard.json
test: prepare network-config wifi-recovery-test queued-prepare esp32-link-test mkiss-test
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile $(BUILD)/combined $(BUILD)/legacy-multiplexer $(BUILD)/remote $(BUILD)/queued-remote $(BUILD)/queued-combined $(BUILD)/stream-combined $(BUILD)/stream-local-combined
	$(BUILD)/combined
	$(BUILD)/legacy-multiplexer
	$(BUILD)/remote
	$(BUILD)/queued-remote
	$(BUILD)/queued-combined
	$(BUILD)/stream-combined
	$(BUILD)/stream-local-combined
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile dashboard-test
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile remote-usb-test
	$(MAKE) -f $(ROOT)/test_support/phy_parity/Makefile provenance
go-test:
	$(MAKE) -C $(ROOT) host-test HOST_TEST_PACKAGES=./internal/radio
dashboard-browser: test
	python3 $(ROOT)/test_support/phy_parity/dashboard_browser.py $(BUILD) $(ROOT)/firmware/shared/RadioDashboardPage.h
network-config:
	$(CXX) -std=c++11 -Wall -Wextra -Werror -fsyntax-only -I$(ROOT)/firmware/shared $(ROOT)/test_support/phy_parity/network_config.cpp
	$(CXX) -std=c++11 -Wall -Wextra -Werror -fsyntax-only -I$(ROOT)/firmware/shared -DWIFI_HOSTNAME='"existing-radio"' -DEXPECTED_HOSTNAME='"existing-radio"' $(ROOT)/test_support/phy_parity/network_config.cpp
	$(CXX) -std=c++11 -Wall -Wextra -Werror -fsyntax-only -I$(ROOT)/firmware/shared -DHOSTNAME='"existing-name"' -DEXPECTED_HOSTNAME='"existing-name"' $(ROOT)/test_support/phy_parity/network_config.cpp
	$(CXX) -std=c++11 -Wall -Wextra -Werror -fsyntax-only -I$(ROOT)/firmware/shared -DKISS_HOSTNAME='"meshcore-secondary"' -DWIFI_HOSTNAME='"existing-radio"' -DEXPECTED_HOSTNAME='"meshcore-secondary"' $(ROOT)/test_support/phy_parity/network_config.cpp
format:
	cd $(ROOT) && gofmt -w internal/radio/*.go
	clang-format -i $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/QueuedTxProtocol.h
provenance:
	@git -C $(UPSTREAM) rev-parse HEAD > $(BUILD)/reference.txt
	@cd $(ROOT) && sha256sum $(patsubst $(ROOT)/%,%,$(UPSTREAM)/examples/kiss_modem/KissModem.cpp $(UPSTREAM)/examples/kiss_modem/KissModem.h \
		$(UPSTREAM)/src/Identity.cpp $(UPSTREAM)/src/Utils.cpp $(CRYPTO)/*.cpp $(CRYPTO)/*.h \
		$(LPP)/*.cpp $(LPP)/*.h $(ROOT)/firmware/shared/WifiKissMultiplexer.* \
		$(ROOT)/firmware/shared/QueuedTxProtocol.h $(ROOT)/firmware/shared/RemoteKissRadio.* \
		$(ROOT)/firmware/esp32/wifi_kiss_main.cpp $(ROOT)/internal/radio/*.go \
		$(ROOT)/test_support/phy_parity/Makefile $(ROOT)/test_support/phy_parity/combined.cpp \
		$(ROOT)/test_support/test_wifi_kiss_multiplexer.cpp $(ROOT)/test_support/test_remote_kiss_radio.cpp \
		$(ROOT)/test_support/phy_parity/seams/*.h $(ROOT)/testdata/parity/native-events.jsonl \
		$(ROOT)/test_support/phy_parity/native_scores.py) > $(BUILD)/sources.sha256
firmware-prepare: prepare
	@mkdir -p $(ROOT)/.tmp/phy-firmware
	git -C $(UPSTREAM) archive HEAD | tar -x -C $(ROOT)/.tmp/phy-firmware
	cd $(ROOT)/.tmp/phy-firmware && patch --forward -p1 < $(ROOT)/firmware/shared/radio-reconfigure.patch
	cd $(ROOT)/.tmp/phy-firmware && patch --forward -p1 < $(ROOT)/firmware/shared/queued-dispatch.patch
	cp $(ROOT)/firmware/esp32/wifi_kiss_main.cpp $(ROOT)/.tmp/phy-firmware/examples/kiss_modem/main.cpp
	cp $(ROOT)/firmware/shared/WifiKissMultiplexer.* $(ROOT)/firmware/shared/PacketPipeline.h $(ROOT)/firmware/shared/QueuedTxProtocol.h $(ROOT)/.tmp/phy-firmware/examples/kiss_modem/
	cp $(ROOT)/firmware/shared/RadioDashboard.h $(ROOT)/firmware/shared/RadioDashboard.cpp $(ROOT)/firmware/shared/RadioDashboardPage.h $(ROOT)/firmware/shared/RadioFirmwareIdentity.h $(ROOT)/.tmp/phy-firmware/examples/kiss_modem/
	cp $(ROOT)/firmware/shared/RadioNetwork.h $(ROOT)/.tmp/phy-firmware/examples/kiss_modem/
	cp $(ROOT)/firmware/esp32/FirmwareIdentity.h $(ROOT)/.tmp/phy-firmware/examples/kiss_modem/
	mkdir -p $(ROOT)/.tmp/phy-firmware/src/helpers/remote
	cp $(ROOT)/firmware/shared/RemoteKissRadio.* $(ROOT)/firmware/remote-radio/esp32/Esp32TcpKissLink.h $(ROOT)/.tmp/phy-firmware/src/helpers/remote/
	cp $(ROOT)/firmware/shared/QueuedTxProtocol.h $(ROOT)/.tmp/phy-firmware/src/helpers/remote/
	cp $(ROOT)/firmware/shared/RadioNetwork.h $(ROOT)/.tmp/phy-firmware/src/helpers/remote/
	mkdir -p $(ROOT)/.tmp/phy-firmware/variants/phyless_xiao_s3
	cp $(ROOT)/firmware/remote-radio/esp32/target.* $(ROOT)/.tmp/phy-firmware/variants/phyless_xiao_s3/
	cat $(ROOT)/firmware/esp32/platformio.modem.ini.example $(ROOT)/firmware/remote-radio/esp32/platformio.ini.example > $(ROOT)/.tmp/phy-firmware/platformio.local.ini
firmware-build: firmware-prepare
	cd $(ROOT)/.tmp/phy-firmware && TMPDIR=$(ROOT)/.tmp PLATFORMIO_LIBDEPS_DIR=$(ROOT)/.tmp/MeshCore/.pio/libdeps \
		pio run -e Xiao_S3_WIO_kiss_wifi -e Phyless_Xiao_S3_repeater
stream-firmware-build: firmware-prepare
	cat $(ROOT)/test_support/phy_parity/stream-targets.ini >> $(ROOT)/.tmp/phy-firmware/platformio.local.ini
	cd $(ROOT)/.tmp/phy-firmware && TMPDIR=$(ROOT)/.tmp PLATFORMIO_LIBDEPS_DIR=$(ROOT)/.tmp/MeshCore/.pio/libdeps \
		pio run -e Xiao_S3_WIO_kiss_wifi -e Xiao_S3_WIO_stream_compile
queued-firmware-build: firmware-prepare
	cd $(ROOT)/.tmp/phy-firmware && TMPDIR=$(ROOT)/.tmp PLATFORMIO_LIBDEPS_DIR=$(ROOT)/.tmp/MeshCore/.pio/libdeps \
		pio run -e Phyless_Xiao_S3_repeater -e Phyless_Xiao_S3_room -e Phyless_Xiao_S3_client
-include $(OBJECTS:.o=.d)
