#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read the live ESP OTA selection before an app-only USB recovery flash."""
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zlib


def app_slot(partitions, otadata):
    entries = []
    for offset in range(0, len(partitions), 32):
        record = partitions[offset:offset + 32]
        if len(record) != 32 or record[:2] != b"\xaa\x50":
            break
        _, kind, subtype, address, size = struct.unpack_from("<HBBII", record)
        entries.append((kind, subtype, address, size))
    apps = sorted((subtype, address, size) for kind, subtype, address, size in entries
                  if kind == 0 and 0x10 <= subtype <= 0x1f)
    if len(apps) != 2 or [a[0] for a in apps] != [0x10, 0x11]:
        raise ValueError("Expected exactly app0/app1 OTA application partitions")
    if len(otadata) < 4096 + 32:
        raise ValueError("Incomplete OTA selection data")
    candidates = []
    for offset in (0, 4096):
        record = otadata[offset:offset + 32]
        seq, = struct.unpack_from("<I", record)
        state, crc = struct.unpack_from("<II", record, 24)
        if seq not in (0, 0xffffffff) and crc == zlib.crc32(record[:4], 0xffffffff):
            if state in (0, 1):
                raise ValueError("OTA application is not health-confirmed; confirm or roll back before USB flash")
            if state in (2, 0xffffffff):
                candidates.append(seq)
    if candidates:
        return apps[(max(candidates) - 1) % len(apps)][1:]
    if otadata[:32] == b"\xff" * 32 and otadata[4096:4096 + 32] == b"\xff" * 32:
        return apps[0][1:]
    raise ValueError("OTA selection corrupt or invalid; refusing to guess application offset")


def live_app_slot(esptool, port, directory, expected_partitions=None):
    directory = Path(directory)
    paths = [directory / ("ota-selection-" + uuid.uuid4().hex + suffix)
             for suffix in ("-partitions.bin", "-otadata.bin")]
    base = [sys.executable, str(esptool), "--chip", "esp32s3", "--port", str(port),
            "--baud", "921600", "--after", "no_reset", "read_flash"]
    try:
        subprocess.run(base + ["0x8000", "0x1000", str(paths[0])], check=True,
                       stdout=subprocess.DEVNULL)
        partitions = paths[0].read_bytes()
        if expected_partitions is not None and not partitions.startswith(expected_partitions):
            raise ValueError("Live partition table differs; refusing app-only flash")
        ota = []
        for offset in range(0, len(partitions), 32):
            record = partitions[offset:offset + 32]
            if len(record) != 32 or record[:2] != b"\xaa\x50":
                break
            _, kind, subtype, address, size = struct.unpack_from("<HBBII", record)
            if kind == 1 and subtype == 0:
                ota.append((address, size))
        if len(ota) != 1 or ota[0][1] != 8192:
            raise ValueError("Expected one 8 KiB OTA selection partition")
        subprocess.run(base + [hex(ota[0][0]), hex(ota[0][1]), str(paths[1])], check=True,
                       stdout=subprocess.DEVNULL)
        return app_slot(partitions, paths[1].read_bytes())
    finally:
        for path in paths:
            path.unlink(missing_ok=True)
