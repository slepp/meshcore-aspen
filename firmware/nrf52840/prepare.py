#!/usr/bin/env python3
"""Create an isolated build from the pinned, unmodified upstream Git tree."""

import argparse
import pathlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import hashlib
import sys
import time

REVISION = "d92964352441e53b93e8667b802e04f6e072b39e"
HERE = pathlib.Path(__file__).resolve().parent


def stage_field_dfu(output, framework=None):
    framework = pathlib.Path(framework or pathlib.Path.home() /
                             ".platformio/packages/framework-arduinoadafruitnrf52")
    native = (framework / "libraries/Bluefruit52Lib/src/services/BLEDfu.cpp").read_text()
    if hashlib.sha256(native.encode()).hexdigest() != "244e1805b18b4e950c6aebb8e98483789c6bd90d9db240d38c32f7fd98d1cf70":
        raise ValueError("Installed Adafruit BLEDfu revision changed; review field-update integration")
    signature = "static void bledfu_control_wr_authorize_cb(uint16_t conn_hdl, BLECharacteristic* chr, ble_gatts_evt_write_t* request)\n{"
    if native.count(signature) != 1 or native.count("err_t BLEDfu::begin(void)") != 1:
        raise ValueError("Adafruit BLEDfu handover changed; review field-update integration")
    gate = """
  if (!nrfmast::authorizeFieldDfu(conn_hdl, request)) {
    ble_gatts_rw_authorize_reply_params_t denied{};
    denied.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    denied.params.write.gatt_status = BLE_GATT_STATUS_ATTERR_INSUF_AUTHORIZATION;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &denied);
    return;
  }
"""
    native = native.replace('#include "bluefruit.h"', '#include "PineDfuService.h"')
    native = native.replace("BLEDfu::", "PineDfuService::").replace(
        "PineDfuService::BLEDfu(", "PineDfuService::PineDfuService(")
    native = native.replace("UUID128_", "PINE_UUID128_")
    native = native.replace(signature, signature + gate)
    (output / "examples/nrfmast/PineDfuService.cpp").write_text(native)


