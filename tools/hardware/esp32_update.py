#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and install the field application without erasing identities or stored data."""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import http.client
import json
import os
import re
import secrets
import socket
import ssl
import subprocess
import time
import urllib.parse

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware import esp32_device as beta_lab
from tools.hardware.esp32_slot import live_app_slot
from firmware.esp32 import https_profile as hardware
from tools.hardware import mast_checks as field
from tools.hardware.admin import checked, private_file
from tools.hardware.rf import public_hex
from tools.hardware.wifi_checks import environment_file, lan_credentials

from tools.hardware.inventory import value as inventory_value

DIRECTORY = field.ROOT / ".tmp/onchip-owner-release-20260928"
BUILD = field.ROOT / ".tmp/onchip-owner-release-firmware"
SERVICE = field.ROOT / ".tmp/onchip-field-service"
def service_address():
    return inventory_value("service_address")
def service_hostname():
    return inventory_value("service_hostname")
def telemetry_policy():
    return inventory_value("telemetry_policy")


def save(name, value):
    data = value if isinstance(value, bytes) else (json.dumps(value, indent=2) + "\n").encode()
    beta_lab.write_private(DIRECTORY / name, data)


def run_make(directory, *arguments, log=None):
    command = ["make", "-s", "-C", str(directory), *map(str, arguments)]
    if log:
        with log.open("x") as output:
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(command, check=True)


def service():
    hardware.private_directory(SERVICE)
    required = ("ca.crt", "server.crt", "server.key", "token", "service.env")
    present = [(SERVICE / name).exists() for name in required]
    if any(present) and not all(present):
        raise ValueError("Incomplete service credentials; refusing rotation of a deployed trust root")
    if not any(present):
        now = datetime.now(timezone.utc)
        ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        server_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        ca_name = x509.Name([x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, "MeshCore field CA")])
        ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
              .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
              .not_valid_before(now - timedelta(minutes=5)).not_valid_after(now + timedelta(days=365))
              .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
              .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
              .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False),
                             critical=True).sign(ca_key, hashes.SHA256()))
        server = (x509.CertificateBuilder()
                  .subject_name(x509.Name([x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, service_hostname())]))
                  .issuer_name(ca_name).public_key(server_key.public_key())
                  .serial_number(x509.random_serial_number())
                  .not_valid_before(now - timedelta(minutes=5)).not_valid_after(now + timedelta(days=365))
                  .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                  .add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False),
                                 critical=True)
                  .add_extension(x509.SubjectKeyIdentifier.from_public_key(server_key.public_key()),
                                 critical=False)
                  .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
                                 critical=False)
                  .add_extension(x509.SubjectAlternativeName([x509.DNSName(service_hostname())]), critical=False)
                  .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),
                                 critical=False).sign(ca_key, hashes.SHA256()))
        token = secrets.token_hex(32)
        for name, data in {
                "ca.crt": ca.public_bytes(serialization.Encoding.PEM),
                "server.crt": server.public_bytes(serialization.Encoding.PEM),
                "server.key": server_key.private_bytes(serialization.Encoding.PEM,
                                                       serialization.PrivateFormat.PKCS8,
                                                       serialization.NoEncryption()),
                "token": token.encode(),
                "service.env": ("MESHCORE_BOT_SERVICE_TOKEN=" + token + "\n").encode()}.items():
            beta_lab.write_private(SERVICE / name, data)
    run_make(field.ROOT / "cmd/meshcore-bot-service", "service-install",
             "SERVICE_DIRECTORY=" + str(SERVICE), "LISTEN=" + service_address() + ":8787")
    for attempt in range(20):
        try:
            with socket.create_connection((service_address(), 8787), timeout=1):
                break
        except ConnectionRefusedError:
            if attempt == 19:
                raise
            time.sleep(.25)
    service_check()


