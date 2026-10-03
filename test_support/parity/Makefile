ROOT := $(abspath ../..)
export TMPDIR := $(ROOT)/.tmp
REFERENCE := d92964352441e53b93e8667b802e04f6e072b39e
UPSTREAM ?= $(ROOT)/.tmp/parity-upstream
CRYPTO ?= $(ROOT)/.tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi/Crypto
LPP ?= $(abspath $(CRYPTO)/../CayenneLPP/src)
BUILD ?= $(ROOT)/.tmp/parity-oracle
ABI_FLAGS ?=
ABI32_FLAGS ?= -m32 -msse2 -mfpmath=sse -idirafter /usr/include/$(shell $(CXX) -dumpmachine)
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections $(ABI_FLAGS)
CPPFLAGS := -Iseams -I$(BUILD) -I$(UPSTREAM)/src -I$(UPSTREAM)/lib/ed25519 -I$(CRYPTO) -I$(LPP) -include seams/FS.h
CORE := Packet Dispatcher Mesh Utils Identity helpers/StaticPoolPacketManager helpers/ClientACL helpers/RegionMap helpers/TransportKeyStore helpers/TxtDataHelpers helpers/ConfigSerializer helpers/IdentityStore helpers/BaseChatMesh helpers/AdvertDataHelpers
CRYPTO_UNITS := AES128 AESCommon BlockCipher Crypto Hash SHA256 SHA512 Ed25519 Curve25519 BigNumberUtil
ED_UNITS := add_scalar fe ge key_exchange keypair sc sha512 sign verify
LPP_OBJECTS := $(BUILD)/lpp/CayenneLPP.o $(BUILD)/lpp/CayenneLPPPolyline.o
ifneq ($(findstring -m32,$(ABI_FLAGS)),)
CPPFLAGS += -DPARITY_NO_CAYENNE
LPP_OBJECTS :=
endif
OBJECTS := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(CORE))) \
 $(addprefix $(BUILD)/crypto/,$(addsuffix .o,$(CRYPTO_UNITS))) \
 $(addprefix $(BUILD)/ed/,$(addsuffix .o,$(ED_UNITS))) \
 $(LPP_OBJECTS) $(BUILD)/oracle.o
.DEFAULT_GOAL := test
.PHONY: prepare test provenance snapshot policy-differential abi32 spike clean FORCE
$(BUILD)/build-config: FORCE
	@mkdir -p "$(@D)"
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' 'CPPFLAGS=$(CPPFLAGS)' 'CXXFLAGS=$(CXXFLAGS)' 'ABI_FLAGS=$(ABI_FLAGS)' > "$@.pending"
	@cmp -s "$@.pending" "$@" && rm -f "$@.pending" || mv "$@.pending" "$@"
prepare:
	@test -d "$(UPSTREAM)" || git -C "$(ROOT)/.tmp/MeshCore" worktree add --detach "$(UPSTREAM)" $(REFERENCE)
	@test "$$(git -C "$(UPSTREAM)" rev-parse HEAD)" = "$(REFERENCE)"
	@test -z "$$(git -C "$(UPSTREAM)" status --porcelain)" || { echo "Reference worktree is modified" >&2; exit 1; }
	@test -f "$(CRYPTO)/AES128.cpp" || { echo "Missing real rweather/Crypto 0.4.0: set CRYPTO to its source directory" >&2; exit 1; }
$(BUILD)/core/%.o: $(UPSTREAM)/src/%.cpp
	@mkdir -p $(@D)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/crypto/%.o: $(CRYPTO)/%.cpp
	@mkdir -p $(@D)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/lpp/%.o: $(LPP)/%.cpp
	@mkdir -p $(@D)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include cstddef -MMD -MP -c $< -o $@
$(BUILD)/ed/%.o: $(UPSTREAM)/lib/ed25519/%.c
	@mkdir -p $(@D)
	@$(CC) -O1 -g $(ABI_FLAGS) -DED25519_NO_SEED -ffunction-sections -fdata-sections -MMD -MP -c $< -o $@
$(BUILD)/kernels.inc: kernels.py $(UPSTREAM)/examples/simple_repeater/MyMesh.cpp $(UPSTREAM)/examples/simple_room_server/MyMesh.cpp $(UPSTREAM)/examples/companion_radio/MyMesh.cpp $(UPSTREAM)/src/helpers/radiolib/RadioLibWrappers.cpp $(UPSTREAM)/src/helpers/CommonCLI.cpp
	python3 kernels.py --upstream "$(UPSTREAM)" --build "$(BUILD)"
$(BUILD)/oracle.o: oracle.cpp seams/Stream.h $(BUILD)/kernels.inc
	@mkdir -p $(@D)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(OBJECTS): $(BUILD)/build-config
$(BUILD)/oracle: $(OBJECTS)
	@$(CXX) $(ABI_FLAGS) -Wl,--gc-sections $^ -o $@
test: prepare
	@mkdir -p "$(BUILD)"
	@rm -f "$(BUILD)/results.json"
	$(MAKE) $(BUILD)/oracle
	$(BUILD)/oracle > $(BUILD)/events.pending.jsonl
	mv "$(BUILD)/events.pending.jsonl" "$(BUILD)/events.jsonl"
	python3 evidence.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --lpp "$(LPP)" --build "$(BUILD)"
provenance:
	python3 evidence.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --lpp "$(LPP)" --build "$(BUILD)"
snapshot: test
	python3 evidence.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --lpp "$(LPP)" --build "$(BUILD)" --snapshot
policy-differential: test
	python3 record_policy.py --events "$(BUILD)/events.jsonl"
	python3 evidence.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --lpp "$(LPP)" --build "$(BUILD)" --snapshot
abi32:
	$(MAKE) test ABI_FLAGS="$(ABI32_FLAGS)" BUILD="$(ROOT)/.tmp/parity-oracle-32-ieee"
	python3 evidence.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --lpp "$(LPP)" --build "$(ROOT)/.tmp/parity-oracle-32-ieee" --snapshot-abi32 --abi-flags="$(ABI32_FLAGS)"
spike: prepare
	python3 spike.py --upstream "$(UPSTREAM)" --crypto "$(CRYPTO)" --build "$(BUILD)"
clean:
	rm -rf "$(BUILD)"
-include $(OBJECTS:.o=.d)
