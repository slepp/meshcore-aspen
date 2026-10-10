#!/usr/bin/env python3
"""Create or verify a signed Pine application manifest; upload only to an inventoried XIAO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from ble_field import APP_START, APP_LIMIT, validate_package

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BOARDS = {"xiao-nrf52840-wio-sx1262": "nrfmast_fleet_lua",
          "sensecap-solar": "nrfmast_solar_lua"}
SIGNER = ROOT / "release/pine-update.pub"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def create_manifest(package, board, source, version):
    checked = validate_package(package)
    if not re.fullmatch(r"[0-9a-f]{40}", source) or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Source must be a full commit hash; version must be MAJOR.MINOR.PATCH")
    return {"schema": 1, "product": "pine", "version": version, "source": source,
            "board": board, "profile": BOARDS[board], "update_only": True,
            "layout": {"application_start": APP_START, "application_limit": APP_LIMIT,
                       "softdevice_id": 0x123},
            "application": {"bytes": checked["bytes"], "sha256": checked["application_sha256"]},
            "package": {"file": package.name, "sha256": sha(package)},
            "dependencies": {"meshcore": "d92964352441e53b93e8667b802e04f6e072b39e",
                             "lua": "5.5.1", "platform": "nordicnrf52@11.0.0",
                             "framework": "1.10701.0+sha.d5413016"}}


def verify(manifest, signature, package, board, signer=SIGNER):
    raw = manifest.read_bytes()
    if len(raw) > 8192:
        raise ValueError("Pine manifest exceeds 8192 bytes")
    key = signer.read_text().strip().split()
    if len(key) < 2 or key[0] != "ssh-ed25519" or not re.fullmatch(r"[A-Za-z0-9+/=]+", key[1]):
        raise ValueError("Pine verification key must be an SSH Ed25519 public key")
    build = HERE / ".build"
    build.mkdir(exist_ok=True)
    allowed = build / f"pine-signers-{os.getpid()}"
    fd = os.open(allowed, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write("pine-release " + " ".join(key[:2]) + "\n")
        result = subprocess.run(["ssh-keygen", "-Y", "verify", "-n", "pine-update",
                                 "-I", "pine-release", "-f", str(allowed), "-s", str(signature)],
                                input=raw, capture_output=True)
        if result.returncode:
            raise ValueError("Pine manifest signature does not match the trusted release key")
    finally:
        allowed.unlink()
    data = json.loads(raw)
    if data.get("product") != "pine" or data.get("schema") != 1 or data.get("board") != board:
        raise ValueError("Signed manifest product, schema or hardware differs from selected Pine")
    expected = create_manifest(package, board, data.get("source", ""), data.get("version", ""))
    if data != expected:
        raise ValueError("Signed Pine manifest differs from package hash, application, layout, profile or dependencies")
    return data


def read_command(link, command, prefix):
    from hardware import serial_command
    for _ in range(3):
        response = serial_command(link, command, 6).strip()
        if response.startswith(prefix) and "\n" not in response:
            return response
        time.sleep(1)
        link.reset_input_buffer()
    raise RuntimeError("Pine readback unavailable: " + command + "; no update attempted or retried")


def flash(data, args):
    snapshot = HERE / ".build" / f"pine-update-{os.getpid()}.zip"
    with args.zip.open("rb") as source:
        raw = source.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024 or hashlib.sha256(raw).hexdigest() != data["package"]["sha256"]:
        raise ValueError("Pine package changed after signature verification; no update attempted")
    fd = os.open(snapshot, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
    original = args.zip
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(raw)
        args.zip = snapshot
        flash_snapshot(data, args)
    finally:
        args.zip = original
        snapshot.unlink(missing_ok=True)


def flash_snapshot(data, args):
    from hardware import connection, bootloader, require_bootloader, usb_serial
    from serial.tools.list_ports import comports
    if args.board != "xiao-nrf52840-wio-sx1262":
        raise ValueError("Serial deployment currently supports the inventoried XIAO; Solar verification is offline")
    queries = {"ids": "ids ", "get radio": "> ", "get tx": "> ", "get repeat": "> ",
               "get path.hash.mode": "> ", "source hash": "SHA256 ", "get bot.ble": "BLE "}
    with connection(str(args.port)) as link:
        time.sleep(.2)
        link.reset_input_buffer()
        running = read_command(link, "get application", "application ")
        if not running.lower().endswith("sha256=" + args.current_sha.lower()):
            raise ValueError("Running Pine application differs from --current-sha; no update attempted")
        update = read_command(link, "get update", "BLE-DFU=")
        if " boot=0x000601 " not in update:
            raise ValueError("Pine bootloader differs from supported Legacy 0.6.1; no update attempted")
        before = {command: read_command(link, command, prefix) for command, prefix in queries.items()}
    bootloader(str(args.port))
    entries = [entry for entry in comports() if entry.vid == 0x2886 and
               entry.serial_number == usb_serial() and entry.pid in (0x44, 0x45)]
    if len(entries) != 1:
        raise RuntimeError("Inventoried Pine bootloader endpoint unavailable; no upload attempted")
    require_bootloader(entries[0].device)
    uploader = Path.home() / ".platformio/packages/tool-adafruit-nrfutil/adafruit-nrfutil.py"
    environment = dict(os.environ, TMPDIR=str(HERE / ".build"))
    # One upload only. Its failure can be uncertain; inspect the device before another invocation.
    try:
        result = subprocess.run(["python3", str(uploader), "dfu", "serial", "--package", str(args.zip),
                                 "--port", entries[0].device, "--baudrate", "115200"],
                                env=environment, capture_output=True, timeout=240)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("Pine upload timed out; completion uncertain; inspect the running hash or bootloader before retrying") from error
    if result.returncode or b"Device programmed." not in result.stdout + result.stderr:
        raise RuntimeError("Pine upload completion uncertain; inspect running application or retained bootloader before retrying")
    deadline = time.monotonic() + 30
    while not args.port.exists() and time.monotonic() < deadline:
        time.sleep(.2)
    time.sleep(8)
    with connection(str(args.port)) as link:
        link.reset_input_buffer()
        read_command(link, "ver", "vpine-" + data["version"] + " ")
        running = read_command(link, "get application", "application ")
        expected = f'application start=0x27000 bytes={data["application"]["bytes"]} sha256={data["application"]["sha256"]}'
        if running.lower() != expected:
            raise RuntimeError("Pine running hash differs after update; do not blindly upload again")
        for command, prefix in queries.items():
            if read_command(link, command, prefix) != before[command]:
                raise RuntimeError("Pine retained state differs: " + command + "; inspect protected backup")
    print("Pine application verified; identities, PHY, BLE and source retained; bootloader unchanged")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    create = sub.add_parser("manifest")
    create.add_argument("--source", required=True)
    create.add_argument("--version", required=True)
    create.add_argument("--output", type=Path, required=True)
    for name in ("verify", "flash"):
        command = sub.add_parser(name)
        command.add_argument("--manifest", type=Path, required=True)
        command.add_argument("--signature", type=Path, required=True)
        command.add_argument("--signer", type=Path, default=SIGNER, help="Trusted public key; obtain independently of the download")
        if name == "flash":
            command.add_argument("--port", type=Path, required=True)
            command.add_argument("--current-sha", required=True, help="Previously read running application SHA256")
    for command in (create, sub.choices["verify"], sub.choices["flash"]):
        command.add_argument("--zip", type=Path, required=True)
        command.add_argument("--board", choices=BOARDS, required=True)
    args = parser.parse_args()
    if args.operation == "manifest":
        data = create_manifest(args.zip, args.board, args.source, args.version)
        with args.output.open("x") as stream:
            json.dump(data, stream, indent=2)
            stream.write("\n")
        print("Pine application manifest created; sign it with namespace pine-update")
    else:
        data = verify(args.manifest, args.signature, args.zip, args.board, args.signer)
        print(f'Signed Pine {data["version"]} application verified for {args.board}')
        if args.operation == "flash":
            if not re.fullmatch(r"[0-9a-fA-F]{64}", args.current_sha):
                raise ValueError("--current-sha must be the previously read SHA256")
            flash(data, args)


if __name__ == "__main__":
    main()
