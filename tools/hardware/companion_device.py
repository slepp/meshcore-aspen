#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Backup, update and restore the explicitly selected companion radio."""
import argparse
import hashlib
import json
import os
import subprocess
import time
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import ROOT, ESPTOOL, status, write_private

from tools.hardware.inventory import value as inventory_value

DIRECTORY = ROOT / ".tmp/onchip-beta-stock"
def device_port():
    return Path(inventory_value("companion_port"))
def device_mac():
    return inventory_value("companion_mac")
STOCK = ROOT / ".tmp/stock-MeshCore"
IMAGE = STOCK / ".pio/build/Stock_Xiao_S3_WIO_companion_usb"


def check():
    if not device_port().is_symlink() or device_port().resolve().name == "ttyACM0":
        raise ValueError("Isolated peer stable by-id path unavailable")
    result = subprocess.run(
        [sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", str(device_port()),
         "--after", "hard_reset", "read_mac"], capture_output=True, text=True, check=True)
    if "MAC: " + device_mac() not in result.stdout:
        raise ValueError("Isolated stock peer hardware MAC mismatch")
    print(f"Verified isolated stock peer {device_mac()}: {device_port()}", flush=True)


def backup():
    before = status(inventory_value("gateway_host"))
    if before["kiss"]["connected"] or before["scheduler"]["queued"]:
        raise ValueError("Isolated gateway has active clients/jobs; refusing disruption")
    check()
    path = DIRECTORY / "flash-before.bin"
    if path.exists():
        raise ValueError("Existing backup will not be overwritten")
    snapshot = DIRECTORY / "before.json"
    if not snapshot.exists():
        write_private(snapshot, json.dumps(before).encode())
    with (DIRECTORY / "backup.log").open("w") as output:
        subprocess.run(["make", "-C", str(ROOT), "firmware-backup",
                        f"UPLOAD_PORT={device_port()}", f"FIRMWARE_BACKUP={path}", "FLASH_SIZE=0x800000"],
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    if path.stat().st_size != 0x800000:
        raise ValueError("Incomplete stock-peer backup")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    write_private(DIRECTORY / "flash-before.sha256", (digest + "\n").encode())
    print(f"Verified private 8 MiB stock-peer backup: {path}", flush=True)


def verified_backup():
    path = DIRECTORY / "flash-before.bin"
    digest = (DIRECTORY / "flash-before.sha256").read_text().strip()
    if path.stat().st_size != 0x800000 or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError("Stock-peer backup size/hash mismatch")
    return path


def flash(require_backup=True):
    backup = verified_backup() if require_backup else None
    revision = subprocess.check_output(
        ["git", "-C", str(STOCK), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(
        ["git", "-C", str(STOCK), "status", "--porcelain"], text=True).strip()
    if revision != "d92964352441e53b93e8667b802e04f6e072b39e" or dirty:
        raise ValueError("Stock MeshCore source provenance mismatch")
    partitions = (IMAGE / "partitions.bin").read_bytes()
    if backup is not None:
        with backup.open("rb") as original:
            original.seek(0x8000)
            if original.read(len(partitions)) != partitions:
                raise ValueError("Stock partition layout differs; app-only flash refused")
    firmware = IMAGE / "firmware.bin"
    if not 0 < firmware.stat().st_size <= 3342336:
        raise ValueError("Stock application size invalid")
    check()
    with (DIRECTORY / "flash.log").open("w") as output:
        subprocess.run(
            [sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", str(device_port()),
             "--baud", "921600", "--after", "hard_reset", "write_flash", "0x10000", str(firmware)],
            stdout=output, stderr=subprocess.STDOUT, check=True)
    print("Isolated peer now has unchanged stock 1.17.1 companion application", flush=True)


def restore():
    original = verified_backup()
    check()
    with (DIRECTORY / "restore.log").open("w") as output:
        subprocess.run(["make", "-C", str(ROOT), "firmware-restore",
                        f"UPLOAD_PORT={device_port()}", f"FIRMWARE_BACKUP={original}"],
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    verify()


def verify():
    before = json.loads((DIRECTORY / "before.json").read_text())
    deadline = time.monotonic() + 60
    while True:
        try:
            after = status(inventory_value("gateway_host"))
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(2)
    fields = ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm",
              "airtime_factor", "cad", "interference_threshold")
    if (any(after["profile"][field] != before["profile"][field] for field in fields) or
            after["profile"]["fault"] or
            after["kiss"]["capacity"] != before["kiss"]["capacity"]):
        raise ValueError("Restored gateway PHY/capacity differs from its backup snapshot")
    (DIRECTORY / "restored.json").write_text(json.dumps(after, indent=2) + "\n")
    print("Verified restored gateway HTTP, original 912.525 MHz PHY and KISS capacity", flush=True)
    return after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "backup", "flash", "flash-forward", "restore", "verify"))
    args = parser.parse_args()
    os.umask(0o077)
    DIRECTORY.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(DIRECTORY, 0o700)
    try:
        {"check": check, "backup": backup, "flash": flash,
         "flash-forward": lambda: flash(require_backup=False),
         "restore": restore, "verify": verify}[args.action]()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Stock-peer operation failed: {error}\n")


if __name__ == "__main__":
    main()
