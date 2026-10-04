#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check HTTPS on the selected companion radio with application rollback."""
import argparse
from contextlib import ExitStack
from datetime import datetime, timedelta, timezone
import hashlib
import http.client
import json
import os
import secrets
import signal
import socket
import ssl
import subprocess
import threading
import time
import urllib.request

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import ESPTOOL, write_private
from tools.hardware.admin import WebClient, checked, private_file
from tools.hardware.mast_checks import Companion
from tools.hardware.rf import public_hex
from tools.hardware.wifi_checks import environment_file, lan_credentials
from firmware.esp32 import https_profile as preparation
from tools.hardware import companion_device as stock

from tools.hardware.inventory import value as inventory_value

ROOT = preparation.ROOT
DIRECTORY = ROOT / ".tmp/onchip-https-peer"
BUILD = ROOT / ".tmp/onchip-https-peer-firmware"
SERVICE = ROOT / ".tmp/onchip-home-service"
def peer_host():
    return inventory_value("gateway_host")
DONE = "HTTPS PROBE PASS: native certificate/time and reference service complete; Lua grant unchanged"


def save(name, value):
    data = value if isinstance(value, bytes) else (json.dumps(value, indent=2) + "\n").encode()
    path = DIRECTORY / name
    temporary = path.with_suffix(path.suffix + ".new")
    flags = os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW
    with os.fdopen(os.open(temporary, flags, 0o600), "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def dashboard():
    with urllib.request.urlopen(f"http://{peer_host()}/api/status", timeout=5) as response:
        return json.load(response)


def preflight():
    preparation.private_directory(DIRECTORY)
    backup = stock.verified_backup()
    raw = backup.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    original = preparation.backup_app(raw, digest)
    current = dashboard()
    if (current["profile"]["frequency_hz"] != 912525000 or
            current["kiss"]["connected"] or current["scheduler"]["queued"] or
            current["scheduler"]["transmitting"]):
        raise ValueError("Spare must be idle on original 912.525 lab PHY")
    if not stock.device_port().is_symlink() or stock.device_port().resolve().name != "ttyACM2":
        raise ValueError("Configured companion USB mapping changed; serial access refused")
    users = subprocess.run(["fuser", str(stock.device_port().resolve())], capture_output=True, text=True)
    if users.returncode not in (0, 1) or users.stdout.strip():
        raise ValueError("Spare serial port has an owner or cannot be checked")
    if not (DIRECTORY / "before.json").exists():
        save("before.json", current)
    save("rollback-app.bin", original)
    save("backup.json", {"mac": stock.device_mac(), "full_sha256": digest, "bytes": len(raw),
                         "app_sha256": hashlib.sha256(original).hexdigest()})
    print("Verified idle companion radio and private 8 MiB application rollback", flush=True)


def build():
    preflight()
    for name, size in (("operator.seed", 32), ("companion.seed", 32)):
        path = DIRECTORY / name
        if not path.exists():
            write_private(path, secrets.token_bytes(size))
    for name in ("password", "role-password"):
        path = DIRECTORY / name
        if not path.exists():
            write_private(path, secrets.token_urlsafe(10).encode())
    values = preparation.settings(environment_file())
    values.update({name: os.environ[name] for name in preparation.KEYS if name in os.environ})
    native = preparation.configuration(values)
    if (native["ONCHIP_BOT_HOME_ADDRESS"] != inventory_value("service_address") or
            native["ONCHIP_BOT_HOME_HOST"] != inventory_value("service_hostname") or
            native["ONCHIP_BOT_HOME_PORT"] != 8787 or native["ONCHIP_BOT_HOME_OPERATIONS"] != 3):
        raise ValueError("Only the authorized health/echo reference service is allowed")
    ssid, password = lan_credentials()
    native.update(WIFI_SSID=ssid.decode(), WIFI_PWD=password.decode(),
                  KISS_HOSTNAME="meshcore-https-peer",
                  ONCHIP_COMMAND_BOT_NAME="HTTPS peer bot",
                  ONCHIP_MANAGEMENT_NAME="HTTPS peer management",
                  ONCHIP_ADMIN_PASSWORD=private_file(DIRECTORY / "role-password", 15).decode(),
                  ONCHIP_MAST_PASSWORD=private_file(DIRECTORY / "password", 15).decode(),
                  ONCHIP_TRUSTED_COMPANION_PUBKEY=public_hex(private_file(DIRECTORY / "companion.seed", 32)),
                  ONCHIP_OPERATOR_PUBKEY=public_hex(private_file(DIRECTORY / "operator.seed", 32)),
                  ONCHIP_BOT_HTTPS_SELF_TEST=1)
    target = preparation.compile_image(native, BUILD)
    partition = (target / "partitions.bin").read_bytes()
    raw = stock.verified_backup().read_bytes()
    if raw[0x8000:0x8000 + len(partition)] != partition:
        raise ValueError("Peer partition mismatch; app-only flash forbidden")
    image = (target / "firmware.bin").read_bytes()
    if not 0 < len(image) <= 0x330000:
        raise ValueError("Peer application exceeds original partition")
    save("candidate.json", {"mac": stock.device_mac(), "sha256": hashlib.sha256(image).hexdigest(),
                            "bytes": len(image), "partition_sha256": hashlib.sha256(partition).hexdigest()})
    print("Built private peer-only TLS candidate with separate administration and no flash", flush=True)


def fixtures():
    preparation.private_directory(DIRECTORY)
    ca = x509.load_pem_x509_certificate(private_file(SERVICE / "ca.crt", 4096))
    ca_key = serialization.load_pem_private_key(private_file(SERVICE / "ca.key", 16384), None)
    key_path = SERVICE / "server.key"
    key = serialization.load_pem_private_key(private_file(key_path, 16384), None)
    now = datetime.now(timezone.utc)
    names = ("expired-leaf", "future-leaf", "expired-intermediate", "future-intermediate")
    contexts = []
    for index, name in enumerate(names):
        start, end = ((now - timedelta(days=2), now - timedelta(days=1)) if index % 2 == 0 else
                      (now + timedelta(days=1), now + timedelta(days=2)))
        issuer, signer, intermediate = ca, ca_key, None
        if index >= 2:
            signer = rsa.generate_private_key(public_exponent=65537, key_size=2048)
            intermediate = (x509.CertificateBuilder()
                .subject_name(x509.Name([x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, name)]))
                .issuer_name(ca.subject).public_key(signer.public_key())
                .serial_number(x509.random_serial_number()).not_valid_before(start).not_valid_after(end)
                .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
                .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
                .add_extension(x509.SubjectKeyIdentifier.from_public_key(signer.public_key()), critical=False)
                .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca.public_key()), critical=False)
                .sign(ca_key, hashes.SHA256()))
            issuer = intermediate
            start, end = now - timedelta(minutes=5), now + timedelta(hours=4)
        leaf = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, inventory_value("service_hostname"))]))
            .issuer_name(issuer.subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(start).not_valid_after(end)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(True, False, isinstance(key, rsa.RSAPrivateKey),
                                        False, False, False, False, False, False), critical=True)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer.public_key()), critical=False)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(inventory_value("service_hostname"))]), critical=False)
            .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .sign(signer, hashes.SHA256()))
        chain = leaf.public_bytes(serialization.Encoding.PEM)
        if intermediate:
            chain += intermediate.public_bytes(serialization.Encoding.PEM)
        save(name + ".crt", chain)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(DIRECTORY / (name + ".crt"), key_path)
        contexts.append(context)
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    counts = [{"port": 8788 + i, "case": names[i], "handshakes": 0, "application_bytes": 0,
               "esp_handshakes": 0, "esp_application_bytes": 0,
               "handshake_errors": 0} for i in range(4)]
    lock = threading.Lock()
    sockets = []
    threads = []

    def serve(listener, context, row):
        while not stopped.is_set():
            try:
                connection, address = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(4)
                try:
                    time.sleep(.35)
                    with context.wrap_socket(connection, server_side=True) as tls:
                        with lock:
                            row["handshakes"] += 1
                            if address[0] == peer_host():
                                row["esp_handshakes"] += 1
                        data = tls.recv(1)
                        with lock:
                            row["application_bytes"] += len(data)
                            if address[0] == peer_host():
                                row["esp_application_bytes"] += len(data)
                except (OSError, ssl.SSLError):
                    with lock:
                        row["handshake_errors"] += 1
            with lock:
                save("fixture-counts.json", counts)

    try:
        for context, row in zip(contexts, counts):
            listener = socket.socket()
            sockets.append(listener)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((inventory_value("service_address"), row["port"]))
            listener.listen(2)
            listener.settimeout(.5)
            thread = threading.Thread(target=serve, args=(listener, context, row))
            threads.append(thread)
            thread.start()
        save("fixture-counts.json", counts)
        print("READY four private expired/future-chain TLS fixtures on wired8788..8791; reference8787 unchanged", flush=True)
        stopped.wait()
    finally:
        stopped.set()
        for thread in threads:
            thread.join(timeout=6)
        for listener in sockets:
            listener.close()


