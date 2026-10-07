#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline readiness and private operator builds only: no serial, RF or device writes."""
import argparse
import fcntl
import hashlib
import ipaddress
import json
import math
import os
import re
import shlex
import ssl
import struct
import subprocess

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY as LAB, device_mac, ROOT, write_private
from tools.hardware.admin import private_file
from tools.hardware.rf import public_hex
from tools.hardware.wifi_checks import environment_file, lan_credentials
from tools.hardware.inventory import value as inventory_value

DIRECTORY = ROOT / ".tmp/onchip-https-hardware"
BUILD = ROOT / ".tmp/onchip-https-operator"
ENV = "Xiao_S3_WIO_onchip_https_probe"
BACKUP = ROOT / ".tmp/onchip-owner-field/pre-real.bin"
def backup_sha256():
    return inventory_value("backup_sha256")
KEYS = ("ONCHIP_BOT_HOME_ADDRESS", "ONCHIP_BOT_HOME_HOST", "ONCHIP_BOT_HOME_PORT",
        "ONCHIP_BOT_HOME_CA_FILE", "ONCHIP_BOT_HOME_OPERATIONS",
        "ONCHIP_BOT_HOME_TOKEN_FILE", "MESHCORE_BOT_SERVICE_TOKEN")


def settings(path):
    values = {}
    for line in private_file(path, 16384).decode("utf-8").splitlines():
        name, separator, value = line.strip().removeprefix("export ").partition("=")
        name = name.strip()
        if not separator or name not in KEYS:
            continue
        if name in values:
            raise ValueError("Duplicate HTTPS setting; values withheld")
        try:
            words = shlex.split(value, comments=True)
        except ValueError:
            raise ValueError("Malformed HTTPS setting; values withheld") from None
        if len(words) != 1:
            raise ValueError("HTTPS setting requires one literal value; values withheld")
        values[name] = words[0]
    return values


def configuration(values):
    required = ("ONCHIP_BOT_HOME_ADDRESS", "ONCHIP_BOT_HOME_HOST",
                "ONCHIP_BOT_HOME_CA_FILE", "ONCHIP_BOT_HOME_OPERATIONS")
    if any(not values.get(name) for name in required):
        raise ValueError("Approved HTTPS address, hostname, CA file, operation allowlist and service token are required")
    try:
        address = ipaddress.IPv4Address(values["ONCHIP_BOT_HOME_ADDRESS"])
        port = int(values.get("ONCHIP_BOT_HOME_PORT", "443"))
        operations = int(values["ONCHIP_BOT_HOME_OPERATIONS"])
    except ValueError:
        raise ValueError("Invalid HTTPS address, port or operation mask; values withheld") from None
    if (address.is_loopback or address.is_multicast or address.is_unspecified or
            int(address) >> 24 == 0 or int(address) & 255 == 255 or
            not 1 <= port <= 65535 or not 1 <= operations <= 7):
        raise ValueError("HTTPS requires a unicast LAN address, valid port and explicit operation mask")
    host = values["ONCHIP_BOT_HOME_HOST"]
    if len(host) > 253 or not re.fullmatch(r"[A-Za-z0-9.-]+", host):
        raise ValueError("Invalid HTTPS verification hostname; value withheld")
    ca_path = Path(values["ONCHIP_BOT_HOME_CA_FILE"]).expanduser()
    try:
        ca = private_file(ca_path, 4096).decode("ascii")
        if not ca.startswith("-----BEGIN CERTIFICATE-----"):
            raise ValueError()
        ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT).load_verify_locations(cadata=ca)
    except (OSError, ValueError, UnicodeError, ssl.SSLError):
        raise ValueError("Approved bounded CA file is unavailable or invalid; contents withheld") from None
    token = values.get("MESHCORE_BOT_SERVICE_TOKEN", "")
    if values.get("ONCHIP_BOT_HOME_TOKEN_FILE"):
        if token:
            raise ValueError("Provide only one HTTPS token source")
        try:
            token = private_file(Path(values["ONCHIP_BOT_HOME_TOKEN_FILE"]).expanduser(), 257).decode("ascii")
            token = token.removesuffix("\n")
        except (OSError, ValueError, UnicodeError):
            raise ValueError("Private HTTPS token file unavailable; contents withheld") from None
    if not 32 <= len(token) <= 256 or any(not 33 <= ord(c) <= 126 for c in token):
        raise ValueError("Approved bounded HTTPS service token is required; value withheld")
    return {"ONCHIP_BOT_HOME_ADDRESS": str(address), "ONCHIP_BOT_HOME_HOST": host,
            "ONCHIP_BOT_HOME_PORT": port, "ONCHIP_BOT_HOME_CA": ca,
            "ONCHIP_BOT_HOME_TOKEN": token, "ONCHIP_BOT_HOME_OPERATIONS": operations}


