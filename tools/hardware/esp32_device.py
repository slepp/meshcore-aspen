#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Guarded, Make-invoked operations for the isolated XIAO beta mast only."""
import argparse
import hashlib
import json
import os
import secrets
import subprocess
import time
import urllib.request
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.admin import private_file
from tools.hardware.rf import public_hex
from tools.hardware.esp32_slot import live_app_slot

from tools.hardware.inventory import value as inventory_value

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / ".tmp/onchip-beta-lab"
BUILD = ROOT / ".tmp/onchip-beta-lab-firmware"
def device_port():
    return Path(inventory_value("mast_port"))
def device_mac():
    return inventory_value("mast_mac")
ESPTOOL = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
ENV = "Xiao_S3_WIO_onchip_beta"


def write_private(path, data):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def fixtures():
    DIRECTORY.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(DIRECTORY, 0o700)
    for name in ("companion.seed", "operator.seed"):
        path = DIRECTORY / name
        if not path.exists():
            write_private(path, secrets.token_bytes(32))
        private_file(path, 32)
    for name in ("password", "role-password"):
        password = DIRECTORY / name
        if not password.exists():
            write_private(password, secrets.token_urlsafe(10).encode("ascii"))
        private_file(password, 15)
    print(f"Disposable private fixtures: {DIRECTORY}")


def status(host):
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=5) as response:
        value = json.load(response)
    if value["profile"]["frequency_hz"] != 912525000:
        raise ValueError("Refusing hardware operation: lab is not on 912.525 MHz")
    return value


def check():
    if not device_port().is_symlink() or device_port().resolve().name not in ("ttyACM1", "ttyACM2"):
        raise ValueError("Expected isolated by-id mapping unavailable; never use production ttyACM0")
    result = subprocess.run(
        [sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", str(device_port()),
         "--after", "hard_reset", "chip_id"], check=True, capture_output=True, text=True)
    if "MAC: " + device_mac() not in result.stdout:
        raise ValueError("ESP32 hardware MAC does not match the isolated mast")
    print(f"Verified isolated mast {device_mac()}: {device_port()} -> {device_port().resolve()}")