def check_fixtures():
    context = ssl.create_default_context(cafile=SERVICE / "ca.crt")
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    token = private_file(SERVICE / "token", 257).decode("ascii").removesuffix("\n")
    body = b'{"operation":"health","request_id":"peer-preflight","args":{}}'
    with socket.create_connection((inventory_value("service_address"), 8787), timeout=5) as raw:
        with context.wrap_socket(raw, server_hostname=inventory_value("service_hostname")) as tls:
            request = (f"POST /v1/rpc HTTP/1.1\r\nHost: {inventory_value('service_hostname')}:8787\r\n"
                       f"Authorization: Bearer {token}\r\nContent-Type: application/json\r\n"
                       f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode() + body
            tls.sendall(request)
            response = bytearray()
            while len(response) <= 4096:
                part = tls.recv(4096)
                if not part:
                    break
                response.extend(part)
            if len(response) > 4096 or not response.startswith(b"HTTP/1.1 200 "):
                raise ValueError("Reference TLS1.2 authenticated health preflight failed")
            value = json.loads(response.split(b"\r\n\r\n", 1)[1])
            if value != {"ok": True, "result": {"status": "ok"}}:
                raise ValueError("Reference health response shape differs")
    for port in range(8788, 8792):
        with socket.create_connection((inventory_value("service_address"), port), timeout=5) as raw:
            try:
                with context.wrap_socket(raw, server_hostname=inventory_value("service_hostname")):
                    raise ValueError("Negative certificate fixture unexpectedly verified")
            except ssl.SSLCertVerificationError as error:
                if error.verify_code != (10 if port % 2 == 0 else 9):
                    print(f"Negative fixture port {port}: unexpected certificate verify code {error.verify_code}", flush=True)
                    raise ValueError("Negative fixture failed for a non-date reason") from None
    print("PASS host-only TLS1.2 reference health and four date-specific negative fixtures; not ESP acceptance", flush=True)


def wait_dashboard():
    until = time.monotonic() + 120
    observed = None
    incomplete = 0
    while time.monotonic() < until:
        try:
            state = dashboard()
        except http.client.IncompleteRead:
            incomplete += 1
            print("Peer boot dashboard response incomplete; retrying within readiness deadline", flush=True)
            time.sleep(1)
            continue
        except OSError:
            time.sleep(1)
            continue
        if not is_candidate(state):
            observed = {"name": state.get("device_name"), "capacity": state.get("kiss", {}).get("capacity")}
            time.sleep(1)
            continue
        save("readiness.json", {"incomplete_responses": incomplete, "ready": True})
        return state
    save("readiness-failed.json", {"dashboard_seen": observed, "incomplete_responses": incomplete})
    raise TimeoutError("Expected private HTTPS peer did not become ready")


def is_candidate(state):
    return (state.get("kiss", {}).get("capacity") == 3 and
            any(role.get("role") == "command-bot" and role.get("name") == "HTTPS peer bot"
                for role in state.get("roles", [])))


def archive_attempt():
    names = ("attempt.json", "serial.log", "flash.log", "readiness.json", "readiness-failed.json",
             "physical-tls.json", "physical-result.json", "restored.json")
    previous = [DIRECTORY / name for name in names if (DIRECTORY / name).exists()]
    if previous:
        archive = DIRECTORY / ("previous-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
        archive.mkdir(mode=0o700)
        for path in previous:
            path.rename(archive / path.name)


def record_restore():
    save("restored.json", {"verified": True, "mac": stock.device_mac(),
                           "verified_at": datetime.now(timezone.utc).isoformat()})


def verify_restore():
    stock.verify()
    record_restore()


def report():
    log = private_file(DIRECTORY / "serial.log", 2 * 1024 * 1024).decode(errors="replace")
    lines = [line for line in log.splitlines() if line.startswith(
        ("HTTPS PROBE", "HTTPS samples:", "HTTPS internal:", "HTTPS DMA:", "HTTPS phase ", "HTTPS buffers:"))]
    restored = json.loads(private_file(DIRECTORY / "restored.json", 4096))
    if (DIRECTORY / "restored.json").stat().st_mtime < (DIRECTORY / "serial.log").stat().st_mtime:
        raise ValueError("Restore evidence predates this serial capture; rerun verify-restore")
    evidence = {
        "mac": stock.device_mac(), "field_changed": False, "native_suite_complete": DONE in log,
        "lua_rf_reply": False, "lua_grant_enabled": False, "weather_enabled": False,
        "mqtt_configured": False, "metrics": lines,
        "restored": restored,
        "fixture_counts": json.loads(private_file(DIRECTORY / "fixture-counts.json", 8192)),
        "candidate": json.loads(private_file(DIRECTORY / "candidate.json", 4096)),
    }
    save("physical-result.json", evidence)
    print(json.dumps(evidence, indent=2))


def exercise():
    import serial
    preflight()
    candidate = json.loads(private_file(DIRECTORY / "candidate.json", 4096))
    target = BUILD / ".pio/build" / preparation.ENV
    firmware = target / "firmware.bin"
    if candidate["mac"] != stock.device_mac() or hashlib.sha256(firmware.read_bytes()).hexdigest() != candidate["sha256"]:
        raise ValueError("Peer candidate identity/digest mismatch")
    archive_attempt()
    save("attempt.json", {"started_at": datetime.now(timezone.utc).isoformat(), "candidate": candidate})
    save("restored.json", {"verified": False, "mac": stock.device_mac()})
    stock.check()
    flashed = False
    stop = threading.Event()
    reader = None
    try:
        flashed = True
        with (DIRECTORY / "flash.log").open("w") as log:
            subprocess.run([sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", str(stock.device_port()),
                            "--baud", "921600", "--after", "hard_reset", "write_flash",
                            "0x10000", str(firmware)], stdout=log, stderr=subprocess.STDOUT, check=True)
        port = serial.Serial()
        port.port, port.baudrate, port.timeout, port.exclusive = str(stock.device_port()), 115200, .2, True
        port.dtr = port.rts = False
        port.open()
        errors = []

        def capture():
            total = 0
            try:
                with port, (DIRECTORY / "serial.log").open("wb") as output:
                    while not stop.is_set():
                        data = port.read(4096)
                        total += len(data)
                        if total > 2 * 1024 * 1024:
                            raise ValueError("Peer serial capture exceeded bound")
                        output.write(data)
                        output.flush()
            except (OSError, ValueError) as error:
                errors.append(type(error).__name__)

        reader = threading.Thread(target=capture)
        reader.start()
        initial = wait_dashboard()
        initial_keys = {role["role"]: role["public_key"] for role in initial["roles"] if role.get("public_key")}
        client = WebClient(f"http://{peer_host()}", private_file(DIRECTORY / "password", 15).decode())
        try:
            checked(client, "bot home off")
            checked(client, "roles 7")
            checked(client, "reboot")
        finally:
            client.close()
        time.sleep(6)
        rebooted = wait_dashboard()
        if any(next((role.get("public_key") for role in rebooted["roles"] if role["role"] == name), None) != key
               for name, key in initial_keys.items() if name != "observer"):
            raise ValueError("Peer role identities changed across scoped configuration reboot")
        with ExitStack() as clients:
            clients.enter_context(Companion(peer_host()))
            clients.enter_context(Companion(peer_host()))
            deadline = time.monotonic() + 140
            while time.monotonic() < deadline:
                log = private_file(DIRECTORY / "serial.log", 2 * 1024 * 1024).decode(errors="replace")
                if "HTTPS PROBE COMPLETE: certificate suite failed" in log:
                    raise ValueError("Native probe failed; inspect private serial log")
                if DONE in log:
                    break
                if errors:
                    raise ValueError("Peer serial capture failed")
                time.sleep(.5)
            else:
                raise TimeoutError("Native probe did not complete")
            state = dashboard()
            roles = {role["role"]: role for role in state["roles"]}
            if any(not roles[name]["ready"] for name in ("repeater", "room", "companion", "management", "command-bot")):
                raise ValueError("Peer roles are not ready")
            client = WebClient(f"http://{peer_host()}", private_file(DIRECTORY / "password", 15).decode())
            try:
                status = checked(client, "status")
                grant = checked(client, "bot home")
                if "roles applied=7 saved=7" not in status or "saved=0 applied=0" not in grant:
                    raise ValueError("Peer role/grant preconditions changed")
            finally:
                client.close()
            lines = [line for line in log.splitlines() if line.startswith((
                "HTTPS PROBE", "HTTPS samples:", "HTTPS internal:", "HTTPS DMA:", "HTTPS phase ", "HTTPS buffers:"))]
            save("physical-tls.json", {"native_tls": True, "lua_rf": False, "field_changed": False,
                                     "role_status": status, "grant": grant, "metrics": lines,
                                     "role_keys": {name: role.get("public_key") for name, role in roles.items()}})
            for line in lines:
                print(line, flush=True)
    finally:
        stop.set()
        if reader:
            reader.join(timeout=4)
            if reader.is_alive():
                raise RuntimeError("Serial reader did not stop; restore blocked to avoid port conflict")
        if flashed:
            stock.restore()
            record_restore()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("preflight", "build", "fixtures", "check-fixtures", "exercise",
                                         "verify-restore", "report"))
    args = parser.parse_args()
    os.umask(0o077)
    try:
        {"preflight": preflight, "build": build, "fixtures": fixtures,
         "check-fixtures": check_fixtures, "exercise": exercise,
         "verify-restore": verify_restore, "report": report}[args.action]()
    except FileNotFoundError as error:
        parser.exit(2, f"Missing required private path or tool: {error.filename}\n")
    except ssl.SSLCertVerificationError as error:
        parser.exit(2, f"TLS fixture certificate verification failed ({error.verify_code}): {error.verify_message}\n")
    except TimeoutError:
        parser.exit(2, "Peer HTTPS timed out before expected readiness; inspect private serial log; peer restore attempted\n")
    except (OSError, ValueError, http.client.HTTPException, subprocess.CalledProcessError) as error:
        print(f"Peer failure type: {type(error).__name__}; private values withheld", file=sys.stderr)
        parser.exit(2, "Peer HTTPS operation failed; inspect private peer logs; secrets withheld\n")


if __name__ == "__main__":
    main()
