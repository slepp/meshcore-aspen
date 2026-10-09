.DEFAULT_GOAL := build
ROOT := $(abspath ../..)
BUILD := $(CURDIR)/.build
UPSTREAM := $(BUILD)/upstream
SOURCE ?= $(ROOT)/.tmp/onchip-upstream
PIO ?= pio
PYTHON ?= python3
ENV ?= nrfmast_rx
LUA_ARCHIVE ?=
PORT ?=
RECORD ?=
UF2_MOUNT ?=
CXX ?= c++
CC ?= cc
FRAMEWORK ?= $(HOME)/.platformio/packages/framework-arduinoadafruitnrf52
ARM_SIZE ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-size
ARM_NM ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm
ARM_OBJCOPY ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objcopy
NRFUTIL ?= $(HOME)/.platformio/packages/tool-adafruit-nrfutil/adafruit-nrfutil.py
UPLOAD_ENV = $(if $(filter upload-rf,$@),$(if $(filter nrfmast_rx,$(ENV)),nrfmast,$(ENV)),nrfmast_rx)

.PHONY: prepare baseline build size test native-test
prepare:
	$(PYTHON) prepare.py --source "$(SOURCE)" --output "$(UPSTREAM)" $(if $(LUA_ARCHIVE),--lua-archive "$(LUA_ARCHIVE)")
	@if test "$(ENV)" = nrfmast_fleet_lua || test "$(ENV)" = nrfmast_solar_lua; then test -f "$(UPSTREAM)/lib/OnchipLua/lua.h" || { echo "Set LUA_ARCHIVE to the verified Lua 5.5.1 archive." >&2; exit 1; }; fi

baseline: prepare
	$(PIO) run -d "$(UPSTREAM)" -e nrfmast_baseline
	$(ARM_OBJCOPY) -O binary "$(UPSTREAM)/.pio/build/nrfmast_baseline/firmware.elf" \
		"$(UPSTREAM)/.pio/build/nrfmast_baseline/firmware.bin"
	$(MAKE) size ENV=nrfmast_baseline

build: prepare
	TMPDIR="$(BUILD)" $(PIO) run -d "$(UPSTREAM)" -e "$(ENV)" -t buildprog -t create_uf2
	$(ARM_OBJCOPY) -O binary "$(UPSTREAM)/.pio/build/$(ENV)/firmware.elf" \
		"$(UPSTREAM)/.pio/build/$(ENV)/firmware.bin"
	$(MAKE) size
	@if test "$(ENV)" = nrfmast_fleet_lua || test "$(ENV)" = nrfmast_solar_lua; then $(MAKE) parser-stack-check; fi

.PHONY: parser-stack-check
parser-stack-check:
	$(PYTHON) parser_stack.py --build "$(UPSTREAM)/.pio/build/$(ENV)" \
		--lua-config "$(UPSTREAM)/lib/OnchipLua/luaconf.h"

.PHONY: lua-probe
lua-probe: prepare
	@test -f "$(LUA_ARCHIVE)" || { echo "Set LUA_ARCHIVE to the Lua 5.5.1 source archive (verified SHA-256)." >&2; exit 1; }
	$(PYTHON) lua_probe.py --archive "$(LUA_ARCHIVE)"
	$(MAKE) build ENV=nrfmast_lua_probe

size:
	$(PYTHON) memory.py --size "$(ARM_SIZE)" --nm "$(ARM_NM)" \
		"$(UPSTREAM)/.pio/build/$(ENV)/firmware.elf"

test: prepare
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -p 'test_*.py'
	$(MAKE) native-test
	$(MAKE) backend-test

.PHONY: backend-test
backend-test:
	@mkdir -p "$(NATIVE)"
	TMPDIR="$(NATIVE)" $(CC) -std=gnu11 -O1 -g $(SANITIZE) \
		-Itests/seams/flash -I"$(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs" \
		-I"$(FRAMEWORK)/libraries/InternalFileSytem/src/flash" \
		tests/note_backend.c "$(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs.c" \
		"$(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs_util.c" \
		"$(FRAMEWORK)/libraries/InternalFileSytem/src/flash/flash_cache.c" \
		-o "$(NATIVE)/note-backend"
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$(NATIVE)/note-backend"

