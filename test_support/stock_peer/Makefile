ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SELF := $(abspath $(lastword $(MAKEFILE_LIST)))
REFERENCE := d92964352441e53b93e8667b802e04f6e072b39e
UPSTREAM := https://github.com/meshcore-dev/MeshCore.git
STOCK := $(ROOT)/.tmp/stock-MeshCore
OVERRIDES := $(dir $(SELF))platformio.local.ini
ENVIRONMENT := Stock_Xiao_S3_WIO_companion_usb
ARTIFACTS := $(STOCK)/.pio/build/$(ENVIRONMENT)
PROVENANCE := $(STOCK)/.pio/stock-peer
PIO ?= pio
JOBS ?= 4
PYTHON ?= python3
ESPTOOL ?= $(HOME)/.platformio/packages/tool-esptoolpy/esptool.py
DEVICE_MAC ?=
PIO_ENV = PLATFORMIO_BUILD_DIR="$(STOCK)/.pio/build" PLATFORMIO_LIBDEPS_DIR="$(STOCK)/.pio/libdeps" TMPDIR="$(STOCK)/.pio/tmp"
.DEFAULT_GOAL := build
.PHONY: prepare build provenance upload boot require-upload-port

prepare:
	@if test ! -d "$(STOCK)/.git"; then \
		git clone --quiet --no-checkout --depth 1 --branch companion-v1.17.1 "$(UPSTREAM)" "$(STOCK)" && \
		git -C "$(STOCK)" checkout --quiet --detach "$(REFERENCE)"; \
	fi
	@test "$$(git -C "$(STOCK)" rev-parse HEAD)" = "$(REFERENCE)" || { echo "Stock reference mismatch" >&2; exit 1; }
	@test -z "$$(git -C "$(STOCK)" status --porcelain)" || { echo "Stock source tree is modified" >&2; exit 1; }
	@cp "$(OVERRIDES)" "$(STOCK)/platformio.local.ini"
	@mkdir -p "$(STOCK)/.pio/tmp" "$(PROVENANCE)"

build: prepare
	$(PIO_ENV) $(PIO) run --project-dir "$(STOCK)" --environment "$(ENVIRONMENT)" --jobs "$(JOBS)" --target compiledb --target mergebin
	$(MAKE) --no-print-directory -f "$(SELF)" provenance

provenance: prepare
	@git -C "$(STOCK)" rev-parse HEAD > "$(PROVENANCE)/reference.txt"
	@git -C "$(STOCK)" status --porcelain > "$(PROVENANCE)/source-status.txt"
	@$(PIO) --version > "$(PROVENANCE)/platformio-version.txt"
	@$(PIO_ENV) $(PIO) project config --project-dir "$(STOCK)" --json-output > "$(PROVENANCE)/resolved-config.json"
	@$(PIO_ENV) $(PIO) pkg list --project-dir "$(STOCK)" --environment "$(ENVIRONMENT)" > "$(PROVENANCE)/packages.txt"
	@cp "$(OVERRIDES)" "$(PROVENANCE)/platformio.local.ini"
	@cp "$(STOCK)/compile_commands.json" "$(PROVENANCE)/compile_commands.json"
	@sha256sum "$(STOCK)/platformio.ini" "$(STOCK)/variants/xiao_s3_wio/platformio.ini" \
		"$(STOCK)/variants/xiao_s3_wio/target.h" "$(STOCK)/variants/xiao_s3_wio/target.cpp" \
		"$(STOCK)/variants/xiao_s3_wio/XiaoS3WIOBoard.h" "$(STOCK)/merge-bin.py" \
		"$(STOCK)/examples/companion_radio/main.cpp" "$(STOCK)/examples/companion_radio/MyMesh.cpp" \
		"$(STOCK)/src/helpers/ArduinoSerialInterface.cpp" "$(OVERRIDES)" "$(SELF)" \
		"$(dir $(SELF))inspect-build.py" > "$(PROVENANCE)/source-sha256.txt"
	@$(PYTHON) "$(dir $(SELF))inspect-build.py" "$(STOCK)" "$(ENVIRONMENT)" > "$(PROVENANCE)/boot-configuration.json"
	@sha256sum "$(ARTIFACTS)/firmware.bin" "$(ARTIFACTS)/firmware-merged.bin" \
		"$(ARTIFACTS)/firmware.elf" "$(ARTIFACTS)/bootloader.bin" "$(ARTIFACTS)/partitions.bin" \
		> "$(PROVENANCE)/binary-sha256.txt"
	@printf 'Stock environment: %s\nImages: %s\nProvenance: %s\n' "$(ENVIRONMENT)" "$(ARTIFACTS)" "$(PROVENANCE)"

require-upload-port:
	@test -n "$(UPLOAD_PORT)" || { echo "UPLOAD_PORT is required; use the stock radio's stable by-id path" >&2; exit 1; }
	@printf '%s\n' "$(DEVICE_MAC)" | grep -Eq '^[[:xdigit:]]{2}(:[[:xdigit:]]{2}){5}$$' || { echo "DEVICE_MAC is required to authorize the selected radio." >&2; exit 1; }

upload: require-upload-port build
	$(PYTHON) "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --before default_reset --after no_reset read_mac > "$(PROVENANCE)/device-check.txt"
	@cat "$(PROVENANCE)/device-check.txt"
	@grep -iq '^MAC: $(DEVICE_MAC)$$' "$(PROVENANCE)/device-check.txt" || { echo "Refusing to erase/upload: not the authorized third device" >&2; exit 1; }
	$(PYTHON) "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --before no_reset --after no_reset write_flash --erase-all 0x0 "$(ARTIFACTS)/firmware-merged.bin"
	@echo "Uploaded with all old flash state erased; device left in download mode. Run the explicit boot target after successful upload."

boot: require-upload-port
	$(PYTHON) "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --before no_reset --after no_reset read_mac > "$(PROVENANCE)/device-check.txt"
	@cat "$(PROVENANCE)/device-check.txt"
	@grep -iq '^MAC: $(DEVICE_MAC)$$' "$(PROVENANCE)/device-check.txt" || { echo "Refusing to boot: not the authorized third device" >&2; exit 1; }
	$(PYTHON) "$(ESPTOOL)" --chip esp32s3 --port "$(UPLOAD_PORT)" --before no_reset --after hard_reset read_mac