def prepare(source, output, lua_archive=None):
    source, output = pathlib.Path(source).resolve(), pathlib.Path(output).resolve()
    if not output.is_relative_to(HERE / ".build"):
        raise ValueError("Build output must be inside firmware/nrf52840/.build")
    output.mkdir(parents=True, exist_ok=True)
    if not source.is_dir():
        source = HERE / ".build" / "source"
        if not source.exists():
            subprocess.run(
                ["git", "clone", "--quiet", "--depth", "1", "--branch",
                 "companion-v1.17.1", "https://github.com/meshcore-dev/MeshCore.git", str(source)],
                check=True,
            )
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", REVISION + "^{commit}"], text=True
    ).strip()
    if revision != REVISION:
        raise ValueError("MeshCore revision mismatch")
    marker = output / ".nrfmast-revision"
    if marker.exists() and marker.read_text().strip() != REVISION:
        raise ValueError("Build directory contains a different MeshCore revision")
    if not marker.exists():
        with tempfile.TemporaryFile(dir=HERE / ".build") as archive:
            subprocess.run(
                ["git", "-C", str(source), "archive", REVISION], stdout=archive, check=True
            )
            archive.seek(0)
            with tarfile.open(fileobj=archive) as tree:
                tree.extractall(output, filter="data")
        marker.write_text(REVISION + "\n")
    patched = output / ".nrfmast-queued"
    if not patched.exists():
        subprocess.run(["patch", "--quiet", "-p1", "-i", str(HERE.parent / "shared/queued-dispatch.patch")],
                       cwd=output, check=True)
        patched.write_text("queued-dispatch\n")
    dest = output / "examples" / "nrfmast"
    dest.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(HERE.parent / "shared"))
    from gps_time import stage_gps_time
    stage_gps_time(output)
    for pattern in ("*.h", "*.cpp"):
        for path in HERE.glob(pattern):
            shutil.copyfile(path, dest / path.name)
    shutil.copyfile(HERE.parent / "runtime" / "AdaptiveAdmission.h", dest / "AdaptiveAdmission.h")
    shutil.copyfile(HERE.parent / "esp32" / "FirmwareIdentity.h", dest / "FirmwareIdentity.h")
    shutil.copytree(HERE / "platform", dest / "platform", dirs_exist_ok=True)
    shared = dest / "onchip"
    shared.mkdir(exist_ok=True)
    units = ("BotVm BotWorker BotTypes BotRegistry BotStore BotTimers BotReminders BotUtilities "
             "BotSettings BotHttps BotNetworkConfig BotHttpsProbe MastSource CommandBot").split()
    for directory in ("esp32", "runtime"):
        for path in (HERE.parent / directory).glob("*.h"):
            shutil.copyfile(path, shared / path.name)
    for unit in units:
        shutil.copyfile(HERE.parent / "runtime" / (unit + ".cpp"), shared / (unit + ".cpp"))
    sys.path.insert(0, str(HERE.parent / "esp32"))
    from prepare import stage_packet_pool, bound_owner_info_replies
    stage_packet_pool(output, shared)
    for name in ("RadioDashboard.h", "QueuedTxProtocol.h", "RadioTimeProtocol.h"):
        shutil.copyfile(HERE.parent / "shared" / name, dest / name)
    (shared / "BuildClock.h").write_text(
        f"#pragma once\n#define ONCHIP_CLOCK_BUILD_EPOCH {int(time.time())}u\n"
        f'#define ONCHIP_NATIVE_REVISION "{REVISION[:12]}"\n'
    )
    chat = output / "src/helpers/BaseChatMesh.h"
    text = chat.read_text()
    hook = "  virtual bool shouldAckMessage(const char*) const { return true; }\n"
    if hook not in text:
        text = text.replace("protected:", "protected:\n" + hook, 1)
        chat.write_text(text)
    chat_source = output / "src/helpers/BaseChatMesh.cpp"
    text = chat_source.read_text()
    callback = "onMessageRecv(from, packet, timestamp, (const char *) &data[5]);  // let UI know"
    if "if (!shouldAckMessage" not in text:
        if text.count(callback) != 1:
            raise ValueError("Pinned BLE carrier ACK hook changed")
        text = text.replace(callback, callback + "\n      if (!shouldAckMessage((const char*)&data[5])) return;")
        chat_source.write_text(text)
    if lua_archive:
        archive = pathlib.Path(lua_archive).resolve()
        expected = "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
        if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            raise ValueError("Lua 5.5.1 archive SHA-256 mismatch")
        with tarfile.open(archive) as tree:
            tree.extractall(HERE / ".build", filter="data")
        sys.path.insert(0, str(HERE.parent / "esp32"))
        from prepare_bot import stage_lua
        stage_lua(HERE / ".build/lua-5.5.1", output, c_stack_limit=32)
    protocol = (output / "examples/companion_radio/MyMesh.cpp").read_text()
    definitions = re.findall(
        r"^#define (?:CMD_|RESP_CODE_|PUSH_CODE_|ERR_CODE_)[A-Z0-9_]+\s+\S+",
        protocol, re.MULTILINE,
    )
    (dest / "CompanionProtocol.h").write_text(
        "#pragma once\n// Frame numbers from pinned companion-v1.17.1.\n" +
        "\n".join(definitions) + "\n"
    )
    header = output / "examples/simple_repeater/MyMesh.h"
    signature = "  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);"
    virtual = signature.replace("  void ", "  virtual void ", 1)
    content = header.read_text()
    if content.count(signature) == 1:
        header.write_text(content.replace(signature, virtual))
    elif content.count(virtual) != 1:
        raise ValueError("Pinned native admin CLI hook changed")
    content = header.read_text()
    hook = "  virtual void onRadioProfile(float, float, uint8_t, uint8_t) {};\n"
    if hook not in content:
        header.write_text(content.replace(virtual, virtual + "\n" + hook))
    repeater_source = output / "examples/simple_repeater/MyMesh.cpp"
    content = repeater_source.read_text()
    identity_include = '#include "../nrfmast/FirmwareIdentity.h"\n'
    if identity_include not in content:
        content = identity_include + content
    content = content.replace("FIRMWARE_VERSION", "MESHCORE_SLP_PINE_VERSION")
    voltage = "telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);"
    battery_guard = "{ const auto battery = board.getBattMilliVolts(); if (battery) telemetry.addVoltage(TELEM_CHANNEL_SELF, battery / 1000.0f); }"
    if voltage in content:
        content = content.replace(voltage, battery_guard)
    elif battery_guard not in content:
        raise ValueError("Pinned Pine battery telemetry hook changed")
    content = bound_owner_info_replies(content)
    for parameters in ("_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr",
                       "pending_freq, pending_bw, pending_sf, pending_cr"):
        call = f"radio_driver.setParams({parameters});"
        notification = f"\n    onRadioProfile({parameters});"
        if call + notification not in content:
            content = content.replace(call, call + notification)
    repeater_source.write_text(content)
    ble_source = output / "src/helpers/nrf52/SerialBLEInterface.cpp"
    content = ble_source.read_text()
    connected = "  return _isDeviceConnected && Bluefruit.connected() > 0;"
    authenticated = "  return _isDeviceConnected && nrfmast::fieldBleAuthenticated(_conn_handle);"
    if connected in content:
        content = content.replace(connected, authenticated)
    elif authenticated not in content:
        raise ValueError("Pinned BLE companion security hook changed")
    dfu = "  bledfu.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);\n  bledfu.begin();"
    if dfu in content and "#if NRFMAST_BLE_DFU" not in content:
        content = content.replace(dfu, "#if NRFMAST_BLE_DFU\n" + dfu + "\n#endif")
    elif "#if NRFMAST_BLE_DFU" not in content:
        raise ValueError("Pinned BLE DFU service hook changed")
    ble_source.write_text(content)
    ble_header = output / "src/helpers/nrf52/SerialBLEInterface.h"
    content = ble_header.read_text()
    if "PineDfuService bledfu;" not in content:
        if content.count("  BLEDfu bledfu;") != 1:
            raise ValueError("Pinned BLE DFU declaration changed")
        content = content.replace("#include <bluefruit.h>",
            '#include <bluefruit.h>\n#include "../../../examples/nrfmast/PineDfuService.h"')
        content = content.replace("  BLEDfu bledfu;", "  PineDfuService bledfu;")
        ble_header.write_text(content)
    # Register first: the Nordic buttonless DFU client expects this layout.
    content = ble_source.read_text()
    block = "#if NRFMAST_BLE_DFU\n" + dfu + "\n#endif"
    if block in content:
        content = content.replace(block, "")
        content = content.replace("  bleuart.setPermission(",
                                  block + "\n\n  bleuart.setPermission(", 1)
        ble_source.write_text(content)
    stage_field_dfu(output)
    board_source = output / "src/helpers/NRF52Board.cpp"
    content = board_source.read_text()
    ota = "bool NRF52Board::startOTAUpdate(const char *id, char reply[]) {"
    guard = """
#if defined(NRFMAST_RF_ENABLED)
  strcpy(reply, "Error: Pine requires guarded start ota confirm");
  return false;
#else
"""
    if "Pine requires guarded start ota confirm" not in content:
        if content.count(ota) != 1 or content.count("  return true;\n}\n#endif") != 1:
            raise ValueError("Pinned Nordic board OTA hook changed")
        content = content.replace(ota, ota + guard)
        content = content.replace("  return true;\n}\n#endif", "  return true;\n#endif\n}\n#endif")
        board_source.write_text(content)
    shutil.copyfile(HERE / "platformio.ini", output / "platformio.local.ini")
    print(f"MeshCore {REVISION} -> {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--lua-archive")
    args = parser.parse_args()
    prepare(args.source, args.output, args.lua_archive)
