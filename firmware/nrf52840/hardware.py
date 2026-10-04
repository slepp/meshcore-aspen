#!/usr/bin/env python3
"""Explicit-device nRF backup and boot checks. RF-capable checks are opt-in."""

import argparse
import errno
import hashlib
import json
import os
import pathlib
import re
import struct
import time

import serial
import serial.tools.list_ports

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.inventory import value as inventory_value

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
def usb_serial():
    return inventory_value("nrf_serial")


def private_directory(value):
    directory = pathlib.Path(value).resolve()
    roots = (ROOT / ".tmp", HERE / ".build")
    if directory in roots or not any(directory.is_relative_to(root) for root in roots):
        raise ValueError("Hardware records must be inside repository .tmp or firmware/nrf52840/.build")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    return directory


def save(directory, name, data):
    path = directory / name
    if isinstance(data, dict):
        data = json.dumps(data, indent=2).encode() + b"\n"
    with path.open("xb") as output:
        os.chmod(path, 0o600)
        output.write(data)


def device(port):
    resolved = str(pathlib.Path(port).resolve())
    matches = [p for p in serial.tools.list_ports.comports() if p.device == resolved]
    if len(matches) != 1 or matches[0].vid != 0x2886 or matches[0].pid not in (0x44, 0x45, 0x8044, 0x8045):
        raise ValueError("Explicit port is not an enumerated Seeed XIAO nRF52840 USB device")
    if matches[0].serial_number != usb_serial():
        raise ValueError("Selected XIAO USB serial does not match the operator inventory; connection refused")
    return matches[0]


def connection(port, baudrate=115200):
    device(port)
    return serial.Serial(port, baudrate=baudrate, timeout=0.1, write_timeout=2, exclusive=True)


