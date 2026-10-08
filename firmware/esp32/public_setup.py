#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create a private offline Aspen SPIFFS image; optionally install it over USB."""
import argparse
import contextlib
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parents[2]
FORMAT = struct.Struct("<4sBBBBIIBBBB16s16s16s65s65s33s65sB7x")
MAX_PROFILE = 4096
FIELDS = {
    "schema", "roles", "path_width", "frequency_hz", "bandwidth_hz", "sf", "cr",
    "tx_dbm", "admin_password", "room_password", "mast_password",
    "operator_public_key", "trusted_companion_public_key",
    "wifi_ssid", "wifi_password", "wifi_enabled",
}
BANDWIDTHS = {7800, 7810, 10400, 10420, 15600, 15630, 20800, 20830,
              31250, 41700, 62500, 125000, 250000, 500000}


def private_parent(value):
    path = Path(os.path.abspath(Path(value).expanduser()))
    if path.is_relative_to(ROOT):
        raise ValueError("Private setup files must be outside the repository")
    # Reject symlinks in every existing path component, including the final file.
    for component in (path, *path.parents):
        if component.is_symlink():
            raise ValueError("Private setup path must not contain symlinks")
    parent = path.parent
    info = parent.stat()
    if info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("Private setup directory must be owner-only (mode 0700)")
    return path


@contextmanager
def private_directory(value):
    path = private_parent(value)
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parent.parts[1:]:
            next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                              dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_fd
        info = os.fstat(descriptor)
        if info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError("Private setup directory must be owner-only (mode 0700)")
        yield path, descriptor
    finally:
        os.close(descriptor)


def read_private(value, maximum):
    with private_directory(value) as (path, directory):
        fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        with os.fdopen(fd, "rb") as source:
            info = os.fstat(source.fileno())
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    stat.S_IMODE(info.st_mode) != 0o600):
                raise ValueError("Private setup input must be an owner-owned regular file (mode 0600)")
            content = source.read(maximum + 1)
    if len(content) > maximum:
        raise ValueError("Private setup input exceeds its size limit")
    return content


def create_private(value, content):
    with private_directory(value) as (path, directory):
        fd = os.open(path.name, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        try:
            with os.fdopen(fd, "wb") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
        except BaseException:
            os.unlink(path.name, dir_fd=directory)
            raise
    return path


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Setup JSON contains a duplicate field")
        result[key] = value
    return result


def integer(profile, key, low, high):
    value = profile[key]
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"Setup field {key} is outside its integer range")
    return value


def text(profile, key, capacity, required=False):
    value = profile[key]
    if (type(value) is not str or not (int(required) <= len(value) < capacity) or
            any(ord(c) < 32 or ord(c) > 126 for c in value)):
        raise ValueError(f"Setup field {key} must be printable ASCII within its length limit")
    return value.encode("ascii")


def key(profile, name, required=False):
    value = text(profile, name, 65, required)
    if not value and not required:
        return value
    if (len(value) != 64 or any(c not in b"0123456789abcdef" for c in value) or
            value in (b"0" * 64, b"f" * 64)):
        raise ValueError(f"Setup field {name} must be a lowercase 32-byte verification public key")
    return value


def encode_profile(content):
    if not isinstance(content, (str, bytes, bytearray)) or len(content) > MAX_PROFILE:
        raise ValueError("Setup JSON exceeds its size limit or has the wrong input type")
    try:
        profile = json.loads(content, object_pairs_hook=unique_object)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("Setup JSON is malformed") from error
    if type(profile) is not dict or set(profile) != FIELDS:
        raise ValueError("Setup JSON must contain exactly the documented fields")
    integer(profile, "schema", 1, 1)
    bandwidth = integer(profile, "bandwidth_hz", 7800, 500000)
    if bandwidth not in BANDWIDTHS:
        raise ValueError("Setup bandwidth_hz is not an SX1262 bandwidth")
    if type(profile["wifi_enabled"]) is not bool:
        raise ValueError("Setup wifi_enabled must be a JSON boolean")
    ssid = text(profile, "wifi_ssid", 33, profile["wifi_enabled"])
    wifi_password = text(profile, "wifi_password", 65)
    if wifi_password and not 8 <= len(wifi_password) <= 63:
        if len(wifi_password) != 64 or any(c not in b"0123456789abcdefABCDEF" for c in wifi_password):
            raise ValueError("Setup wifi_password must be empty, 8..63 bytes or a 64-digit hex PSK")
    record = FORMAT.pack(
        b"MCP\1", integer(profile, "roles", 0, 15), integer(profile, "path_width", 1, 3), 0, 0,
        integer(profile, "frequency_hz", 150000000, 960000000), bandwidth,
        integer(profile, "sf", 5, 12), integer(profile, "cr", 5, 8),
        integer(profile, "tx_dbm", 0, 22), 0,
        text(profile, "admin_password", 16, True), text(profile, "room_password", 16),
        text(profile, "mast_password", 16, True), key(profile, "operator_public_key", True),
        key(profile, "trusted_companion_public_key"), ssid, wifi_password, int(profile["wifi_enabled"]),
    )
    return record + hashlib.sha256(record).digest()