def backup():
    lab, gateway = status(inventory_value("mast_host")), status(inventory_value("gateway_host"))
    check()
    for name, value in (("before.json", lab), ("gateway.json", gateway)):
        path = DIRECTORY / name
        if not path.exists():
            write_private(path, json.dumps(value).encode())
    path = DIRECTORY / "flash-before.bin"
    if path.exists():
        raise ValueError("Backup exists; refusing to overwrite")
    with (DIRECTORY / "backup.log").open("w") as output:
        subprocess.run(["make", "-C", str(ROOT), "firmware-backup",
                        f"UPLOAD_PORT={device_port()}", f"FIRMWARE_BACKUP={path}", "FLASH_SIZE=0x800000"],
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    if path.stat().st_size != 0x800000:
        raise ValueError("Incomplete full-flash backup")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    write_private(DIRECTORY / "flash-before.sha256", (digest + "\n").encode())
    print(f"Verified private 8 MiB flash backup: {path}; SHA256 {digest}")


def build(incremental=False):
    shared = ("WifiKissMultiplexer.cpp", "WifiKissMultiplexer.h",
              "RadioDashboard.cpp", "RadioDashboard.h", "QueuedTxProtocol.h")
    baseline = DIRECTORY / "field-shared-baseline.json"
    shared_dir = BUILD / "examples/kiss_modem"
    if incremental:
        hashes = {name: hashlib.sha256((shared_dir / name).read_bytes()).hexdigest() for name in shared}
        if baseline.exists():
            if json.loads(private_file(baseline, 8192)) != hashes:
                raise ValueError("Prepared field shared sources changed; refusing mixed-fabric rebuild")
        else:
            write_private(baseline, json.dumps(hashes).encode())
    env = os.environ.copy()
    env.update(MESHCORE_HOSTNAME="meshcore-beta-lab", WIFI_SSID="", WIFI_PWD="",
               ONCHIP_ADMIN_PASSWORD=private_file(DIRECTORY / "role-password", 15).decode(),
               ONCHIP_MAST_PASSWORD=private_file(DIRECTORY / "password", 15).decode(),
               ONCHIP_SERVICE_REGION="",
               ONCHIP_ROOM_PASSWORD="", ONCHIP_MQTT_URI="",
               ONCHIP_TRUSTED_COMPANION_PUBKEY=public_hex(private_file(DIRECTORY / "companion.seed", 32)),
               ONCHIP_OPERATOR_PUBKEY=public_hex(private_file(DIRECTORY / "operator.seed", 32)))
    log = DIRECTORY / ("field-rebuild.log" if incremental else "build.log")
    with log.open("w") as output:
        subprocess.run(["make", "-C", str(ROOT / "firmware/esp32"),
                        "field-rebuild" if incremental else "bot-firmware",
                        f"BUILD={BUILD}", f"CONFIG={ROOT / 'firmware/esp32/platformio.ini.example'}",
                        f"ENV={ENV}"], env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    if incremental:
        if any(hashlib.sha256((shared_dir / name).read_bytes()).hexdigest() != hashes[name] for name in shared):
            raise ValueError("Shared field sources changed during incremental build")
    print(f"Built disposable-credential beta image; private build log: {log}")


def flash():
    baseline = DIRECTORY / "field-shared-baseline.json"
    if baseline.exists():
        shared_dir = BUILD / "examples/kiss_modem"
        if any(hashlib.sha256((shared_dir / name).read_bytes()).hexdigest() != digest
               for name, digest in json.loads(private_file(baseline, 8192)).items()):
            raise ValueError("Refusing field flash with changed shared-source baseline")
    backup = DIRECTORY / "flash-before.bin"
    digest = (DIRECTORY / "flash-before.sha256").read_text().strip()
    if backup.stat().st_size != 0x800000 or hashlib.sha256(backup.read_bytes()).hexdigest() != digest:
        raise ValueError("Flash backup integrity mismatch")
    image_dir = BUILD / ".pio/build" / ENV
    partition = (image_dir / "partitions.bin").read_bytes()
    with backup.open("rb") as source:
        source.seek(0x8000)
        if source.read(len(partition)) != partition:
            raise ValueError("Partition layout differs; refusing app-only flash")
    image = image_dir / "firmware.bin"
    if not 0 < image.stat().st_size <= 3342336:
        raise ValueError("Invalid application size")
    check()
    address, capacity = live_app_slot(ESPTOOL, device_port(), DIRECTORY, partition)
    if image.stat().st_size > capacity:
        raise ValueError("Application image exceeds selected OTA partition")
    with (DIRECTORY / "flash.log").open("w") as output:
        subprocess.run([sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", str(device_port()),
                        "--baud", "921600", "--after", "hard_reset", "write_flash",
                        hex(address), str(image)], check=True, stdout=output, stderr=subprocess.STDOUT)
    print("Flashed isolated mast application only; bootloader, partitions, NVS and SPIFFS preserved")


def boot():
    import serial
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = str(device_port()), 115200, .2
    port.dtr = False
    port.rts = False
    port.open()
    until = time.monotonic() + 12
    output = bytearray()
    with port:
        while time.monotonic() < until:
            output.extend(port.read(1024))
            if len(output) > 65536:
                raise ValueError("Boot log budget exceeded")
    (DIRECTORY / "boot.log").write_bytes(output)
    print(output.decode("utf-8", errors="replace"))


def rf_test():
    import serial
    import threading
    from tools.hardware.management_rf_checks import main as radio_main
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = str(device_port()), 115200, .2
    port.dtr = False
    port.rts = False
    port.open()
    done = threading.Event()
    errors = []

    def capture():
        try:
            total = 0
            with port, (DIRECTORY / "rf-serial.log").open("wb") as output:
                while not done.is_set():
                    data = port.read(2048)
                    total += len(data)
                    if total > 1024 * 1024:
                        raise ValueError("RF acceptance serial log exceeded 1 MiB")
                    output.write(data)
                    output.flush()
        except (OSError, ValueError) as error:
            errors.append(error)

    reader = threading.Thread(target=capture)
    reader.start()
    try:
        time.sleep(2)
        radio_main()
    finally:
        done.set()
        reader.join(timeout=3)
    if reader.is_alive() or errors:
        raise RuntimeError(f"RF serial capture failed: {errors}")
    print(f"Bounded physical test serial metrics: {DIRECTORY / 'rf-serial.log'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fixtures", "check", "backup", "build", "rebuild", "flash", "boot", "rf-test"))
    args = parser.parse_args()
    os.umask(0o077)
    try:
        fixtures()
        actions = {"check": check, "backup": backup, "build": build, "rebuild": lambda: build(True),
                   "flash": flash, "boot": boot, "rf-test": rf_test}
        if args.action != "fixtures":
            actions[args.action]()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Isolated beta lab operation failed: {error}\n")


if __name__ == "__main__":
    main()
