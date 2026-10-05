# Included by firmware/esp32/Makefile; all generated files stay in repo .tmp.
ONCHIP_BOT_WASM ?= 1
ifneq ($(words $(ONCHIP_BOT_WASM)),1)
$(error ONCHIP_BOT_WASM must be 0 or 1)
endif
ifneq ($(filter $(ONCHIP_BOT_WASM),0 1),$(ONCHIP_BOT_WASM))
$(error ONCHIP_BOT_WASM must be 0 or 1)
endif
export ONCHIP_BOT_WASM
BOT_BUILD := $(ROOT)/.tmp/onchip-bot-$(TEST_VARIANT)-wasm$(ONCHIP_BOT_WASM)
PLUGIN_SCHEMA ?= none
PLUGIN_ROLLBACK ?= none
BOT_LUA := $(ROOT)/.tmp/onchip-lua/lua-5.5.1
BOT_LUA_ARCHIVE := $(ROOT)/.tmp/onchip-lua/lua-5.5.1.tar.gz
BOT_LUA_SHA := 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
BOT_LUA_NATIVE := $(BOT_BUILD)/lib/OnchipLua
BOT_WAMR := $(ROOT)/.cache/meshcore-wamr
BOT_WAMR_LIB := $(BOT_WAMR)/host/libmeshcore_wamr.a
BOT_WAMR_INCLUDES := -I$(BOT_WAMR)/source/core/iwasm/include
BOT_WASM_SOURCE := ../runtime/BotWasm.cpp
ifeq ($(ONCHIP_BOT_WASM),0)
BOT_WAMR_LIB :=
BOT_WAMR_INCLUDES :=
BOT_WASM_SOURCE :=
endif
BOT_HOST_RUNNER := $(BOT_BUILD)/bot-host-runner
BOT_LUA_C := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
	lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
BOT_FLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-reorder \
	-ffunction-sections -fdata-sections -Wl,--gc-sections -pthread \
	-DMESHCORE_ONCHIP_BOT=1 -DONCHIP_BOT_WASM=$(ONCHIP_BOT_WASM) $(BOT_WAMR_INCLUDES) $(TEST_FLAGS)
.PHONY: bot-adaptive-test bot-adaptive-native-test bot-adaptive-admin-test bot-source-api-test
.PHONY: bot-repeater-test
bot-repeater-test: bot-host-runner
	"$(BOT_HOST_RUNNER)" --repeater-test
.PHONY: bot-repeater-storage-test
bot-repeater-storage-test: bot-prepare-phy
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_FLAGS) -I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(CRYPTO) \
		tests/bot_repeater_storage.cpp ../runtime/BotSettings.cpp ../runtime/BotTypes.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -o $(BOT_BUILD)/bot-repeater-storage
	"$(BOT_BUILD)/bot-repeater-storage"
bot-adaptive-test:
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(TEST_FLAGS) -I. \
		tests/adaptive_admission.cpp -o "$(BOT_BUILD)/adaptive-admission"
	"$(BOT_BUILD)/adaptive-admission"
bot-adaptive-native-test: bot-host-runner
	"$(BOT_HOST_RUNNER)" --adaptive-test
bot-source-api-test: bot-host-runner
	"$(BOT_HOST_RUNNER)" --source-api-test
bot-adaptive-admin-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 ADAPTIVE_ADMIN_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
BOT_NATIVE_FLAGS := -DKISS_LOCAL_SOURCES=5 -DKISS_MAX_TCP_CLIENTS=4 -DESP32 \
	-DMAX_CONTACTS=256 -DMAX_GROUP_CHANNELS=8 \
	-DONCHIP_CLOCK_BUILD_EPOCH=1767225600u
BOT_NATIVE_INCLUDES := -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
	-I$(BUILD)/src -I$(UPSTREAM)/examples/kiss_modem -I$(UPSTREAM)/lib/ed25519 \
	-I$(NATIVE) -I. -I$(CRYPTO) -I$(LPP) -I$(ROOT)/firmware/shared -I$(ROOT)/test_support \
	-I$(BOT_LUA_NATIVE) -I$(BASE64)
BOT_NATIVE_SOURCES := ../runtime/CommandBot.cpp ../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp \
	../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp ../runtime/BotSettings.cpp \
	RoleIdentity.cpp RoleProfile.cpp Clock.cpp
BOT_NATIVE_MESH_SOURCES := $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp \
	$(BUILD)/src/Dispatcher.cpp $(BUILD)/src/Mesh.cpp $(BUILD)/src/Packet.cpp \
	$(BUILD)/src/helpers/AdvertDataHelpers.cpp $(BUILD)/src/helpers/TransportKeyStore.cpp \
	$(BUILD)/src/helpers/BaseChatMesh.cpp $(BUILD)/src/helpers/TxtDataHelpers.cpp