CRYPTO := $(UPSTREAM)/.pio/libdeps/nrfmast_baseline/Crypto
LPP ?= $(UPSTREAM)/.pio/libdeps/nrfmast_fleet_lua/CayenneLPP/src
NATIVE := $(BUILD)/native
NATIVE_FLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-reorder \
	-ffunction-sections -fdata-sections -DMAX_CONTACTS=8 -DMAX_CONNECTIONS=1 \
	-Itests/seams -I. -I../esp32 -I../runtime -I../shared -I$(UPSTREAM)/examples/nrfmast -I$(UPSTREAM)/src -I$(UPSTREAM)/lib/ed25519 -I$(CRYPTO)
NATIVE_FLAGS += -I$(FRAMEWORK)/libraries/Adafruit_LittleFS/src
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CRYPTO_UNITS := AES128 AESCommon BlockCipher Crypto Hash SHA256 SHA512 Ed25519 Curve25519 BigNumberUtil
ED_UNITS := add_scalar fe ge key_exchange keypair sc sha512 sign verify
CORE_UNITS := Dispatcher Mesh Packet Identity Utils
HELPER_UNITS := AdvertDataHelpers BaseChatMesh TxtDataHelpers StaticPoolPacketManager TransportKeyStore IdentityStore
NATIVE_OBJECTS := $(addprefix $(NATIVE)/crypto/,$(addsuffix .o,$(CRYPTO_UNITS))) \
	$(addprefix $(NATIVE)/ed/,$(addsuffix .o,$(ED_UNITS))) \
	$(addprefix $(NATIVE)/core/,$(addsuffix .o,$(CORE_UNITS))) \
	$(addprefix $(NATIVE)/helpers/,$(addsuffix .o,$(HELPER_UNITS))) $(NATIVE)/note-lfs.o $(NATIVE)/note-lfs-util.o

native-test:
	@test -f "$(CRYPTO)/AES128.cpp" || $(MAKE) baseline
	$(MAKE) "$(NATIVE)/test"
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$(NATIVE)/test"

$(NATIVE)/crypto/%.o: $(CRYPTO)/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(NATIVE_FLAGS) -MMD -MP -c "$<" -o "$@"
$(NATIVE)/ed/%.o: $(UPSTREAM)/lib/ed25519/%.c
	@mkdir -p "$(@D)"
	$(CC) -O1 -g -DED25519_NO_SEED -MMD -MP -c "$<" -o "$@"