def backup_app(raw, expected_sha=None):
    """Validate the backed-up ota_0 partition and its ESP image checksum/digest."""
    if len(raw) != 0x800000 or hashlib.sha256(raw).hexdigest() != (expected_sha or backup_sha256()):
        raise ValueError("Verified 8 MiB mast rollback backup is unavailable")
    partition = None
    for offset in range(0x8000, 0x9000, 32):
        magic, kind, subtype, start, size = struct.unpack_from("<HBBII", raw, offset)
        if magic == 0x50AA and kind == 0 and subtype == 0x10:
            partition = (start, size)
    if partition != (0x10000, 0x330000):
        raise ValueError("Unexpected mast application partition; no rollback image prepared")
    image = raw[partition[0]:sum(partition)]
    if image[0] != 0xE9 or not 1 <= image[1] <= 16 or image[23] != 1:
        raise ValueError("Unsupported backed-up ESP application header")
    offset, checksum = 24, 0xEF
    for _ in range(image[1]):
        if offset + 8 > len(image):
            raise ValueError("Truncated backed-up ESP segment")
        size = struct.unpack_from("<I", image, offset + 4)[0]
        offset += 8
        if size > len(image) - offset:
            raise ValueError("Oversized backed-up ESP segment")
        for byte in image[offset:offset + size]:
            checksum ^= byte
        offset += size
    end = (offset + 16) & ~15
    if end + 32 > len(image) or image[end - 1] != checksum:
        raise ValueError("Backed-up ESP application checksum mismatch")
    if hashlib.sha256(image[:end]).digest() != image[end:end + 32]:
        raise ValueError("Backed-up ESP application digest mismatch")
    return image[:end + 32]


def header(values):
    lines = []
    for name, value in values.items():
        if (not re.fullmatch(r"[A-Z][A-Z0-9_]*", name) or type(value) not in (str, int, float) or
                isinstance(value, float) and not math.isfinite(value)):
            raise ValueError("Invalid native operator definition")
        encoded = ('"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'
                   if isinstance(value, str) else str(value))
        lines.extend((f"#undef {name}", f"#define {name} {encoded}"))
    return ("\n".join(lines) + "\n").encode("ascii")