def storage_partitions(content):
    if len(content) not in (0xc00, 0x1000):
        raise ValueError("Expected a 3072- or 4096-byte ESP partition table")
    entries = {}
    for offset in range(0, len(content), 32):
        row = content[offset:offset + 32]
        if len(row) != 32 or row[:2] != b"\xaa\x50":
            break
        _, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", row)
        name = label.rstrip(b"\0").decode("ascii")
        if name in entries or flags:
            raise ValueError("Unsupported or duplicate partition")
        entries[name] = (kind, subtype, start, size)
    if (entries.get("app0") != (0, 16, 0x10000, 0x330000) or
            entries.get("app1") != (0, 17, 0x340000, 0x330000) or
            entries.get("nvs") != (1, 2, 0x9000, 0x5000) or
            entries.get("spiffs", ())[:3] != (1, 130, 0x670000) or
            entries["spiffs"][3] != 0x180000 or
            entries.get("coredump") != (1, 3, 0x7f0000, 0x10000)):
        raise ValueError("Expected Aspen 8 MiB dual-slot partition layout")
    return entries


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        # Tools may print filesystem contents; do not include their output.
        raise ValueError(f"USB/image tool failed (exit {result.returncode}); no credentials printed")


def build_image(profile, output, partitions, mkspiffs):
    record = encode_profile(read_private(profile, MAX_PROFILE))
    entries = storage_partitions(Path(partitions).read_bytes())
    output = private_parent(output)
    with tempfile.TemporaryDirectory(prefix="aspen-setup-", dir=output.parent) as temporary:
        directory = Path(temporary)
        data = directory / "data"
        data.mkdir(mode=0o700)
        create_private(data / "onchip-layout", b"meshcore-onchip-fs-v1\n")
        create_private(data / "public-setup.bin", record)
        image = directory / "spiffs.bin"
        run([str(mkspiffs), "-c", str(data), "-b", "4096", "-p", "256",
             "-s", str(entries["spiffs"][3]), str(image)])
        image.chmod(0o600)
        content = read_private(image, entries["spiffs"][3])
        if len(content) != entries["spiffs"][3]:
            raise ValueError("Generated setup image has the wrong SPIFFS size")
        create_private(output, content)
    print("Private SPIFFS setup image created (mode 0600); keep it outside public release artifacts.")


def install_config(image, partitions, backup, esptool, port, confirm_replace=False):
    expected = Path(partitions).read_bytes()
    entries = storage_partitions(expected)
    content = read_private(image, entries["spiffs"][3])
    if len(content) != entries["spiffs"][3]:
        raise ValueError("Setup image size does not match the SPIFFS partition")
    backup = private_parent(backup)
    if backup.exists():
        raise ValueError("SPIFFS backup path already exists; setup refused")
    # Inspect NVS in memory, never export its identity/private-key records.
    sys.path.insert(0, str(Path(esptool).resolve().parent))
    try:
        module = importlib.import_module("esptool")
    except ImportError as error:
        raise ValueError("Use esptool 4.5.1 and its installed Python dependencies") from error
    if module.__version__ != "4.5.1":
        raise ValueError("USB setup requires esptool 4.5.1")
    connection = None
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            connection = module.detect_chip(port=port)
            if connection.CHIP_NAME != "ESP32-S3":
                raise ValueError("USB setup requires an ESP32-S3")
            connection = connection.run_stub()
            table = connection.read_flash(0x8000, len(expected))
            nvs_start, nvs_size = entries["nvs"][2:]
            nvs_blank = connection.read_flash(nvs_start, nvs_size) == b"\xff" * nvs_size
            start, size = entries["spiffs"][2:]
            filesystem = connection.read_flash(start, size)
    except module.FatalError as error:
        raise ValueError("USB storage inspection failed; no configuration written") from error
    finally:
        if connection is not None:
            connection._port.close()
    if len(table) != len(expected) or len(filesystem) != entries["spiffs"][3]:
        raise ValueError("USB storage inspection incomplete; setup refused")
    if table != expected and table != b"\xff" * len(expected):
        raise ValueError("Installed partition table differs; setup refused")
    create_private(backup, filesystem)
    blank_storage = nvs_blank and filesystem == b"\xff" * len(filesystem)
    if not blank_storage and not confirm_replace:
        raise ValueError("Existing NVS/SPIFFS detected; setup refused. Backup retained. "
                         "Replacing SPIFFS destroys role preferences, ACLs, programs and caller banks; "
                         "requires --confirm-replace-spiffs-with-backup")
    with tempfile.TemporaryDirectory(prefix="aspen-usb-", dir=backup.parent) as temporary:
        copied = create_private(Path(temporary) / "spiffs.bin", content)
        run([str(esptool), "--chip", "esp32s3", "--port", port, "--after", "no_reset",
             "write_flash", hex(entries["spiffs"][2]), str(copied)])
        run([str(esptool), "--chip", "esp32s3", "--port", port, "--after", "no_reset",
             "verify_flash", hex(entries["spiffs"][2]), str(copied)])
    print("Private SPIFFS image installed and verified; SPIFFS backup retained, NVS untouched. USB bootloader left active.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    image = commands.add_parser("image", help="Build an offline private SPIFFS image; does not access hardware")
    image.add_argument("--profile", required=True)
    image.add_argument("--output", required=True)
    image.add_argument("--partitions", required=True)
    image.add_argument("--mkspiffs", required=True)
    install = commands.add_parser("install-config", help="USB ONLY: replace SPIFFS, backing up and checking storage first")
    install.add_argument("--image", required=True)
    install.add_argument("--partitions", required=True)
    install.add_argument("--backup", required=True)
    install.add_argument("--esptool", required=True)
    install.add_argument("--port", required=True)
    install.add_argument("--confirm-replace-spiffs-with-backup", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "image":
            build_image(args.profile, args.output, args.partitions, args.mkspiffs)
        else:
            install_config(args.image, args.partitions, args.backup, args.esptool, args.port,
                           args.confirm_replace_spiffs_with_backup)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Public setup failed: {error}\n")


if __name__ == "__main__":
    main()