def service_request(operation, arguments):
    context = ssl.create_default_context(cafile=SERVICE / "ca.crt")
    token = private_file(SERVICE / "token", 257).decode()
    body = json.dumps({"operation": operation, "request_id": secrets.token_hex(8), "args": arguments}).encode()
    with socket.create_connection((service_address(), 8787), timeout=8) as raw:
        with context.wrap_socket(raw, server_hostname=service_hostname()) as tls:
            request = (f"POST /v1/rpc HTTP/1.1\r\nHost: {service_hostname()}:8787\r\n"
                       f"Authorization: Bearer {token}\r\nContent-Type: application/json\r\n"
                       f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode() + body
            tls.sendall(request)
            response = http.client.HTTPResponse(tls)
            response.begin()
            result = json.loads(response.read(4097))
            if response.status != 200 or result.get("ok") is not True:
                raise ValueError("HTTPS " + operation + " failed: " + str(result.get("error", {}).get("code")))
            return result["result"]


def service_check():
    if service_request("health", {}) != {"status": "ok"}:
        raise ValueError("HTTPS health response mismatch")
    nonce = secrets.token_hex(8)
    if service_request("echo", {"text": nonce}) != {"text": nonce}:
        raise ValueError("HTTPS echo response mismatch")
    weather = service_request("weather", {"place": "Calgary"})
    if weather.get("source") != "open-meteo" or not 0 <= weather.get("source_age_seconds", -1) <= 7200:
        raise ValueError("HTTPS weather freshness/source mismatch")
    print("PASS durable TLS/SNI/auth service health, unique echo and live Open-Meteo weather", flush=True)


def current():
    admin = field.admin_status()
    field.require_phy(admin, field.active_profile())
    if ("roles applied=7 saved=7" not in admin["status"] or
            "connected=1" not in admin["wifi status"] or
            "repeater=3 room=3 companion=3 management=3" not in admin["role-path"]):
        raise ValueError("Field roles, WiFi or path policy differs")
    state = field.dashboard()
    roles = {role["role"]: role for role in state["roles"]}
    for name in ("repeater", "room", "companion", "management", "command-bot"):
        if not roles.get(name, {}).get("ready"):
            raise ValueError("Field role not ready: " + name)
    return {"admin": admin, "dashboard": state,
            "keys": {name: role["public_key"] for name, role in roles.items()},
            "at": datetime.now(timezone.utc).isoformat(), "mac": field.device_mac()}


def export():
    before = current()
    run_make(field.ROOT / "firmware/esp32", "mast-cli",
             "ARGS=--web http://" + field.mast_host() + " --password-file " + str(field.LAB / "password") +
             " download " + str(DIRECTORY / "source-before.lua"))
    source = private_file(DIRECTORY / "source-before.lua", 4096)
    if hashlib.sha256(source).hexdigest() != before["admin"]["source hash"].split()[1]:
        raise ValueError("Exported source differs from authenticated preflight")
    save("before.json", before)
    print("Saved authenticated current source and identity/configuration readback; no reset", flush=True)


def diagnostic_fixture(cpp):
    start, end = b'const char BotDefaultSource[] = R"lua(', b')lua";'
    if cpp.count(start) != 1 or end not in cpp.split(start, 1)[1]:
        raise ValueError("Historical bundled source declaration is unavailable")
    return cpp.split(start, 1)[1].split(end, 1)[0]


def bundled():
    original = private_file(DIRECTORY / "source-before.lua", 4096)
    historical = subprocess.check_output(
        ["git", "show", "86c3c74:firmware/onchip/BotTypes.cpp"])
    if original != diagnostic_fixture(historical):
        raise ValueError("Archived source differs from the historical bundled diagnostics; preserve user logic")
    client = field.connect()
    try:
        active = checked(client, "source hash").split()[1]
        authorized = {hashlib.sha256(original).hexdigest(),
                      "17fe695546af5189de7e046d20cc133ba9e098226700e5fc1dce6cd532516d4d"}
        if active not in authorized:
            raise ValueError("Active source is neither archived fixture nor installed example")
        print(checked(client, "source remove"), flush=True)
        for _ in range(20):
            time.sleep(.5)
            status = checked(client, "source status")
            print(status, flush=True)
            if "Error:" in status:
                raise ValueError(status)
            if "durably saved and active" in status or "bot disabled" in status:
                expected_source = checked(client, "source hash").split()[1]
                save("bundled.json", {"sha256": expected_source})
                if "bot disabled" in status:
                    checked(client, "bot on")
                    checked(client, "reboot")
                    time.sleep(12)
                break
        else:
            raise TimeoutError("Bundled activation incomplete")
    finally:
        client.close()
    verify(expected_source=expected_source)
    print("Current bundled useful command suite active; no demo handlers installed", flush=True)


def build():
    if environment_file().resolve() != field.ROOT / ".env.dev.local":
        raise ValueError("This field release requires the independently authorized .env.dev.local")
    native = hardware.configuration({
        "ONCHIP_BOT_HOME_ADDRESS": service_address(), "ONCHIP_BOT_HOME_HOST": service_hostname(),
        "ONCHIP_BOT_HOME_PORT": "8787", "ONCHIP_BOT_HOME_OPERATIONS": "7",
        "ONCHIP_BOT_HOME_CA_FILE": str(SERVICE / "ca.crt"),
        "ONCHIP_BOT_HOME_TOKEN_FILE": str(SERVICE / "token")})
    ssid, password = lan_credentials()
    frequency, bandwidth, sf, cr, tx_power = field.active_profile()
    native.update(WIFI_SSID=ssid.decode(), WIFI_PWD=password.decode(),
                  ONCHIP_RADIO_FREQ_MHZ=frequency / 1_000_000,
                  ONCHIP_RADIO_BW_KHZ=bandwidth / 1_000,
                  ONCHIP_RADIO_SF=sf, ONCHIP_RADIO_CR=cr, ONCHIP_RADIO_TX_POWER=tx_power,
                  ONCHIP_ADMIN_PASSWORD=private_file(field.LAB / "role-password", 15).decode(),
                  ONCHIP_MAST_PASSWORD=private_file(field.LAB / "password", 15).decode(),
                  ONCHIP_TRUSTED_COMPANION_PUBKEY=public_hex(private_file(field.LAB / "companion.seed", 32)),
                  ONCHIP_OPERATOR_PUBKEY=public_hex(private_file(field.LAB / "operator.seed", 32)))
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    target = hardware.compile_image(native, BUILD)
    image = (target / "firmware.bin").read_bytes()
    if subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip() != revision:
        raise ValueError("Source revision changed while building; repeat the configured build before flashing")
    label = "image-" + str(time.time_ns())
    save(label + ".bin", image)
    save(label + "-partitions.bin", (target / "partitions.bin").read_bytes())
    save(label + ".json", {"sha256": hashlib.sha256(image).hexdigest(), "bytes": len(image),
                           "revision": revision, "image": label + ".bin",
                           "partitions": label + "-partitions.bin",
                           "bundled_sha256": hashlib.sha256(diagnostic_fixture(
                               (field.ROOT / "firmware/runtime/BotTypes.cpp").read_bytes())).hexdigest()})
    print(f"Built private HTTPS7 field image with {frequency / 1_000_000:g} MHz/"
          f"BW{bandwidth / 1_000:g}/SF{sf}/CR{cr} compiled PHY; no flash", flush=True)


def flash_image(image, label):
    if not 0 < image.stat().st_size <= 0x330000:
        raise ValueError("Application image does not fit the field app partition")
    beta_lab.check()
    address, capacity = live_app_slot(beta_lab.ESPTOOL, field.device_port(), DIRECTORY)
    if image.stat().st_size > capacity:
        raise ValueError("Application image exceeds selected OTA partition")
    with (DIRECTORY / (label + "-" + str(time.time_ns()) + ".log")).open("x") as log:
        subprocess.run([sys.executable, str(beta_lab.ESPTOOL), "--chip", "esp32s3",
                        "--port", str(field.device_port()), "--baud", "921600", "--after", "hard_reset",
                        "write_flash", hex(address), str(image)],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    time.sleep(12)


def verify(before=None, expected_source=None):
    if before is None:
        deployments = sorted(DIRECTORY.glob("deployment-*.json"))
        if deployments:
            deployment = json.loads(private_file(deployments[-1], 65536))
            before = deployment["before"]
            if expected_source is None:
                expected_source = deployment["expected_source"]
        else:
            before = json.loads(private_file(DIRECTORY / "before.json", 65536))
    after = current()
    if after["keys"] != before["keys"]:
        raise ValueError("Role identities changed during app update")
    if expected_source is None:
        expected_source = before["admin"]["source hash"].split()[1]
        if (DIRECTORY / "custom-source.json").exists():
            expected_source = json.loads(private_file(DIRECTORY / "custom-source.json", 4096))["sha256"]
        if (DIRECTORY / "bundled.json").exists():
            expected_source = json.loads(private_file(DIRECTORY / "bundled.json", 4096))["sha256"]
    if after["admin"]["source hash"].split()[1] != expected_source:
        raise ValueError("Preserved field source hash changed")
    for key in ("bot policy", "room access"):
        if after["admin"][key] != before["admin"][key]:
            raise ValueError("Preserved field configuration changed: " + key)
    with field.Companion() as companion:
        if companion.width != 3 or companion.public_key != after["keys"]["companion"]:
            raise ValueError("Companion protocol/path/identity mismatch")
    save("verified-" + str(time.time_ns()) + ".json", after)
    print("PASS field PHY/WiFi, role identities, command source, policy and protocol13; one TCP slot used", flush=True)


def flash():
    manifests = sorted(DIRECTORY.glob("image-*.json"))
    if not manifests:
        raise ValueError("No preserved stable field candidate")
    candidate = json.loads(private_file(manifests[-1], 4096))
    image = DIRECTORY / candidate["image"]
    partitions = DIRECTORY / candidate["partitions"]
    if image.parent != DIRECTORY or partitions.parent != DIRECTORY:
        raise ValueError("Candidate files must remain in the private release directory")
    if hashlib.sha256(private_file(image, 0x330000)).hexdigest() != candidate["sha256"]:
        raise ValueError("Preserved candidate image hash mismatch")
    before = current()
    client = field.connect()
    try:
        source_status = checked(client, "source status")
    finally:
        client.close()
    expected_source = before["admin"]["source hash"].split()[1]
    if source_status.endswith("; bundled source"):
        expected_source = candidate.get("bundled_sha256")
        if not expected_source:
            raise ValueError("Rebuild the candidate to record its bundled source hash")
    save("deployment-" + str(time.time_ns()) + ".json",
         {"before": before, "expected_source": expected_source, "candidate": candidate})
    flash_image(image, "flash")
    verify(before, expected_source)


def grants():
    client = field.connect()
    try:
        for command in ("bot home on", "bot shared on", "bot reminders on",
                        "bot home", "bot shared", "bot reminders"):
            print(checked(client, command), flush=True)
    finally:
        client.close()


def telemetry():
    """Explicit Aspen-only provisioning; never run as part of a build/flash."""
    policy = telemetry_policy()
    if field.mast_host() != policy["host"]:
        raise ValueError("Telemetry deployment is authorized only for the configured mast host")
    mast_key = os.environ.get("TELEMETRY_MAST_KEY", "")
    if not re.fullmatch("[0-9a-fA-F]{64}", mast_key):
        raise ValueError("Set TELEMETRY_MAST_KEY to Aspen's verified current management public key")
    ca_file = os.environ.get("TELEMETRY_CA_FILE")
    if not ca_file:
        raise ValueError("Set TELEMETRY_CA_FILE to the verified receiver CA PEM bundle")
    ca = Path(ca_file).read_bytes()
    if not 0 < len(ca) <= 4096 or b"-----BEGIN CERTIFICATE-----" not in ca:
        raise ValueError("Telemetry CA must be a PEM bundle of at most 4096 bytes")
    url = urllib.parse.urlsplit(policy["endpoint"])
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("Telemetry deployment requires an HTTPS endpoint without URL credentials")
    path = url.path or "/"
    if url.query:
        path += "?" + url.query
    port = url.port or 443
    address = socket.gethostbyname(url.hostname)
    context = ssl.create_default_context(cadata=ca.decode("ascii"))
    # Check this exact address, hostname and operator CA before changing a device.
    with socket.create_connection((address, port), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=url.hostname):
            pass
    token = b""
    token_file = os.environ.get("TELEMETRY_TOKEN_FILE")
    if token_file:
        token = private_file(Path(token_file), 257)
        if not 0 < len(token) <= 256 or any(byte < 33 or byte > 126 for byte in token):
            raise ValueError("Telemetry bearer token must be 1..256 printable non-space bytes")
    commands = [
        "telemetry off", "telemetry endpoint discard",
        f"telemetry endpoint address {address}",
        f"telemetry endpoint host {url.hostname}",
        f"telemetry endpoint port {port}",
        f"telemetry endpoint path {path}",
        "telemetry endpoint ca clear", "telemetry endpoint token clear",
    ]
    for name, data in (("ca", ca), ("token", token)):
        for offset in range(0, len(data), 56):
            commands.append(f"telemetry endpoint {name} " + data[offset:offset + 56].hex())
    commands.extend(["telemetry endpoint commit",
                     f"telemetry interval {policy['interval_seconds']}",
                     "telemetry on" if policy["enabled"] else "telemetry off"])
    if any(len(command) > 145 for command in commands):
        raise ValueError("Telemetry configuration exceeds native nonce-tagged command capacity")
    client = field.NativeClient(
        os.environ.get("TELEMETRY_RF_GATEWAY", field.gateway_host()), 8001,
        Path(os.environ.get("TELEMETRY_SEED_FILE", field.LAB / "companion.seed")), mast_key,
        private_file(Path(os.environ.get("TELEMETRY_PASSWORD_FILE", field.LAB / "password")), 15).decode("ascii"))
    try:
        if checked(client, "telemetry identity") != policy["device"]:
            raise ValueError("Authenticated device hardware identity is not Aspen; no changes made")
        for command in commands:
            checked(client, command)
        endpoint = checked(client, "telemetry endpoint status")
        status = checked(client, "telemetry status")
        if "configured=1" not in endpoint or f"on={int(policy['enabled'])}" not in status:
            raise ValueError("Telemetry configuration readback differs")
        if checked(client, "telemetry endpoint host") != url.hostname or \
                checked(client, "telemetry endpoint path") != path:
            raise ValueError("Telemetry receiver readback differs")
        print("Aspen telemetry runtime policy saved: " + status, flush=True)
        print("Check telemetry counts/times after one interval for physical-device delivery", flush=True)
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("service", "service-check", "export", "bundled",
                                         "build", "flash", "verify", "grants", "telemetry"))
    args = parser.parse_args()
    os.umask(0o077)
    hardware.private_directory(DIRECTORY)
    {"service": service, "service-check": service_check, "export": export, "bundled": bundled,
     "build": build, "flash": flash, "verify": verify, "grants": grants,
     "telemetry": telemetry}[args.action]()


if __name__ == "__main__":
    main()