BOT_HOST_WORKER_SOURCES := $(filter-out $(ROOT)/firmware/shared/WifiKissMultiplexer.cpp $(ROOT)/firmware/shared/RadioDashboard.cpp,$(BOT_NATIVE_MESH_SOURCES))
BOT_NATIVE_OBJECTS = $(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(wildcard $(PHY_BUILD)/crypto/*.o) \
	$(wildcard $(PHY_BUILD)/ed/*.o) $(BOT_LUA_OBJECTS)
BOT_REPLAY_RUNTIME_SOURCES = Runtime.cpp Management.cpp MastAdmin.cpp ../runtime/MastSource.cpp MastWeb.cpp \
	Lifecycle.cpp CompanionSessions.cpp Observer.cpp ObserverWire.cpp \
	$(NATIVE)/Repeater.cpp $(NATIVE)/Room.cpp $(NATIVE)/Companion.cpp
BOT_REPLAY_HELPERS = $(filter-out $(BOT_NATIVE_MESH_SOURCES),$(addprefix $(BUILD)/src/helpers/,$(addsuffix .cpp,$(NATIVE_HELPERS))))

.PHONY: bot-lua bot-vm-test bot-test bot-runtime-test bot-prepare bot-firmware bot-build-test
.PHONY: beta-test beta-build beta-client-test mast-cli mast-admin-ui-test mast-admin-browser-test
.PHONY: beta-lab-fixtures beta-lab-check beta-lab-backup beta-lab-build beta-lab-rebuild beta-lab-flash beta-lab-boot
.PHONY: beta-lab-rf-test
.PHONY: beta-hello-test
.PHONY: beta-vm-probe beta-vm-test beta-bare-test
.PHONY: beta-runtime-field-test
.PHONY: bot-host-runner bot-host-runner-test bot-native-harness-test bot-package-test bot-package-validate bot-release-check
.PHONY: bot-package-create bot-package-inspect bot-package-sign bot-package-verify
.PHONY: bot-plugin-install bot-plugin-update bot-plugin-fetch bot-plugin-status bot-plugin-diagnose bot-plugin-rollback bot-plugin-remove bot-plugin-reboot
.PHONY: bot-prepare-phy
bot-prepare-phy:
	$(MAKE) -C $(ROOT) native-test-deps
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
.PHONY: operator-radio-adverts operator-radio-contact operator-radio-recover-companion operator-radio-dm
.PHONY: operator-radio-isolated-adverts operator-radio-isolated-dm operator-radio-learn-contact
operator-radio-adverts operator-radio-contact operator-radio-recover-companion operator-radio-dm operator-radio-isolated-adverts operator-radio-isolated-dm operator-radio-learn-contact:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/radio_checks.py $(patsubst operator-radio-%,%,$@) $(OPERATOR_ARGS)
.PHONY: owner-field-prepare owner-field-lab-check owner-field-room-check owner-field-room-admin-check owner-field-backup owner-field-deploy owner-field-status owner-field-diagnostic
.PHONY: owner-field-monitor
.PHONY: owner-field-conversation-check
.PHONY: owner-field-fleet-gateway owner-field-fleet-peer owner-field-fleet-names owner-field-fleet-adverts owner-field-multihop-install owner-field-multihop-resume owner-field-rf-management-check
.PHONY: owner-field-aspen-names owner-field-aspen-names-persist
.PHONY: owner-field-names-test
owner-field-names-test:
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$(ROOT):.:tests python3 -m unittest -v tools.hardware.tests.test_role_names tools.hardware.tests.test_radio_checks
.PHONY: owner-field-rekey-peers owner-field-keys-test
.PHONY: owner-field-rekey-aspen owner-field-rekey-aspen-base
owner-field-rekey-aspen owner-field-rekey-aspen-base:
	@test -n "$(KEY_MANIFEST)" || { echo "Set KEY_MANIFEST to the existing private manifest." >&2; exit 1; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/identity_install.py --manifest "$(KEY_MANIFEST)" --target "$(patsubst owner-field-rekey-%,%,$@)"
owner-field-rekey-peers:
	@test -n "$(KEY_MANIFEST)" || { echo "Set KEY_MANIFEST to the parent's existing private manifest." >&2; exit 1; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/identity_install.py --manifest "$(KEY_MANIFEST)"
owner-field-keys-test:
	PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$(ROOT):.:tests python3 -m unittest -v tools.hardware.tests.test_identity_install tools.hardware.tests.test_role_names
.PHONY: owner-field-release-service owner-field-release-service-check owner-field-release-export owner-field-release-bundled owner-field-release-build owner-field-release-flash owner-field-release-verify owner-field-release-grants owner-field-tool-test
.PHONY: owner-field-release-telemetry
owner-field-release-service owner-field-release-service-check owner-field-release-export owner-field-release-bundled owner-field-release-build owner-field-release-flash owner-field-release-verify owner-field-release-grants owner-field-release-telemetry:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/esp32_update.py $(patsubst owner-field-release-%,%,$@)
owner-field-tool-test:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_mast_checks.py' -v
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_https_profile.py' -v
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_https_checks.py' -v
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_monitor.py' -v
.PHONY: owner-field-deploy-protected owner-field-stock-test
owner-field-stock-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_checks.py $(FIELD_STOCK_ARGS)
owner-field-deploy-protected:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/mast_checks.py deploy-protected
owner-field-monitor:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/monitor.py
owner-field-prepare owner-field-lab-check owner-field-room-check owner-field-room-admin-check owner-field-backup owner-field-deploy owner-field-status owner-field-diagnostic owner-field-conversation-check owner-field-fleet-gateway owner-field-fleet-peer owner-field-fleet-names owner-field-fleet-adverts owner-field-aspen-names owner-field-aspen-names-persist owner-field-multihop-install owner-field-multihop-resume owner-field-rf-management-check:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/mast_checks.py $(patsubst owner-field-%,%,$@)
beta-runtime-field-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/bot_checks.py
.PHONY: beta-multirole-test beta-stock-multirole-test
beta-multirole-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/role_checks.py
beta-stock-multirole-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_cli.py --expected-roles 7 --local-relay
beta-vm-probe:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/lua_checks.py --expect-budget-failure
beta-vm-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/lua_checks.py
beta-bare-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/lua_checks.py --bare
beta-hello-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/management_rf_checks.py --hello
.PHONY: beta-stock-check beta-stock-backup beta-stock-flash beta-stock-flash-forward beta-stock-restore beta-stock-verify
.PHONY: beta-stock-rf-test beta-stock-public-test beta-stock-status
.PHONY: beta-wifi-prepare beta-wifi-ap-up beta-wifi-ap-down beta-wifi-dhcp
.PHONY: beta-wifi-provision beta-wifi-config-get beta-wifi-host-config beta-wifi-restore-mast beta-wifi-remove-credentials beta-wifi-host beta-wifi-host-build beta-wifi-follow-test beta-wifi-editor-test
.PHONY: beta-wifi-inspect beta-wifi-recover-source beta-wifi-lan-provision
beta-wifi-prepare beta-wifi-provision beta-wifi-lan-provision beta-wifi-config-get beta-wifi-host-config beta-wifi-restore-mast beta-wifi-follow-test beta-wifi-inspect beta-wifi-recover-source:
	PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 $(ROOT)/tools/hardware/wifi_checks.py $(patsubst beta-wifi-%,%,$@)
beta-wifi-ap-up beta-wifi-ap-down beta-wifi-remove-credentials:
	sudo -n env PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 $(ROOT)/tools/hardware/wifi_checks.py $(patsubst beta-wifi-%,%,$@)
beta-wifi-dhcp:
	sudo -n dnsmasq --keep-in-foreground --conf-file=$(ROOT)/.tmp/onchip-beta-wifi/dhcp.conf
beta-wifi-host:
	$(ROOT)/.tmp/onchip-beta-wifi/meshcore-host -config $(ROOT)/.tmp/onchip-beta-wifi/host.json
beta-wifi-host-build:
	cd $(ROOT) && TMPDIR=$(ROOT)/.tmp go build -trimpath -buildvcs=false -o .tmp/onchip-beta-wifi/meshcore-host ./cmd/meshcore-host
$(ROOT)/.tmp/onchip-beta-wifi/browser-tools/node_modules/playwright-core/package.json:
	npm install --prefix $(ROOT)/.tmp/onchip-beta-wifi/browser-tools --no-save --package-lock=false --ignore-scripts --no-audit --no-fund --quiet playwright-core@1.56.1
beta-wifi-editor-test: $(ROOT)/.tmp/onchip-beta-wifi/browser-tools/node_modules/playwright-core/package.json
	TMPDIR=$(ROOT)/.tmp node wifi_editor_test.mjs
beta-stock-rf-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_cli.py
beta-stock-public-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_cli.py --public-only
beta-stock-status:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_cli.py --status
beta-stock-check beta-stock-backup beta-stock-flash beta-stock-flash-forward beta-stock-restore beta-stock-verify:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/companion_device.py $(patsubst beta-stock-%,%,$@)
beta-lab-rf-test:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/esp32_device.py rf-test
beta-lab-fixtures beta-lab-check beta-lab-backup beta-lab-build beta-lab-rebuild beta-lab-flash beta-lab-boot:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/esp32_device.py $(patsubst beta-lab-%,%,$@)
ifeq ($(BOT_INTEGRATION),1)
BOT_LIFECYCLE_FLAGS := -DMESHCORE_ONCHIP_BOT=1 -DONCHIP_BOT_RUNTIME_TEST=1 -DONCHIP_BOT_WASM=$(ONCHIP_BOT_WASM) $(BOT_WAMR_INCLUDES) -I$(BOT_LUA_NATIVE)
BOT_LIFECYCLE_SOURCES := ../runtime/CommandBot.cpp ../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
	../runtime/BotSettings.cpp
BOT_LIFECYCLE_SOURCES += tests/support/BotNativeHarness.cpp
endif
$(BOT_LUA_ARCHIVE):
	@mkdir -p $(dir $@)
	curl --fail --location --retry 2 --silent --show-error \
		https://www.lua.org/ftp/lua-5.5.1.tar.gz -o $@.partial
	echo '$(BOT_LUA_SHA)  $@.partial' | sha256sum -c -
	mv $@.partial $@
$(BOT_LUA)/src/lua.h: $(BOT_LUA_ARCHIVE)
	echo '$(BOT_LUA_SHA)  $<' | sha256sum -c -
	tar -xzf $< -C $(dir $(BOT_LUA))
$(BOT_LUA_NATIVE)/llex.c: $(BOT_LUA)/src/lua.h prepare_bot.py bot.mk
	python3 -B prepare_bot.py --lua $(BOT_LUA) --target $(BOT_BUILD)
$(BOT_BUILD)/lua/%.o: $(BOT_LUA_NATIVE)/llex.c
	@mkdir -p $(BOT_BUILD)/lua
	$(CC) -std=c11 -O2 -g -ffunction-sections -fdata-sections $(TEST_FLAGS) \
		-I$(BOT_LUA_NATIVE) -c $(BOT_LUA_NATIVE)/$*.c -o $@
BOT_LUA_OBJECTS := $(addprefix $(BOT_BUILD)/lua/,$(addsuffix .o,$(BOT_LUA_C)))
BOT_LUA_OBJECTS += $(BOT_WAMR_LIB)
ifeq ($(ONCHIP_BOT_WASM),1)
$(BOT_WAMR_LIB): prepare_wasm.py ../runtime/wasm/CMakeLists.txt
	TMPDIR=$(ROOT)/.tmp python3 -B prepare_wasm.py
endif
ifeq ($(BOT_INTEGRATION),1)
BOT_LIFECYCLE_SOURCES += $(BOT_LUA_OBJECTS)
endif
bot-lua: $(BOT_LUA_OBJECTS)
bot-vm-test: bot-lua
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_VM_TEST=1 -I. -I$(UPSTREAM)/src -I$(BOT_LUA_NATIVE) \
		tests/bot_vm.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp $(BOT_LUA_OBJECTS) \
		-lm -o $(BOT_BUILD)/bot-vm
	$(BOT_BUILD)/bot-vm
.PHONY: bot-wasm-test bot-wasm-examples bot-wasm-platform-test
bot-wasm-platform-test:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_wasm_platform.py' -v
bot-wasm-examples:
	TMPDIR=$(ROOT)/.tmp python3 -B ../runtime/wasm/build_examples.py
bot-wasm-test: prepare bot-lua bot-wasm-examples
	@test "$(ONCHIP_BOT_WASM)" = 1 || { echo "bot-wasm-test requires ONCHIP_BOT_WASM=1" >&2; exit 1; }
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_VM_TEST=1 -I. -I$(UPSTREAM)/src -I$(BOT_LUA_NATIVE) \
		tests/bot_wasm.cpp ../runtime/BotVm.cpp ../runtime/BotWasm.cpp ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp $(BOT_LUA_OBJECTS) \
		-lm -o $(BOT_BUILD)/bot-wasm
	timeout 20s $(BOT_BUILD)/bot-wasm "$(ROOT)/.tmp/wasm-examples"
.PHONY: bot-wasm-disabled-test
bot-wasm-disabled-test: prepare bot-lua bot-host-runner bot-native-worker
	@test "$(ONCHIP_BOT_WASM)" = 0 || { echo "Use ONCHIP_BOT_WASM=0 for disabled-build validation" >&2; exit 1; }
	$(CXX) $(BOT_FLAGS) -I. -I$(UPSTREAM)/src -I$(BOT_LUA_NATIVE) \
		tests/bot_wasm_disabled.cpp ../runtime/BotVm.cpp ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp $(BOT_LUA_OBJECTS) \
		-lm -o $(BOT_BUILD)/bot-wasm-disabled
	$(BOT_BUILD)/bot-wasm-disabled
	@if nm -C "$(BOT_NATIVE_WORKER)" "$(BOT_HOST_RUNNER)" "$(BOT_BUILD)/bot-wasm-disabled" | grep -E 'wasm_runtime_|meshcore_wamr_|BotWasmSession'; then \
		echo "Disabled artifact contains Wasm runtime symbols" >&2; exit 1; fi
	BOT_NATIVE_WORKER="$(BOT_NATIVE_WORKER)" PYTHONDONTWRITEBYTECODE=1 \
		python3 $(ROOT)/internal/nativebot/wasm_disabled_test.py
.PHONY: bot-wasm-integration-test
bot-wasm-integration-test: bot-host-runner bot-wasm-examples
	@test "$(ONCHIP_BOT_WASM)" = 1 || { echo "bot-wasm-integration-test requires ONCHIP_BOT_WASM=1" >&2; exit 1; }
	PYTHONDONTWRITEBYTECODE=1 BOT_HOST_RUNNER="$(BOT_HOST_RUNNER)" \
		python3 -m unittest discover -s tests -p 'test_wasm_*.py' -v
.PHONY: bot-wasm-worker-test
bot-wasm-worker-test: bot-native-worker bot-wasm-examples
	@test "$(ONCHIP_BOT_WASM)" = 1 || { echo "bot-wasm-worker-test requires ONCHIP_BOT_WASM=1" >&2; exit 1; }
	$(CXX) $(BOT_FLAGS) -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-I$(ROOT)/internal/nativebot -I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(BOT_LUA_NATIVE) -I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 \
		tests/bot_wasm_worker.cpp $(ROOT)/internal/nativebot/nvs.cpp $(ROOT)/internal/nativebot/spiffs.cpp \
		../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp \
		../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp Clock.cpp $(BOT_LUA_OBJECTS) \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -lm -o $(BOT_BUILD)/bot-wasm-worker
	$(BOT_BUILD)/bot-wasm-worker $(ROOT)/.tmp $(ROOT)/.tmp/wasm-examples
	BOT_NATIVE_WORKER="$(BOT_NATIVE_WORKER)" PYTHONDONTWRITEBYTECODE=1 \
		python3 -m unittest discover -s $(ROOT)/internal/nativebot -p 'wasm_worker_test.py' -v
.PHONY: bot-wasm-build-test
bot-wasm-build-test:
	@test "$(ONCHIP_BOT_WASM)" = 1 || { echo "bot-wasm-build-test requires ONCHIP_BOT_WASM=1; use bot-firmware for a disabled image" >&2; exit 1; }
	MESHCORE_HOSTNAME=wasm-build WIFI_SSID=build-only WIFI_PWD=not-a-secret \
		ONCHIP_ADMIN_PASSWORD=build-only ONCHIP_ROOM_PASSWORD= ONCHIP_MQTT_URI= \
		ONCHIP_MAST_PASSWORD=build-only ONCHIP_OPERATOR_PUBKEY= \
		ONCHIP_TRUSTED_COMPANION_PUBKEY= ONCHIP_SERVICE_REGION= \
		$(MAKE) bot-firmware BUILD=$(ROOT)/.tmp/onchip-wasm-firmware \
		CONFIG=$(ROOT)/firmware/esp32/platformio.ini.example ENV=Xiao_S3_WIO_onchip_beta
$(BOT_HOST_RUNNER): prepare bot-prepare-phy tests/bot_native.cpp tests/support/BotNativeHarness.cpp \
		tests/support/BotNativeHarness.h tests/support/BotReplayNetwork.cpp tests/support/BotReplayNetwork.h ../runtime/AdaptiveAdmission.h \
		$(filter-out $(NATIVE)/Repeater.cpp $(NATIVE)/Room.cpp $(NATIVE)/Companion.cpp,$(BOT_REPLAY_RUNTIME_SOURCES)) Runtime.h \
		$(BOT_NATIVE_SOURCES) $(BOT_NATIVE_MESH_SOURCES) $(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o $(BOT_LUA_OBJECTS) $(BOT_WAMR_LIB)
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_FLAGS) -DBOT_HOST_RUNNER=1 -DONCHIP_BOT_HTTPS=1 -DONCHIP_BOT_NATIVE_HTTPS=1 \
		-DMESHCORE_MAST_ADMIN=1 -DMESHCORE_ONCHIP=1 -DCOMPANION_SESSIONS_HOST -DCONFIG_LWIP_MAX_SOCKETS=16 \
		$(filter-out -DKISS_MAX_TCP_CLIENTS=4,$(BOT_NATIVE_FLAGS)) -DKISS_MAX_TCP_CLIENTS=3 \
		-DONCHIP_SERVICE_REGION='"#beta-lab"' \
		-DONCHIP_RADIO_FREQ_MHZ=912.525 -DONCHIP_RADIO_BW_KHZ=250 \
		-DONCHIP_RADIO_SF=7 -DONCHIP_RADIO_CR=5 -DONCHIP_RADIO_TX_POWER=2 \
		-DLORA_FREQ=912.525 -DLORA_BW=250 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=2 \
		$(BOT_NATIVE_INCLUDES) -I$(BOT_JSON_INCLUDE) \
		tests/bot_native.cpp tests/support/BotNativeHarness.cpp tests/support/BotReplayNetwork.cpp \
		TelemetryEndpoint.cpp TelemetryHttps.cpp $(BOT_NATIVE_SOURCES) $(BOT_NATIVE_MESH_SOURCES) \
		$(BOT_REPLAY_RUNTIME_SOURCES) $(BOT_REPLAY_HELPERS) \
		$(BOT_NATIVE_OBJECTS) $(PHY_BUILD)/CayenneLPP.o $(PHY_BUILD)/CayenneLPPPolyline.o $(BOT_JSON_LIBS) $(BOT_WAMR_LIB) -lm -o $@
bot-host-runner: prepare bot-lua $(BOT_HOST_RUNNER)
.PHONY: bot-local bot-local-test
bot-local: bot-host-runner
	cd "$(ROOT)" && PYTHONDONTWRITEBYTECODE=1 python3 firmware/runtime/bot_local.py --runner "$(BOT_HOST_RUNNER)" \
		$(if $(SOURCE),--source "$(SOURCE)",--bundled) $(if $(REPLAY),--replay "$(REPLAY)") \
		$(if $(SCENARIO),--scenario "$(SCENARIO)")
bot-local-test: bot-host-runner
	PYTHONDONTWRITEBYTECODE=1 BOT_HOST_RUNNER="$(BOT_HOST_RUNNER)" \
		python3 -m unittest discover -s tests -p 'test_bot_local.py' -v
.PHONY: bot-discovery-test bot-discovery-board-test
bot-discovery-test: bot-prepare-phy
	$(MAKE) bot-host-runner
	$(BOT_HOST_RUNNER) --native-discovery-test
bot-discovery-board-test:
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 NATIVE_DISCOVERY_ONLY=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
.PHONY: bot-native-worker bot-native-worker-test
BOT_NATIVE_WORKER := $(BOT_BUILD)/bot-native-worker
.PHONY: bot-native-worker-install
bot-native-worker-install: bot-native-worker
	install -Dm 755 "$(BOT_NATIVE_WORKER)" "$(HOME)/.local/libexec/meshcore-bot-native-worker"
BOT_HOST_NETWORK_FLAGS = -DONCHIP_BOT_HTTPS=1 -DONCHIP_BOT_NATIVE_HTTPS=1 -I. -I$(BOT_JSON_INCLUDE)
BOT_HOST_ADMIN_FLAGS = $(BOT_FLAGS) -DMESHCORE_HOST_BOT_SOURCE=1 -DMESHCORE_MAST_ADMIN=1 $(BOT_HOST_NETWORK_FLAGS)
$(BOT_BUILD)/host-worker.o: $(ROOT)/internal/nativebot/worker.cpp $(ROOT)/internal/nativebot/NativeClock.h \
		../runtime/MastSource.h ../runtime/CommandBot.h ../runtime/BotWorker.h ../runtime/BotSignal.h bot.mk
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_HOST_ADMIN_FLAGS) $(BOT_NATIVE_FLAGS) -I$(ROOT)/internal/nativebot $(BOT_NATIVE_INCLUDES) -c $< -o $@
$(BOT_BUILD)/host-mast-source.o: ../runtime/MastSource.cpp ../runtime/MastSource.h ../runtime/CommandBot.h ../runtime/BotWorker.h ../runtime/BotSignal.h bot.mk
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_HOST_ADMIN_FLAGS) $(BOT_NATIVE_FLAGS) -I$(ROOT)/internal/nativebot $(BOT_NATIVE_INCLUDES) -c $< -o $@
$(BOT_NATIVE_WORKER): $(BOT_LUA_OBJECTS) $(BOT_NATIVE_SOURCES) $(BOT_HOST_WORKER_SOURCES) \
		../runtime/CommandBot.h LocalRadio.h ../runtime/BotSignal.h ../runtime/AdaptiveAdmission.h ../runtime/BotScheduleFiles.h ../runtime/BotJournal.h ../runtime/BotTimers.h ../runtime/BotReminders.h $(BOT_BUILD)/host-worker.o $(BOT_BUILD)/host-mast-source.o \
		$(ROOT)/internal/nativebot/worker.cpp $(ROOT)/internal/nativebot/HostLocalRadio.h \
		$(ROOT)/internal/nativebot/nvs.cpp $(ROOT)/internal/nativebot/spiffs.cpp \
		$(ROOT)/internal/nativebot/HttpsTransport.cpp $(ROOT)/internal/nativebot/HttpsTransport.h \
		$(ROOT)/internal/nativebot/NativeClock.cpp $(ROOT)/internal/nativebot/NativeClock.h \
		$(PHY_BUILD)/Identity.o $(PHY_BUILD)/Utils.o
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_FLAGS) -DMESHCORE_HOST_BOT_SOURCE=1 -DMESHCORE_MAST_ADMIN=0 $(BOT_HOST_NETWORK_FLAGS) $(BOT_NATIVE_FLAGS) \
		-I$(ROOT)/internal/nativebot $(BOT_NATIVE_INCLUDES) \
		$(ROOT)/internal/nativebot/nvs.cpp $(ROOT)/internal/nativebot/spiffs.cpp \
		$(ROOT)/internal/nativebot/HttpsTransport.cpp \
		$(ROOT)/internal/nativebot/NativeClock.cpp \
		TelemetryEndpoint.cpp TelemetryHttps.cpp \
		$(BOT_BUILD)/host-worker.o $(BOT_BUILD)/host-mast-source.o $(BOT_NATIVE_SOURCES) $(BOT_HOST_WORKER_SOURCES) \
		$(BOT_NATIVE_OBJECTS) $(BOT_JSON_LIBS) -lssl -lcrypto -lm -o $@
bot-native-worker: bot-prepare bot-lua bot-prepare-phy $(BOT_NATIVE_WORKER)
.PHONY: bot-native-host-test
bot-native-host-test: bot-native-worker
	cd $(ROOT) && BOT_NATIVE_WORKER=$(BOT_NATIVE_WORKER) go test ./internal/nativebot/host ./internal/app \
		-run 'TestNativeBotUsesSharedMastAndPrivateStagedSource|TestNativeBotCoordinatedIdentityApply|TestNativeBotSharesOneMKISSSocketWithoutAnotherScheduler|TestHelloUsesVerifiedProfileAndFullMastAirtimeTable|TestRadioResultsSeparateAdmissionAndTerminalOutcome|TestPhysicalSnapshotPreservesSeparateSourceAndAggregateCounters|TestOwnerCommandsAreBoundedAndCorrelated' -count=1
bot-native-worker-test: bot-native-worker
	BOT_NATIVE_WORKER="$(BOT_NATIVE_WORKER)" PYTHONDONTWRITEBYTECODE=1 \
		python3 -m unittest discover -s $(ROOT)/internal/nativebot -p 'worker_test.py' -v
.PHONY: bot-native-clock-provider-test bot-native-clock-test
bot-native-clock-provider-test: bot-prepare
	@mkdir -p "$(BOT_BUILD)"
	$(CXX) $(BOT_FLAGS) $(BOT_NATIVE_FLAGS) -DMESHCORE_HOST_BOT_SOURCE=1 -DMESHCORE_HOST_CLOCK_TEST=1 \
		-I. $(BOT_NATIVE_INCLUDES) -I$(ROOT)/internal/nativebot \
		$(ROOT)/internal/nativebot/clock_test.cpp $(ROOT)/internal/nativebot/NativeClock.cpp Clock.cpp \
		-o $(BOT_BUILD)/native-clock-test
	$(BOT_BUILD)/native-clock-test $(ROOT)/.tmp
bot-native-clock-test: bot-native-worker bot-native-clock-provider-test
	$(MAKE) BOT_BUILD="$(BOT_BUILD)-clock-test" TEST_FLAGS="$(TEST_FLAGS) -DMESHCORE_HOST_CLOCK_TEST=1" bot-native-worker
	cd $(ROOT) && TMPDIR=$(ROOT)/.tmp BOT_NATIVE_WORKER="$(BOT_NATIVE_WORKER)" \
		BOT_NATIVE_CLOCK_TEST_WORKER="$(BOT_BUILD)-clock-test/bot-native-worker" \
		go test ./internal/app -run TestNativeBotRemindersUseTrustedHostClock -count=1
bot-host-runner-test: bot-host-runner
	PYTHONDONTWRITEBYTECODE=1 BOT_HOST_RUNNER="$(BOT_HOST_RUNNER)" \
		python3 -m unittest discover -s tests -p 'test_bot_host_runner.py' -v
bot-native-harness-test: bot-host-runner-test
	$(BOT_HOST_RUNNER)
bot-package-test:
	@mkdir -p "$(ROOT)/.tmp"
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_bot_packages.py' -v
bot-package-fetch-check: bot-https-worker-test bot-https-owner-fetch-test
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_bot_package_fetch.py' -v
.PHONY: bot-https-owner-fetch-test bot-package-fetch-check
bot-https-owner-fetch-test:
	$(MAKE) bot-https-worker-test TEST_FLAGS='$(TEST_FLAGS) -DONCHIP_BOT_OWNER_FETCH_TEST=1'
bot-package-validate: bot-host-runner
	@test -n "$(PACKAGE)" || { echo "Set PACKAGE to a Lua source package." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py package validate "$(PACKAGE)"
	$(BOT_HOST_RUNNER) --validate "$(PACKAGE)"
bot-package-create:
	@test -n "$(PLUGIN_SOURCE)" -a -n "$(PACKAGE)" -a -n "$(PLUGIN_NAME)" -a -n "$(PLUGIN_VERSION)" || \
		{ echo "Set PLUGIN_SOURCE, PACKAGE, PLUGIN_NAME and PLUGIN_VERSION." >&2; exit 2; }
	mkdir -p "$(dir $(PACKAGE))"
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py package create "$(PLUGIN_SOURCE)" "$(PACKAGE)" \
		--name "$(PLUGIN_NAME)" --version "$(PLUGIN_VERSION)" \
		--schema "$(PLUGIN_SCHEMA)" --rollback "$(PLUGIN_ROLLBACK)" \
		$(foreach capability,$(PLUGIN_CAPABILITIES),--capability "$(capability)")
bot-package-inspect:
	@test -n "$(PACKAGE)" || { echo "Set PACKAGE to a Lua source package." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py package inspect "$(PACKAGE)"
bot-package-sign:
	@test -n "$(PACKAGE)" -a -n "$(SIGNING_KEY)" || { echo "Set PACKAGE and SIGNING_KEY." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py package sign "$(PACKAGE)" --private-key-file "$(SIGNING_KEY)" $(if $(SIGNATURE),--signature "$(SIGNATURE)")
bot-package-verify:
	@test -n "$(PACKAGE)" -a -n "$(SIGNATURE)" -a -n "$(VERIFY_KEY)" || \
		{ echo "Set PACKAGE, SIGNATURE and VERIFY_KEY." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py package verify "$(PACKAGE)" \
		--signature "$(SIGNATURE)" --public-key-file "$(VERIFY_KEY)"
bot-plugin-install bot-plugin-update:
	@test -n "$(PACKAGE)" || { echo "Set PACKAGE to a Lua source package." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py $(MAST_CLI_ARGS) $(if $(filter bot-plugin-update,$@),update,package-install) \
		"$(PACKAGE)" $(if $(SIGNATURE),--signature "$(SIGNATURE)") $(if $(VERIFY_KEY),--public-key-file "$(VERIFY_KEY)")
bot-plugin-fetch:
	@test -n "$(PACKAGE_SHA256)" || \
		{ echo "Set PACKAGE_SHA256 for the configured package GET endpoint." >&2; exit 2; }
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py $(MAST_CLI_ARGS) package-fetch \
		package "$(PACKAGE_SHA256)"
bot-plugin-status bot-plugin-diagnose bot-plugin-rollback bot-plugin-remove bot-plugin-reboot:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py $(MAST_CLI_ARGS) \
		$(patsubst bot-plugin-%,%,$@)
bot-release-check: bot-package-test bot-host-runner-test bot-test bot-storage-test \
	bot-kv-files-test bot-kv-native-test bot-schedule-files-test bot-schedule-native-test bot-runtime-test beta-test bot-network-check
.PHONY: bot-network-check
bot-network-check: bot-vm-test bot-https-test bot-https-device-test bot-native-https-test \
	bot-package-fetch-check bot-native-worker-test bot-native-host-test
	cd $(ROOT) && TMPDIR=$(ROOT)/.tmp go test -race ./cmd/meshcore-bot-service
.PHONY: bot-utility-test
bot-utility-test:
	@mkdir -p $(BOT_BUILD)
	$(CXX) $(BOT_FLAGS) -I. -I$(UPSTREAM)/src tests/bot_utilities.cpp ../runtime/BotUtilities.cpp -lm -o $(BOT_BUILD)/bot-utilities
	$(BOT_BUILD)/bot-utilities
bot-test: bot-vm-test bot-utility-test bot-host-runner
	$(BOT_HOST_RUNNER)
bot-runtime-test: bot-lua bot-prepare-phy
	$(MAKE) lifecycle-test BOT_INTEGRATION=1
.PHONY: bot-kv-files-test bot-kv-native-test bot-storage-test bot-schedule-files-test bot-schedule-native-test
bot-schedule-files-test: prepare bot-prepare-phy
	@mkdir -p $(BOT_BUILD)
	$(CXX) $(BOT_FLAGS) -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(CRYPTO) tests/bot_schedule_files.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotStore.cpp ../runtime/BotTypes.cpp Clock.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -o $(BOT_BUILD)/bot-schedule-files
	$(BOT_BUILD)/bot-schedule-files
bot-schedule-native-test: prepare bot-lua bot-prepare-phy
	$(CXX) $(BOT_FLAGS) -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-I$(ROOT)/internal/nativebot -I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(BOT_LUA_NATIVE) -I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 \
		tests/bot_schedule_native.cpp $(ROOT)/internal/nativebot/nvs.cpp $(ROOT)/internal/nativebot/spiffs.cpp \
		../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp \
		../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp Clock.cpp $(BOT_LUA_OBJECTS) \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -lm -o $(BOT_BUILD)/bot-schedule-native
	$(BOT_BUILD)/bot-schedule-native $(ROOT)/.tmp
bot-kv-files-test: prepare bot-prepare-phy
	@mkdir -p $(BOT_BUILD)
	$(CXX) $(BOT_FLAGS) -I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(CRYPTO) tests/bot_kv_files.cpp ../runtime/BotStore.cpp ../runtime/BotTypes.cpp \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -o $(BOT_BUILD)/bot-kv-files
	$(BOT_BUILD)/bot-kv-files
bot-kv-native-test: prepare bot-lua bot-prepare-phy
	$(CXX) $(BOT_FLAGS) -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-I$(ROOT)/internal/nativebot -I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(BOT_LUA_NATIVE) -I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 \
		tests/bot_kv_native.cpp $(ROOT)/internal/nativebot/nvs.cpp $(ROOT)/internal/nativebot/spiffs.cpp \
		../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp \
		../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp Clock.cpp $(BOT_LUA_OBJECTS) \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -lm -o $(BOT_BUILD)/bot-kv-native
	$(BOT_BUILD)/bot-kv-native $(ROOT)/.tmp
bot-storage-test: prepare bot-lua bot-prepare-phy
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	$(CXX) $(BOT_FLAGS) -DONCHIP_CLOCK_BUILD_EPOCH=1767225600u \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(BOT_LUA_NATIVE) \
		-I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 \
		tests/bot_storage.cpp ../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp \
		../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp Clock.cpp $(BOT_LUA_OBJECTS) \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o -lm -o $(BOT_BUILD)/bot-storage
	$(BOT_BUILD)/bot-storage
beta-test: beta-client-test beta-signed-test
	$(MAKE) lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 \
		TEST_FLAGS='$(TEST_FLAGS) -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
beta-client-test:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s $(ROOT)/tools/hardware/tests -p 'test_admin.py' -v
	$(MAKE) mast-admin-ui-test
mast-admin-ui-test:
	node tests/mast_web.js MastAdminPage.h
mast-admin-browser-test: mast-admin-ui-test
	node tests/mast_admin_browser.mjs
mast-cli:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/admin.py $(ARGS)
beta-build:
	MESHCORE_HOSTNAME=meshcore-beta WIFI_SSID=build-only WIFI_PWD=not-a-secret \
		ONCHIP_ADMIN_PASSWORD=build-only ONCHIP_ROOM_PASSWORD= ONCHIP_MQTT_URI= \
		ONCHIP_MAST_PASSWORD=beta-build-only ONCHIP_SERVICE_REGION= \
		ONCHIP_TRUSTED_COMPANION_PUBKEY= ONCHIP_OPERATOR_PUBKEY= \
		$(MAKE) bot-firmware BUILD=$(ROOT)/.tmp/onchip-beta-firmware \
		CONFIG=$(ROOT)/firmware/esp32/platformio.ini.example ENV=Xiao_S3_WIO_onchip_beta
.PHONY: bot-https-build bot-https-test
.PHONY: bot-https-worker-test
.PHONY: bot-network-test
.PHONY: bot-https-tls-test
.PHONY: bot-https-device-test
bot-https-device-test: prepare
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(BOT_BUILD)
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams -I$(BUILD)/src \
		-c TelemetryEndpoint.cpp -o $(BOT_BUILD)/telemetry-device-endpoint.o
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 -DARDUINO_ARCH_ESP32 -DONCHIP_TLS_PSRAM=0 \
		-DONCHIP_CLOCK_BUILD_EPOCH=1767225600u -DONCHIP_CLOCK_TEST_BUSY=1 \
		-Itests/https_device -Itests/seams -I$(ROOT)/test_support/phy_parity/seams/http \
		-I$(ROOT)/test_support/phy_parity/seams -I. -I$(NATIVE) -I$(BUILD)/src -I$(BOT_JSON_INCLUDE) -I$(CRYPTO) \
		tests/bot_https_device.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp TelemetryHttps.cpp Clock.cpp $(BOT_BUILD)/telemetry-device-endpoint.o \
		$(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -o $(BOT_BUILD)/bot-https-device
	$(BOT_BUILD)/bot-https-device
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 -DARDUINO_ARCH_ESP32 \
		-DONCHIP_CLOCK_BUILD_EPOCH=1767225600u -DONCHIP_CLOCK_TEST_BUSY=1 \
		-Itests/https_device -Itests/seams -I$(ROOT)/test_support/phy_parity/seams/http \
		-I$(ROOT)/test_support/phy_parity/seams -I. -I$(NATIVE) -I$(BUILD)/src -I$(BOT_JSON_INCLUDE) -I$(CRYPTO) \
		tests/bot_https_psram_device.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp TelemetryHttps.cpp Clock.cpp $(BOT_BUILD)/telemetry-device-endpoint.o \
		$(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -o $(BOT_BUILD)/bot-https-psram-device
	$(BOT_BUILD)/bot-https-psram-device
.PHONY: bot-https-readiness bot-https-operator-build bot-https-probe-build
.PHONY: bot-https-peer-preflight bot-https-peer-build bot-https-peer-fixtures bot-https-peer-check-fixtures bot-https-peer-exercise bot-https-peer-verify-restore bot-https-peer-report
bot-https-peer-preflight bot-https-peer-build bot-https-peer-fixtures bot-https-peer-check-fixtures bot-https-peer-exercise bot-https-peer-verify-restore bot-https-peer-report:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tools/hardware/https_checks.py $(patsubst bot-https-peer-%,%,$@)
bot-https-probe-build:
	PYTHONDONTWRITEBYTECODE=1 python3 https_profile.py probe-build
bot-https-readiness bot-https-operator-build:
	PYTHONDONTWRITEBYTECODE=1 python3 https_profile.py $(if $(filter bot-https-readiness,$@),readiness,build)
bot-https-build:
	MESHCORE_HOSTNAME=meshcore-https-build WIFI_SSID=build-only WIFI_PWD=not-a-secret \
		ONCHIP_ADMIN_PASSWORD=build-only ONCHIP_ROOM_PASSWORD= ONCHIP_MQTT_URI= \
		ONCHIP_MAST_PASSWORD=beta-build-only ONCHIP_SERVICE_REGION= \
		ONCHIP_TRUSTED_COMPANION_PUBKEY= ONCHIP_OPERATOR_PUBKEY= \
		$(MAKE) bot-firmware BUILD=$(ROOT)/.tmp/onchip-https-firmware \
		CONFIG=$(ROOT)/firmware/esp32/platformio.ini.example ENV=Xiao_S3_WIO_onchip_https
BOT_JSON_INCLUDE ?= $(HOME)/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/include/json/cJSON
BOT_JSON_LIBS ?= -l:libcjson.so.1
bot-network-test: prepare
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(BOT_BUILD)
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_JSON_INCLUDE) -I$(CRYPTO) \
		tests/bot_network.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) \
		-o $(BOT_BUILD)/bot-network
	$(BOT_BUILD)/bot-network
bot-https-test: prepare bot-lua bot-network-test
	@test -f "$(BOT_JSON_INCLUDE)/cJSON.h" || { echo "SDK cJSON headers required; set BOT_JSON_INCLUDE explicitly." >&2; exit 1; }
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 -DONCHIP_BOT_VM_TEST=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_LUA_NATIVE) -I$(BOT_JSON_INCLUDE) -I$(CRYPTO) \
		tests/bot_https.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
		$(BOT_LUA_OBJECTS) $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -lm -o $(BOT_BUILD)/bot-https
	$(BOT_BUILD)/bot-https
bot-https-worker-test: prepare bot-lua bot-prepare-phy
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(NATIVE) -I$(BOT_LUA_NATIVE) -I$(BOT_JSON_INCLUDE) \
		-I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 \
		tests/bot_https_worker.cpp ../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp ../runtime/BotSettings.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp \
		TelemetryEndpoint.cpp TelemetryHttps.cpp \
		../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp Clock.cpp $(BOT_LUA_OBJECTS) \
		$(PHY_BUILD)/Utils.o $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -lm -o $(BOT_BUILD)/bot-https-worker
	$(BOT_BUILD)/bot-https-worker $(if $(filter 1,$(ONCHIP_BOT_WASM)),$(ROOT)/.tmp/wasm-examples)
ifeq ($(ONCHIP_BOT_WASM),1)
bot-https-worker-test: bot-wasm-examples
endif
bot-https-tls-test: prepare bot-lua
	$(PHY_MAKE) prepare $(PHY_BUILD)/combined
	@mkdir -p $(BOT_BUILD)
	cd $(ROOT) && TMPDIR=$(ROOT)/.tmp go build -trimpath -buildvcs=false -o $(BOT_BUILD)/bot-service ./cmd/meshcore-bot-service
	$(CXX) $(BOT_FLAGS) -DONCHIP_BOT_HTTPS=1 \
		-I. -Itests/seams -I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_JSON_INCLUDE) -I$(BOT_LUA_NATIVE) -I$(CRYPTO) \
		tests/bot_https_tls.cpp ../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
		$(BOT_LUA_OBJECTS) $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -lm -lssl -lcrypto -o $(BOT_BUILD)/bot-https-tls
	PYTHONDONTWRITEBYTECODE=1 python3 tests/https_loopback.py \
		--service $(BOT_BUILD)/bot-service --client $(BOT_BUILD)/bot-https-tls
.PHONY: bot-native-https-test
bot-native-https-test: prepare bot-lua bot-prepare-phy
	cd $(ROOT) && TMPDIR=$(ROOT)/.tmp go build -trimpath -buildvcs=false -o $(BOT_BUILD)/bot-service ./cmd/meshcore-bot-service
	$(CXX) $(BOT_FLAGS) -Itests/seams $(BOT_HOST_NETWORK_FLAGS) -I$(ROOT)/internal/nativebot \
		-I$(ROOT)/test_support/phy_parity/seams \
		-I$(UPSTREAM)/src -I$(BOT_LUA_NATIVE) -I$(CRYPTO) \
		tests/bot_https_tls.cpp $(ROOT)/internal/nativebot/HttpsTransport.cpp $(ROOT)/internal/nativebot/NativeClock.cpp \
		../runtime/BotHttps.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotVm.cpp $(BOT_WASM_SOURCE) ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
		$(BOT_LUA_OBJECTS) $(PHY_BUILD)/crypto/*.o $(BOT_JSON_LIBS) -lm -lssl -lcrypto -o $(BOT_BUILD)/bot-native-https
	PYTHONDONTWRITEBYTECODE=1 python3 tests/https_loopback.py \
		--service $(BOT_BUILD)/bot-service --client $(BOT_BUILD)/bot-native-https --faults
bot-prepare: prepare $(BOT_LUA)/src/lua.h
	python3 -B prepare_bot.py --lua $(BOT_LUA) --target $(BUILD)
	TMPDIR=$(ROOT)/.tmp python3 -B prepare_wasm.py --target $(BUILD) --enabled $(ONCHIP_BOT_WASM)
bot-firmware: bot-prepare
	cd $(BUILD) && TMPDIR=$(ROOT)/.tmp pio run -e $(ENV)
bot-build-test: bot-vm-test
	MESHCORE_HOSTNAME=command-bot-build WIFI_SSID=build-only WIFI_PWD=not-a-secret \
		ONCHIP_ADMIN_PASSWORD=build-only ONCHIP_ROOM_PASSWORD= \
		ONCHIP_MQTT_URI= \
		$(MAKE) bot-firmware BUILD=$(ROOT)/.tmp/onchip-bot-firmware \
		CONFIG=$(ROOT)/firmware/esp32/platformio.ini.example ENV=Xiao_S3_WIO_onchip_bot
	MESHCORE_HOSTNAME=command-bot-build WIFI_SSID=build-only WIFI_PWD=not-a-secret \
		ONCHIP_ADMIN_PASSWORD=build-only ONCHIP_ROOM_PASSWORD= \
		ONCHIP_MQTT_URI= \
		$(MAKE) firmware BUILD=$(ROOT)/.tmp/onchip-bot-firmware \
		CONFIG=$(ROOT)/firmware/esp32/platformio.ini.example ENV=Xiao_S3_WIO_onchip