def memory_header(data):
    if not hasattr(os, "memfd_create"):
        raise ValueError("Private operator builds require Linux memfd support")
    fd = os.memfd_create("onchip-operator", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(os.dup(fd), "wb") as stream:
            stream.write(data)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                    fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
    except OSError:
        os.close(fd)
        raise
    return fd


def private_directory(path):
    if path.is_symlink():
        raise ValueError("Private artifact directory must not be a symlink")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.stat().st_uid != os.getuid() or path.stat().st_mode & 0o077:
        raise ValueError("Artifact directory must be owner-only")


def readiness():
    report = {"hardware_touched": False, "deploy_authorized": False,
              "runtime_endpoint_installer": False, "blockers": []}
    try:
        lan_credentials()
        report["private_lan_configuration"] = True
    except (OSError, ValueError, UnicodeError):
        report["blockers"].append("Private real-LAN WiFi configuration is unavailable")
    try:
        values = settings(environment_file())
        values.update({name: os.environ[name] for name in KEYS if name in os.environ})
        configuration(values)
        report["approved_https_configuration"] = True
    except (OSError, ValueError, UnicodeError) as error:
        # Only locally constructed ValueError messages are exposed.
        report["blockers"].append(str(error) if type(error) is ValueError else
                                  "Private HTTPS configuration could not be read; details withheld")
    try:
        raw = BACKUP.read_bytes()
        app = backup_app(raw)
        report["backup_verified"] = True
        report["rollback_app_bytes"] = len(app)
        report["rollback_app_sha256"] = hashlib.sha256(app).hexdigest()
        frozen = ROOT / ".tmp/onchip-beta-lab-firmware/.pio/build/Xiao_S3_WIO_onchip_beta/firmware.bin"
        report["frozen_build_matches_backup_app"] = frozen.read_bytes() == app
        if not report["frozen_build_matches_backup_app"]:
            report["blockers"].append("Frozen build differs from verified backup app; reconcile running app before exact field rollback")
    except (OSError, ValueError):
        report["blockers"].append("Backup/app rollback verification requires investigation; no flash permitted")
    report["blockers"].extend(("Monitor release/deployment authorization is not attested by this offline tool",
                               "Authorize ownership and application rollback for the selected companion radio",
                               "Physical native certificate-date/SNTP acceptance must precede bot grant activation",
                               "Three-slot profile cannot serve old four-source per_role host; required-mode MKISS rollout or isolated bench must be authorized"))
    return report


def compile_image(native, target, profile=ENV, cloud_config_fd=None):
    private_directory(DIRECTORY)
    private_directory(target)
    env = os.environ.copy()
    for name in (*KEYS, "WIFI_AP", "WIFI_PASSWORD"):
        env.pop(name, None)
    # Existing management principals stay identical; no secret -D arguments.
    for name in (*native, "WIFI_SSID", "WIFI_PWD", "ONCHIP_ADMIN_PASSWORD", "ONCHIP_MAST_PASSWORD",
                 "ONCHIP_TRUSTED_COMPANION_PUBKEY", "ONCHIP_OPERATOR_PUBKEY",
                 "ONCHIP_ROOM_PASSWORD", "ONCHIP_MQTT_URI", "ONCHIP_SERVICE_REGION"):
        env[name] = ""
    env["MESHCORE_HOSTNAME"] = "meshcore-beta-lab"
    definitions = header(native)
    if cloud_config_fd is not None:
        seals = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL
        if (type(cloud_config_fd) is not int or profile != "Xiao_S3_WIO_onchip_cloudroom_probe" or
                fcntl.fcntl(cloud_config_fd, fcntl.F_GET_SEALS) & seals != seals):
            raise ValueError("Cloud-room configuration requires a sealed private header and the cloudroom probe profile")
        definitions += (f'\n#undef ONCHIP_CLOUD_ROOM_CONFIG_HEADER\n'
                        f'#define ONCHIP_CLOUD_ROOM_CONFIG_HEADER "/proc/{os.getpid()}/fd/{cloud_config_fd}"\n').encode()
    fd = memory_header(definitions)
    try:
        env["ONCHIP_OPERATOR_HEADER"] = f"/proc/{os.getpid()}/fd/{fd}"
        log = DIRECTORY / (target.name + ".log")
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW
        with os.fdopen(os.open(log, flags, 0o600), "w") as output:
            subprocess.run(["make", "-s", "-C", str(ROOT / "firmware/esp32"), "bot-firmware",
                            f"BUILD={target}", f"CONFIG={ROOT / 'firmware/esp32/platformio.ini.example'}",
                            f"ENV={profile}"], env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    finally:
        os.close(fd)
    return target / ".pio/build" / profile


def build():
    values = settings(environment_file())
    values.update({name: os.environ[name] for name in KEYS if name in os.environ})
    native = configuration(values)
    app = backup_app(BACKUP.read_bytes())
    ssid, password = lan_credentials()
    native.update(WIFI_SSID=ssid.decode("utf-8"), WIFI_PWD=password.decode("utf-8"),
                  ONCHIP_ADMIN_PASSWORD=private_file(LAB / "role-password", 15).decode(),
                  ONCHIP_MAST_PASSWORD=private_file(LAB / "password", 15).decode(),
                  ONCHIP_TRUSTED_COMPANION_PUBKEY=public_hex(private_file(LAB / "companion.seed", 32)),
                  ONCHIP_OPERATOR_PUBKEY=public_hex(private_file(LAB / "operator.seed", 32)))
    private_directory(DIRECTORY)
    rollback = DIRECTORY / "rollback-app.bin"
    if rollback.exists():
        if private_file(rollback, 0x330000) != app:
            raise ValueError("Existing rollback application differs; refusing overwrite")
    else:
        write_private(rollback, app)
    compile_image(native, BUILD)
    target = BUILD / ".pio/build" / ENV
    image = (target / "firmware.bin").read_bytes()
    partition = (target / "partitions.bin").read_bytes()
    if not 0 < len(image) <= 0x330000 or BACKUP.read_bytes()[0x8000:0x8000 + len(partition)] != partition:
        raise ValueError("Candidate image/partition is incompatible; no flash permitted")
    print(json.dumps({"built": True, "flashed": False, "mac": device_mac(),
                      "candidate_sha256": hashlib.sha256(image).hexdigest(),
                      "rollback_sha256": hashlib.sha256(app).hexdigest(),
                      "private_build": str(BUILD), "runtime_tls_verified": False}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("readiness", "build", "probe-build"))
    args = parser.parse_args()
    os.umask(0o077)
    try:
        if args.action == "readiness":
            report = readiness()
            print(json.dumps(report, indent=2))
            return 2 if report["blockers"] else 0
        if args.action == "probe-build":
            compile_image({"ONCHIP_BOT_HOME_OPERATIONS": 0}, ROOT / ".tmp/onchip-https-probe")
            print("Built disabled/unconfigured metrics probe through sealed-header path; no hardware access")
            return 0
        build()
        return 0
    except (OSError, ValueError, UnicodeError, subprocess.CalledProcessError):
        parser.exit(2, "HTTPS operator preparation failed; private prerequisites/log require inspection; values withheld\n")


if __name__ == "__main__":
    raise SystemExit(main())