$(NATIVE)/core/%.o: $(UPSTREAM)/src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(NATIVE_FLAGS) $(SANITIZE) -MMD -MP -c "$<" -o "$@"
$(NATIVE)/helpers/%.o: $(UPSTREAM)/src/helpers/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(NATIVE_FLAGS) $(SANITIZE) -MMD -MP -c "$<" -o "$@"
$(NATIVE)/helpers/IdentityStore.o: NATIVE_FLAGS += -include Arduino.h
$(NATIVE)/note-lfs.o: $(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs.c
	@mkdir -p "$(@D)"
	$(CC) -O1 -g $(SANITIZE) -c "$<" -o "$@"
$(NATIVE)/note-lfs-util.o: $(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs_util.c
	@mkdir -p "$(@D)"
	$(CC) -O1 -g $(SANITIZE) -c "$<" -o "$@"
$(NATIVE)/test: tests/native.cpp tests/note_journal.cpp ../runtime/AdaptiveAdmission.h CommandBot.cpp CommandBot.h RuntimeConfig.cpp RuntimeConfig.h SharedRadio.cpp SharedRadio.h NoteStore.cpp NoteStore.h NoteJournal.cpp NoteJournal.h NotePartition.cpp NotePartition.h NoteDiagnostics.h BleConfig.cpp BleConfig.h CompanionInterface.cpp CompanionInterface.h $(NATIVE_OBJECTS)
	$(CXX) $(NATIVE_FLAGS) $(SANITIZE) -Wl,--gc-sections \
		tests/native.cpp tests/note_journal.cpp CommandBot.cpp RuntimeConfig.cpp SharedRadio.cpp NoteStore.cpp NoteJournal.cpp NotePartition.cpp BleConfig.cpp CompanionInterface.cpp $(NATIVE_OBJECTS) -o "$@"

-include $(wildcard $(NATIVE)/*/*.d)

.PHONY: production-lua-test production-lua-radio-test production-lua-source-parser-test
LUA_NATIVE := $(UPSTREAM)/lib/OnchipLua
LUA_UNITS := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
LUA_TEST_FLAGS := -m32 -malign-double -idirafter /usr/include/x86_64-linux-gnu -O1 -g \
	-ffunction-sections -fdata-sections -DMESHCORE_ONCHIP_BOT=1 \
	-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_JOB_LIMIT=2 -DONCHIP_BOT_WASM=0 -DONCHIP_BOT_HTTPS=0 \
	-I../esp32 -I../runtime -I../shared -I$(UPSTREAM)/src -I$(LUA_NATIVE)
LUA_TEST_OBJECTS := $(addprefix $(NATIVE)/lua/,$(addsuffix .o,$(LUA_UNITS)))
$(NATIVE)/lua/%.o: $(LUA_NATIVE)/%.c $(LUA_NATIVE)/luaconf.h $(LUA_NATIVE)/lua.h
	@mkdir -p "$(@D)"
	TMPDIR="$(NATIVE)" $(CC) -std=c11 $(LUA_TEST_FLAGS) -c "$<" -o "$@"
production-lua-test: $(LUA_TEST_OBJECTS)
	TMPDIR="$(NATIVE)" $(CXX) -std=c++17 $(LUA_TEST_FLAGS) -DONCHIP_BOT_VM_TEST=1 -DONCHIP_BOT_HEAP_MODEL=1 \
		tests/lua_vm.cpp ../runtime/BotVm.cpp ../runtime/LuaRuntime.cpp ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
		$(LUA_TEST_OBJECTS) -Wl,--gc-sections -lm -pthread -o "$(NATIVE)/lua-vm"
	"$(NATIVE)/lua-vm"
	TMPDIR="$(NATIVE)" $(CXX) -std=c++17 $(LUA_TEST_FLAGS) -DNRFMAST_PRODUCTION_LUA=1 \
		-Itests/lua-seams -Iplatform -Itests/seams -I../esp32/tests/seams -I. \
		-I$(CRYPTO) -I$(UPSTREAM)/lib/ed25519 -I$(FRAMEWORK)/libraries/Adafruit_LittleFS/src \
		tests/lua_storage.cpp PineFilesystem.cpp PineMetadata.cpp \
		../runtime/BotWorker.cpp ../runtime/BotStore.cpp ../runtime/BotTimers.cpp ../runtime/BotReminders.cpp \
		../runtime/BotVm.cpp ../runtime/LuaRuntime.cpp ../runtime/BotUtilities.cpp ../runtime/BotTypes.cpp ../runtime/BotRegistry.cpp \
		../runtime/BotSettings.cpp ../runtime/BotNetworkConfig.cpp ../runtime/BotHttps.cpp \
		$(UPSTREAM)/src/Utils.cpp $(CRYPTO)/SHA256.cpp $(CRYPTO)/Hash.cpp $(CRYPTO)/Crypto.cpp \
		$(LUA_TEST_OBJECTS) $(NATIVE)/lua-lfs.o $(NATIVE)/lua-lfs-util.o -Wl,--gc-sections,--wrap=calloc -lm -pthread -o "$(NATIVE)/lua-storage"
	"$(NATIVE)/lua-storage"

production-lua-radio-test: $(NATIVE)/crypto/SHA256.o $(NATIVE)/crypto/Hash.o $(NATIVE)/crypto/Crypto.o
	TMPDIR="$(NATIVE)" $(CXX) -std=c++17 -O1 -g $(SANITIZE) -ffunction-sections -fdata-sections -DNRFMAST_PRODUCTION_LUA=1 -DNRFMAST_RF_ENABLED=1 \
		-Itests/seams -I. -I$(UPSTREAM)/src -I$(UPSTREAM)/examples/nrfmast -I$(UPSTREAM)/examples/kiss_modem \
		-I$(LPP) -I$(CRYPTO) \
		tests/lua_radio.cpp SharedRadio.cpp $(UPSTREAM)/src/Dispatcher.cpp $(UPSTREAM)/src/helpers/StaticPoolPacketManager.cpp $(UPSTREAM)/src/Packet.cpp \
		$(NATIVE)/crypto/SHA256.o $(NATIVE)/crypto/Hash.o $(NATIVE)/crypto/Crypto.o \
		-Wl,--gc-sections -o "$(NATIVE)/lua-radio"
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$(NATIVE)/lua-radio"
production-lua-source-parser-test:
	$(MAKE) -C ../esp32 lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 ONCHIP_BOT_WASM=0 \
		UPSTREAM="$(SOURCE)" BOT_LUA="$(BUILD)/lua-5.5.1" BOT_LUA_NATIVE="$(LUA_NATIVE)" \
		$(if $(LUA_ARCHIVE),BOT_LUA_ARCHIVE="$(LUA_ARCHIVE)") \
		TEST_FLAGS='-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_JOB_LIMIT=2 -DPINE_PARSER_STACK_TEST=1 -DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
$(NATIVE)/lua-lfs.o: $(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs.c
	@mkdir -p "$(@D)"
	TMPDIR="$(NATIVE)" $(CC) -m32 -malign-double -O1 -g -c "$<" -o "$@"
$(NATIVE)/lua-lfs-util.o: $(FRAMEWORK)/libraries/Adafruit_LittleFS/src/littlefs/lfs_util.c
	@mkdir -p "$(@D)"
	TMPDIR="$(NATIVE)" $(CC) -m32 -malign-double -O1 -g -c "$<" -o "$@"
production-lua-test: production-lua-radio-test $(NATIVE)/lua-lfs.o $(NATIVE)/lua-lfs-util.o \
	$(NATIVE)/crypto/SHA256.o $(NATIVE)/crypto/Hash.o $(NATIVE)/crypto/Crypto.o

.PHONY: adaptive-arm-check
adaptive-arm-check:
	@mkdir -p "$(NATIVE)"
	$(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++ \
		-std=c++11 -Os -Wall -Wextra -Werror -mcpu=cortex-m4 -mthumb \
		-I../esp32 -I../runtime -c ../esp32/tests/adaptive_admission.cpp -o "$(NATIVE)/adaptive-admission.o"

.PHONY: companion-backup bootloader uf2-backup upload-rx upload-rf hardware-check status fleet-profile fleet-name runtime-check
companion-backup bootloader hardware-check status fleet-profile fleet-name runtime-check:
	@test -n "$(PORT)" || { echo "Set PORT to the explicitly reserved Seeed USB by-id path." >&2; exit 1; }
	@if test "$@" != bootloader; then test -n "$(RECORD)" || { echo "Set RECORD to a new private repository subdirectory." >&2; exit 1; }; fi
	$(PYTHON) hardware.py "$@" --port "$(PORT)" $(if $(RECORD),--directory "$(RECORD)") \
		$(if $(and $(filter hardware-check status,$@),$(filter nrfmast nrfmast_fleet nrfmast_fleet_lua,$(ENV))),--rf-enabled)

uf2-backup:
	@test -n "$(UF2_MOUNT)" -a -n "$(RECORD)" || { echo "Set UF2_MOUNT and a private repository RECORD directory." >&2; exit 1; }
	$(PYTHON) hardware.py uf2-backup --mount "$(UF2_MOUNT)" --directory "$(RECORD)"

upload-rx upload-rf:
	@test -n "$(PORT)" || { echo "Set PORT to the explicitly reserved Seeed USB port." >&2; exit 1; }
	$(PYTHON) hardware.py require-bootloader --port "$(PORT)"
	$(MAKE) build ENV=$(UPLOAD_ENV)
	$(PYTHON) "$(NRFUTIL)" dfu serial --package "$(UPSTREAM)/.pio/build/$(UPLOAD_ENV)/firmware.zip" \
		--port "$(PORT)" --baudrate 115200 --singlebank