def frame(port, expected, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if port.read(1) != b">":
            continue
        header = port.read(2)
        if len(header) != 2:
            raise RuntimeError("Short companion frame header")
        length = int.from_bytes(header, "little")
        if not 0 < length <= 4096:
            raise RuntimeError("Invalid companion frame length")
        payload = port.read(length)
        if len(payload) != length:
            raise RuntimeError("Short companion frame body")
        if payload[0] in expected:
            return payload
    raise TimeoutError(f"No companion response of type {sorted(expected)}")


def request(port, payload, response):
    port.write(b"<" + len(payload).to_bytes(2, "little") + payload)
    return frame(port, {response, 1, 15})


def companion_backup(port, directory):
    info = device(port)
    manifest = {
        "port": port, "usb_serial": info.serial_number, "vid": info.vid, "pid": info.pid,
        "kind": "Read-only companion API snapshot; not a full firmware/filesystem backup",
        "responses": {},
    }
    queries = [
        ("device", bytes([22, 13]), 13),
        ("self", bytes([1]) + bytes(7) + b"nrfmast-backup", 5),
        ("private-key", bytes([23]), 14),
        ("clock", bytes([5]), 9),
        ("storage", bytes([20]), 12),
        ("tuning", bytes([43]), 23),
        ("autoadd", bytes([59]), 25),
        ("default-scope", bytes([64]), 28),
    ]
    with connection(port) as link:
        time.sleep(0.2)
        link.reset_input_buffer()
        for name, payload, response in queries:
            data = request(link, payload, response)
            save(directory, name + ".bin", data)
            manifest["responses"][name] = {"type": data[0], "bytes": len(data)}
            if name == "self" and data[0] == 5:
                if len(data) < 58:
                    raise RuntimeError("Companion self-info omitted the radio profile")
                manifest["radio"] = {
                    "frequency_mhz": int.from_bytes(data[48:52], "little") / 1000,
                    "bandwidth_khz": int.from_bytes(data[52:56], "little") / 1000,
                    "sf": data[56], "cr": data[57], "tx_power_dbm": data[2],
                }
            if name == "private-key" and data[0] == 14:
                if len(data) != 65 or request(link, payload, response) != data:
                    raise RuntimeError("Private-key export repeat read did not match")
                manifest["identity_export_repeat_verified"] = True
        start = request(link, bytes([4]), 2)
        if start[0] != 2:
            raise RuntimeError("Companion did not start contact export")
        contacts = []
        while True:
            data = frame(link, {3, 4})
            contacts.append(data.hex())
            if data[0] == 4:
                break
            if len(contacts) > 400:
                raise RuntimeError("Contact export exceeded the released target capacity")
        save(directory, "contacts.json", {"frames": contacts})
        manifest["contacts"] = len(contacts) - 1
        channels = []
        channel_count = (directory / "device.bin").read_bytes()[3]
        if channel_count > 40:
            raise RuntimeError("Unexpected channel capacity")
        for index in range(channel_count):
            channels.append(request(link, bytes([31, index]), 18).hex())
        save(directory, "channels.json", {"frames": channels})
        manifest["channels"] = channel_count
    save(directory, "companion-manifest.json", manifest)
    print(json.dumps({k: v for k, v in manifest.items() if k not in ("port", "usb_serial")}, indent=2))


def bootloader(port):
    original = device(port)
    if original.pid in (0x44, 0x45):
        print("Selected XIAO is already in its serial DFU bootloader.")
        return
    try:
        with connection(port, 1200) as link:
            link.dtr = True
            time.sleep(0.1)
            link.dtr = False
    except OSError as error:
        if error.errno not in (errno.EIO, errno.ENODEV, errno.ESHUTDOWN):
            raise
        print("USB disconnected during the 1200-baud reset; checking bootloader enumeration.")
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        for entry in serial.tools.list_ports.comports():
            if entry.vid == 0x2886 and entry.serial_number == original.serial_number and entry.pid in (0x44, 0x45):
                print(f"Verified serial DFU: {entry.device}; VID/PID {entry.vid:04x}:{entry.pid:04x}")
                return
        time.sleep(0.2)
    raise TimeoutError("Selected XIAO did not enumerate in serial DFU mode")


def require_bootloader(port):
    if device(port).pid not in (0x44, 0x45):
        raise ValueError("Enter serial DFU with make bootloader, then select its new USB by-id path")


def uf2_backup(mount, directory):
    mount = pathlib.Path(mount).resolve()
    info = (mount / "INFO_UF2.TXT").read_bytes()
    if b"XIAO" not in info.upper():
        raise ValueError("UF2 volume does not identify a XIAO board")
    current = (mount / "CURRENT.UF2").read_bytes()
    if current != (mount / "CURRENT.UF2").read_bytes():
        raise RuntimeError("UF2 backup repeat read differed")
    save(directory, "INFO_UF2.TXT", info)
    save(directory, "current.uf2", current)
    blocks = {}
    for offset in range(0, len(current), 512):
        block = current[offset:offset + 512]
        if len(block) != 512:
            raise RuntimeError("Partial UF2 block")
        magic0, magic1, flags, address, size, number, total, family = struct.unpack_from("<8I", block)
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise RuntimeError("Invalid UF2 magic")
        if flags & 1:
            continue
        if not 0 < size <= 476 or address + size > 0x100000 or address in blocks:
            raise RuntimeError("Invalid or duplicate nRF flash range")
        if flags & 0x2000 and family != 0xADA52840:
            raise RuntimeError("UF2 family is not nRF52840")
        blocks[address] = block[32:32 + size]
    start, end = min(blocks), max(a + len(b) for a, b in blocks.items())
    cursor = start
    image = bytearray()
    for address, data in sorted(blocks.items()):
        if address != cursor:
            raise RuntimeError("UF2 backup contains a gap; raw image is not contiguous")
        image.extend(data)
        cursor += len(data)
    save(directory, f"flash-base-{start:08x}.bin", image)
    manifest = {
        "repeat_read_verified": True, "uf2_bytes": len(current),
        "uf2_sha256": hashlib.sha256(current).hexdigest(),
        "flash_start": hex(start), "flash_end_exclusive": hex(end),
        "internalfs_included": start <= 0xED000 and end >= 0xF4000,
        "external_qspi_included": False,
    }
    save(directory, "uf2-manifest.json", manifest)
    print(json.dumps(manifest, indent=2))


def serial_command(link, command, duration=0.5):
    link.write(command.encode() + b"\r\n")
    deadline = time.monotonic() + duration
    data = bytearray()
    while time.monotonic() < deadline:
        data.extend(link.read(4096))
    return data.decode("utf-8", errors="replace")


def packet_stats(text):
    records, diagnostics = [], []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("{"):
            records.append(json.loads(line))
        elif line == "repeater: transmit failed (RF is disabled in nrfmast_rx)":
            diagnostics.append(line)
        else:
            raise RuntimeError("Unexpected background output during packet statistics: " + line)
    if len(records) != 1 or not isinstance(records[0], dict):
        raise RuntimeError("Expected exactly one native packet statistics record")
    return records[0], diagnostics


def snapshot(port, directory, name, rf_enabled=False):
    with connection(port) as link:
        time.sleep(0.2)
        startup = link.read(4096).decode("utf-8", errors="replace")
        result = {"startup": startup}
        for command in ("ver", "board", "get name", "get radio", "get tx", "get repeat", "get path.hash.mode",
                        "ids", "mem", "stats-packets", "stats-core"):
            result[command] = serial_command(link, command)
    save(directory, name, result)
    for command, reply in result.items():
        if command != "startup" and (not reply.strip() or "??:" in reply or "ERR" in reply):
            raise RuntimeError(f"Native serial command failed: {command}")
    match = re.search(r"mem heap_total=(\d+) heap_used=(\d+) heap_free=(\d+) sampled_low=(\d+) stack_free=(\d+) rf=(\d+) tx=(\d+)", result["mem"])
    if not match:
        raise RuntimeError("No nrfmast memory response; capture serial output before proceeding")
    memory = dict(zip(("heap_total", "heap_used", "heap_free", "sampled_low", "stack_free", "rf", "tx"), map(int, match.groups())))
    if memory["rf"] != int(rf_enabled):
        raise RuntimeError("Target RF mode does not match the requested hardware check")
    if not rf_enabled and memory["tx"] != 0:
        raise RuntimeError("TX-disabled target reports a physical transmission")
    packets, _ = packet_stats(result["stats-packets"])
    if not rf_enabled and any(packets[key] != 0 for key in ("sent", "flood_tx", "direct_tx")):
        raise RuntimeError("Physical RadioLib counters report a transmission")
    identities = re.search(r"ids repeater=([0-9a-fA-F]{64}) bot=([0-9a-fA-F]{64})", result["ids"])
    if not identities or identities[1] == identities[2]:
        raise RuntimeError("Distinct role identities were not reported")
    return result, memory, identities.groups()


def reboot(port):
    with connection(port) as link:
        link.write(b"reboot\r\n")
        link.flush()
    time.sleep(2)
    deadline = time.monotonic() + 30
    while not pathlib.Path(port).exists() and time.monotonic() < deadline:
        time.sleep(0.2)


def hardware_check(port, directory, rf_enabled=False):
    before, first_memory, first_ids = snapshot(port, directory, "before-restart.json", rf_enabled)
    reboot(port)
    after, second_memory, second_ids = snapshot(port, directory, "after-restart.json", rf_enabled)
    if first_ids != second_ids:
        raise RuntimeError("Role identity changed after restart")
    if any(before[c] != after[c] for c in ("get name", "get radio", "get tx", "get repeat", "get path.hash.mode")):
        raise RuntimeError("Native preferences differed after restart")
    summary = {
        "before": first_memory, "after": second_memory,
        "distinct_identities_persisted": True, "selected_preferences_unchanged": True,
        "native_serial_responded": all(before[c].strip() and after[c].strip() for c in ("ver", "board")),
        "physical_packets_before": packet_stats(before["stats-packets"])[0],
        "physical_packets_after": packet_stats(after["stats-packets"])[0],
        "serial_diagnostics_before": packet_stats(before["stats-packets"])[1],
        "serial_diagnostics_after": packet_stats(after["stats-packets"])[1],
    }
    save(directory, "hardware-summary.json", summary)
    print(json.dumps(summary, indent=2))


def status(port, directory, rf_enabled=False):
    result, memory, _ = snapshot(port, directory, f"status-{time.time_ns()}.json", rf_enabled)
    print(json.dumps({"commands": {key: value for key, value in result.items() if key != "startup"},
                      "memory": memory}, indent=2))


def fleet_name(port, directory):
    before, _, identities = snapshot(port, directory, "name-before.json", True)
    with connection(port) as link:
        reply = serial_command(link, "set name Pine-Relay", duration=1.5).strip()
        if reply != "OK":
            raise RuntimeError("Native Pine name change rejected: " + reply)
    after, _, final_ids = snapshot(port, directory, "name-after.json", True)
    if (identities != final_ids or after["get name"].strip() != "> Pine-Relay" or
            any(before[key] != after[key] for key in
                ("get radio", "get tx", "get repeat", "get path.hash.mode"))):
        raise RuntimeError("Pine name change did not preserve identities/PHY/path policy")
    print("PASS Pine-Relay saved through native CLI; both identities and radio/path settings unchanged")


def runtime_rf_check(port, directory, identities):
    from tools.hardware.esp32_device import DIRECTORY as LAB
    from tools.hardware.admin import NativeClient, checked, private_file
    from tools.hardware.mast_checks import LAB_PHY, dashboard
    from tools.hardware.rf import public_hex

    gateway = inventory_value("receiver_host")
    profile = dashboard(gateway)["profile"]
    if (tuple(profile[key] for key in
              ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) != LAB_PHY or
            not profile["committed"] or profile["fault"]):
        raise RuntimeError("Refusing nRF runtime RF check outside the committed lab profile")
    principal = public_hex(private_file(LAB / "companion.seed", 32))
    with connection(port) as link:
        for command in (f"time {int(time.time())}", f"setperm {principal} 3"):
            if not serial_command(link, command, duration=1.5).strip().startswith("OK"):
                raise RuntimeError("Native clock/admin ACL provisioning failed")
    client = NativeClient(gateway, 8001, LAB / "companion.seed", identities[0], "", tagged=False)
    try:
        capabilities = checked(client, "get capabilities")
        if "key-import=USB-only" not in capabilities:
            raise RuntimeError("Encrypted RF CLI did not reach runtime configuration")
        saved = checked(client, "set bot.name Pine-Bot")
        name = checked(client, "get bot.name")
        identity = checked(client, "get bot.pub.key")
        denied = client.exchange("get bot.prv.key")
        if (not saved.startswith("OK saved;") or name.strip() != "> Pine-Bot" or
                identity.lower() != f"active={identities[1]} saved={identities[1]} reboot=0".lower() or
                denied != "Error: private-key export disabled"):
            raise RuntimeError("Encrypted RF runtime name/identity/privacy readback mismatch")
        save(directory, "runtime-rf.json", {
            "gateway": gateway, "repeater_key": identities[0], "bot_key": identities[1],
            "capabilities": capabilities, "name": name, "identity": identity,
            "private_export_denied": True, "path_hash_bytes": 3})
    finally:
        client.close()
    print("PASS independent encrypted native RF administration; saved bot name and public-key "
          "readback, private export denied; gateway/shared PHY unchanged")


def runtime_check(port, directory):
    before, _, identities = snapshot(port, directory, "runtime-before.json", True)
    replies = {}
    with connection(port) as link:
        for command in ("get capabilities", "get pub.key", "get bot.pub.key",
                        "set bot.name Pine-Bot-Check", "get bot.name",
                        "set bot.name Pine-Bot"):
            if command == "set bot.name Pine-Bot-Check":
                if ("key-import=USB-only" not in replies["get capabilities"] or
                        "channel-keys=unsupported" not in replies["get capabilities"]):
                    raise RuntimeError("Runtime capabilities mismatch")
                for query, identity in zip(("get pub.key", "get bot.pub.key"), identities):
                    if replies[query].lower() != f"active={identity} saved={identity} reboot=0".lower():
                        raise RuntimeError("Pending or changed identity before runtime-name reboot")
            reply = serial_command(link, command, duration=1.5).strip()
            replies[command] = reply
            if not reply or reply.startswith(("Error", "Err", "??")):
                save(directory, "runtime-error.json", replies)
                raise RuntimeError("Runtime command failed: " + command)
    save(directory, "runtime-staged.json", replies)
    if (replies["get bot.name"] != "> Pine-Bot-Check" or
            not all(replies[cmd].startswith("OK saved;") for cmd in
                    ("set bot.name Pine-Bot-Check", "set bot.name Pine-Bot"))):
        raise RuntimeError("Runtime capabilities or immediate name readback mismatch")
    hardware_check(port, directory, True)
    after, _, final_ids = snapshot(port, directory, "runtime-after.json", True)
    with connection(port) as link:
        name = serial_command(link, "get bot.name").strip()
    save(directory, "runtime-name.json", {"name": name, "identities_unchanged": identities == final_ids})
    if (name != "> Pine-Bot" or identities != final_ids or
            any(before[key] != after[key] for key in
                ("get name", "get radio", "get tx", "get repeat", "get path.hash.mode"))):
        raise RuntimeError("Runtime bot name did not persist with unchanged native identities/PHY")
    runtime_rf_check(port, directory, identities)
    print("PASS runtime Pine-Bot rename and reboot persistence; no rebuild/rekey/PHY change; "
          "channel keys explicitly unsupported for this DM-only bot")


def fleet_profile(port, directory):
    before, _, identities = snapshot(port, directory, "fleet-before.json", True)
    result = {}
    with connection(port) as link:
        for command in ("set name Pine-Relay", "set path.hash.mode 2", "set repeat on",
                        "set tx 2", "set radio 912.525,250,7,5"):
            reply = serial_command(link, command, duration=1.5).strip()
            result[command] = reply
            if not reply.startswith("OK"):
                save(directory, "fleet-config-error.json", result)
                raise RuntimeError("Native fleet preference rejected: " + command)
        for command in ("get name", "get radio", "get path.hash.mode", "ids"):
            result[command] = serial_command(link, command).strip()
    save(directory, "fleet-staged.json", result)
    radio = tuple(map(float, result["get radio"].removeprefix("> ").split(",")))
    if (result["get path.hash.mode"] != "> 2" or result["get name"] != "> Pine-Relay" or
            result["ids"] != before["ids"].strip() or len(identities) != 2 or
            len(radio) != 4 or abs(radio[0] - 912.525) > .0001 or radio[1:] != (250, 7, 5)):
        raise RuntimeError("Fleet name/path/identity/PHY readback mismatch")
    print("Staged Pine-Relay, three-byte paths and 912.525/250/SF7/CR5/2dBm; "
          "radio applies at next boot; identities retained")


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("companion-backup", "bootloader", "require-bootloader", "uf2-backup", "hardware-check", "status", "fleet-profile", "fleet-name", "runtime-check"))
    parser.add_argument("--port")
    parser.add_argument("--directory")
    parser.add_argument("--mount")
    parser.add_argument("--rf-enabled", action="store_true",
                        help="Expect RF-capable firmware during status/hardware-check; does not enable RF")
    args = parser.parse_args()
    if args.operation == "bootloader":
        bootloader(args.port)
    elif args.operation == "require-bootloader":
        require_bootloader(args.port)
    else:
        directory = private_directory(args.directory)
        if args.operation == "companion-backup":
            companion_backup(args.port, directory)
        elif args.operation == "uf2-backup":
            uf2_backup(args.mount, directory)
        elif args.operation == "status":
            status(args.port, directory, args.rf_enabled)
        elif args.operation == "fleet-profile":
            fleet_profile(args.port, directory)
        elif args.operation == "fleet-name":
            fleet_name(args.port, directory)
        elif args.operation == "runtime-check":
            runtime_check(args.port, directory)
        else:
            hardware_check(args.port, directory, args.rf_enabled)
