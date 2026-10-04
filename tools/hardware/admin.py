#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared bot/source CLI over encrypted RF, authenticated web or local owner Unix socket."""
import argparse
from collections import deque
import hashlib
import hmac
import os
import re
import secrets
import socket
import stat
import struct
import time
import urllib.request
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519
from nacl.bindings import crypto_scalarmult_ed25519_base_noclamp
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "firmware/runtime"))
import bot_packages
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from tools.hardware.rf import _shared_secret, kiss, target_key

SOURCE_LIMIT = 4096
CHUNK = 48
DATA_LIMIT = 2422
DATA_MAGIC = {"kv": b"BKD\x01", "timers": b"BTD\x01", "reminders": b"BRD\x01"}
IDENTITY_ROLES = ("repeater", "room", "companion", "bot", "command-bot", "management")
PACKAGE_FETCH_API = (
    "Owner GET JSON=2048; Package GET alias=package raw=1..4096 "
    "type=text/plain|application/octet-stream|application/x-lua SHA256=source"
)


def private_file(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError("Credential file must be a private regular file (0600)")
        value = stream.read(limit + 1)
    if len(value) > limit:
        raise ValueError("Credential file too large")
    return value


def check_native_identity(native, expected):
    if len(native) != 64 or native[0] & 7 or native[31] & 0xc0 != 0x40:
        raise ValueError("Identity file requires a raw 64-byte native expanded key, not a seed or seed/public pair")
    public = crypto_scalarmult_ed25519_base_noclamp(native[:32])
    if public[0] in (0, 255):
        raise ValueError("Native identity public prefix 00/ff is unsupported")
    if not hmac.compare_digest(public, target_key(expected)):
        raise ValueError("Identity file does not match the expected full public key; nothing sent")
    return public.hex()


def check_role_password(password):
    if not 1 <= len(password) <= 15 or any(byte < 32 or byte > 126 for byte in password):
        raise ValueError("Role administrator password requires 1..15 printable ASCII bytes; no trailing newline")


def read_file(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Input must be a regular file")
        value = stream.read(limit + 1)
    if len(value) > limit:
        raise ValueError("Input file too large")
    return value


def write_new_file(path, data, mode=0o644):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def encrypt(secret, plaintext):
    plaintext += bytes((-len(plaintext)) % 16)
    cipher = Cipher(algorithms.AES(secret[:16]), modes.ECB()).encryptor()
    ciphertext = cipher.update(plaintext) + cipher.finalize()
    return hmac.new(secret, ciphertext, hashlib.sha256).digest()[:2] + ciphertext


def decode(secret, sender, target, packet):
    if len(packet) < 6 or packet[0] >> 6:
        return None
    start = 5 if packet[0] & 3 in (0, 3) else 1
    kind, path = (packet[0] >> 2) & 15, packet[start]
    size = (path & 63) * ((path >> 6) + 1)
    if path >> 6 == 3 or size > 64 or kind not in (1, 2, 8):
        return None
    payload = packet[start + 1 + size:]
    if len(payload) < 20 or len(payload) > 184 or (len(payload) - 4) % 16:
        return None
    if payload[:2] != bytes((sender[0], target[0])):
        return None
    if not hmac.compare_digest(payload[2:4], hmac.new(secret, payload[4:], hashlib.sha256).digest()[:2]):
        return None
    cipher = Cipher(algorithms.AES(secret[:16]), modes.ECB()).decryptor()
    return kind, cipher.update(payload[4:]) + cipher.finalize()


def reciprocal_path(secret, sender, target, packet, learned):
    decoded = decode(secret, sender, target, packet)
    if decoded is None or decoded[0] != 8 or packet[0] & 3 not in (0, 1):
        raise ValueError("Expected an authenticated native flood PATH return")
    count = (learned[0] & 63) * ((learned[0] >> 6) + 1) if learned else -1
    if not learned or learned[0] >> 6 == 3 or count > 64 or len(learned) != count + 1:
        raise ValueError("Invalid learned native path")
    start = 5 if packet[0] & 3 == 0 else 1
    size = (packet[start] & 63) * ((packet[start] >> 6) + 1)
    # Pinned Mesh::onPeerPathRecv returns the received flood path directly
    # along the authenticated supplied path; empty extra uses FF + random[4].
    body = packet[start:start + size + 1] + b"\xff" + secrets.token_bytes(4)
    return b"\x22" + learned + bytes((target[0], sender[0])) + encrypt(secret, body)


class NativeClient:
    def __init__(self, gateway, port, seed_file, target, password, timeout=20, room=False, path=None,
                 tagged=True, retry_commands=True):
        if path is not None and (not path or path[0] >> 6 == 3 or not path[0] & 63 or
                                 len(path) != 1 + ((path[0] >> 6) + 1) * (path[0] & 63) or
                                 len(path) > 65):
            raise ValueError("Explicit native route requires a bounded nonempty encoded path")
        seed = private_file(seed_file, 32)
        if len(seed) != 32:
            raise ValueError("Companion seed must be 32 bytes")
        self.key = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
        self.public = self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.target = target_key(target)
        self.secret = _shared_secret(self.key, self.target)
        self.clock_file = Path(str(seed_file) + ".clock")
        self.timestamp = int(private_file(self.clock_file, 20)) if self.clock_file.exists() else 0
        self.path = path
        self.fixed_path = path
        self.tagged = tagged
        self.retry_commands = retry_commands
        self.received_cli = deque(maxlen=32)
        self.routed_responses = self.incomplete_routes = self.route_repairs = 0
        self.timeout = timeout
        self.connection = socket.create_connection((gateway, port), timeout=5)
        self.tag = secrets.randbits(64)
        try:
            self.exchange(password, login=True, room=room)
            if path is not None:
                self.path = path
                self.send_fixed_path()
                time.sleep(2.5)
        except (OSError, ValueError):
            self.connection.close()
            raise

    def close(self):
        self.connection.close()

    def send_fixed_path(self):
        if self.fixed_path is None:
            raise ValueError("No fixed native return route")
        # Match the native reciprocal-PATH delay; the relay may still be sending the login reply.
        time.sleep(.5)
        body = self.fixed_path + b"\xff" + secrets.token_bytes(4)
        packet = (b"\x22" + self.fixed_path + bytes((self.target[0], self.public[0])) +
                  encrypt(self.secret, body))
        self.connection.sendall(kiss(packet))

    def exchange(self, text, login=False, room=False):
        self.timestamp = max(self.timestamp + 1, int(time.time()))
        if not 0 < self.timestamp <= 0xffffffff:
            raise ValueError("Native timestamp exhausted")
        fd = os.open(self.clock_file, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(str(self.timestamp))
            stream.flush()
            os.fsync(stream.fileno())
        prefix = "" if login or not self.tagged else f"{self.tag:016x}|"
        self.tag = (self.tag + 1) % (1 << 64)
        wire_text = (prefix + text).encode("ascii")
        if len(wire_text) > (15 if login else 162):
            raise ValueError("Native text capacity exceeded")
        body = struct.pack("<I", self.timestamp)
        if login and room:
            body += struct.pack("<I", self.timestamp)  # Native room sync-since, not mast CLI framing.
        body += wire_text + b"\0" if login else b"\x04" + wire_text
        route = b"\x80" if self.path is None else self.path
        header = (7 if login else 2) << 2 | (1 if self.path is None else 2)
        identities = bytes((self.target[0],)) + self.public if login else bytes((self.target[0], self.public[0]))
        packet = bytes((header,)) + route + identities + encrypt(self.secret, body)
        if len(packet) > 255:
            raise ValueError("Native packet capacity exceeded")
        self.connection.sendall(kiss(packet))
        deadline = time.monotonic() + self.timeout
        attempts = 0
        retry_limit = 2 if getattr(self, "retry_commands", True) else 0
        next_retry = time.monotonic() + self.timeout / 3
        frame, escaped, valid = bytearray(), False, False
        total = 0
        route_repaired = False
        while time.monotonic() < deadline:
            wake = min(deadline, next_retry) if not login and attempts < retry_limit else deadline
            self.connection.settimeout(max(.01, wake - time.monotonic()))
            try:
                data = self.connection.recv(512)
            except socket.timeout:
                if login or attempts == retry_limit:
                    raise TimeoutError("No authenticated mast reply; outcome unknown") from None
                attempts += 1
                # Native attempt bits avoid relay packet dedup; timestamp/text
                # stay identical so mast retries return the cached outcome.
                retry_body = body[:4] + bytes((4 | attempts,)) + body[5:]
                retry_packet = bytes((header,)) + route + identities + encrypt(self.secret, retry_body)
                self.connection.sendall(kiss(retry_packet))
                next_retry = time.monotonic() + self.timeout / 3
                continue
            if not data:
                raise ValueError("Gateway disconnected; outcome unknown")
            total += len(data)
            if total > 8 * 1024 * 1024:
                raise ValueError("Gateway receive budget exceeded")
            for byte in data:
                if byte == 0xc0:
                    received = bytes(frame[1:])
                    result = decode(self.secret, self.public, self.target, received) if valid and not escaped and frame[:1] == b"\0" else None
                    frame, escaped, valid = bytearray(), False, True
                    if result is None:
                        continue
                    kind, plain = result
                    if self.fixed_path is not None and not login:
                        start = 5 if received[0] & 3 in (0, 3) else 1
                        if received[0] & 3 not in (2, 3) or received[start] & 63:
                            self.incomplete_routes += 1
                            if (not route_repaired and received[0] & 3 in (0, 1) and kind == 2 and
                                    len(plain) > 5 and plain[4] >> 2 == 1 and
                                    plain[5:].startswith(prefix.encode("ascii"))):
                                self.send_fixed_path()
                                self.route_repairs += 1
                                route_repaired = True
                            continue
                    if login:
                        if kind == 8:
                            path = plain[0]
                            n = (path & 63) * ((path >> 6) + 1)
                            if path >> 6 == 3 or n > 64 or len(plain) < n + 15 or plain[n + 1] != 1:
                                continue
                            self.path, plain = plain[:n + 1], plain[n + 2:]
                        if len(plain) >= 13 and plain[4] == 0 and plain[6:8] == b"\x01\x03":
                            if kind == 8 and received[0] & 3 in (0, 1):
                                response = reciprocal_path(self.secret, self.public, self.target, received, self.path)
                                time.sleep(.5)  # Same reciprocal-PATH delay as pinned Mesh.cpp.
                                self.connection.sendall(kiss(response))
                            return "Authenticated native administrator"
                    elif kind == 2 and len(plain) > 5 and plain[4] >> 2 == 1:
                        if struct.unpack_from("<I", plain)[0] == self.timestamp:
                            continue
                        response = plain[5:].split(b"\0", 1)[0].decode("ascii")
                        if response.startswith(prefix):
                            if not self.tagged:
                                # Native reply timestamps do not identify the request.
                                fingerprint = hashlib.sha256(plain).digest()
                                if fingerprint in self.received_cli:
                                    continue
                                self.received_cli.append(fingerprint)
                            if self.fixed_path is not None:
                                self.routed_responses += 1
                            return response[len(prefix):]
                elif valid:
                    if escaped:
                        if byte not in (0xdc, 0xdd):
                            valid = False
                            continue
                        byte = 0xc0 if byte == 0xdc else 0xdb
                        escaped = False
                    elif byte == 0xdb:
                        escaped = True
                        continue
                    if len(frame) == 256:
                        valid = False
                    else:
                        frame.append(byte)
        raise TimeoutError("No authenticated mast reply; outcome unknown")

    def command(self, text):
        return self.exchange(text)

    def import_identity(self, role, native, expected):
        if role not in IDENTITY_ROLES:
            raise ValueError("Identity import role must be repeater, room, companion, bot or management")
        role = "bot" if role == "command-bot" else role
        public = check_native_identity(native, expected)
        try:
            response = self.command(f"key {role} {native.hex()}")
        except (OSError, ValueError):
            raise ValueError(f"Identity import reply unavailable; outcome unknown; inspect key {role} pending before reboot") from None
        pending = f"Pending {public}; reboot required; peers must learn new key"
        active = f"KEY {public}; already active; no reboot required"
        if response not in (pending, active):
            errors = {
                "Error: invalid private identity or role identity unavailable",
                "Error: different pending identity or role reset; inspect pending key before cancelling",
                "Error: public identity already active or pending for another role; no change",
                "Error: identity commit/readback unknown; inspect pending key before reboot",
                "Error: private identity import requires encrypted Management RF; web/Lua import denied",
            }
            if response in errors:
                raise ValueError(response)
            raise ValueError(f"Unexpected identity import reply; outcome unknown; inspect key {role} pending before reboot")
        try:
            readback = self.command(f"key {role}" + (" pending" if response == pending else ""))
        except (OSError, ValueError):
            raise ValueError(f"Identity readback unavailable; outcome unknown; inspect key {role} pending before reboot") from None
        if readback != (pending if response == pending else f"KEY {public}"):
            raise ValueError(f"Identity readback mismatch; outcome unknown; inspect key {role} pending before reboot")
        return response

    def set_role_password(self, role, password):
        if role not in ("repeater", "room"):
            raise ValueError("Role administrator password role must be repeater or room")
        check_role_password(password)
        try:
            response = self.command(f"role password {role} {password.hex()}")
        except (OSError, ValueError):
            raise ValueError("Role password reply unavailable; outcome unknown; verify login before retry") from None
        expected = f"Saved and applied {role} administrator password; ACL/sessions unchanged"
        if response == expected:
            return response
        errors = {
            "Error: role inactive/busy; password unchanged",
            "Error: role password persistence unknown; live unchanged; saved may differ; verify before retry",
            "Error: role password requires authenticated encrypted Management RF; web/Lua denied",
            "Error: mast replay storage invalid; administration disabled",
        }
        if response in errors:
            raise ValueError(response)
        raise ValueError("Unexpected role password reply; outcome unknown; verify login before retry")


class UnixClient:
    def __init__(self, path, timeout=20):
        self.path = Path(path)
        if not self.path.is_absolute() or not 1 <= timeout <= 180:
            raise ValueError("Unix management requires an absolute socket path and timeout 1..180 seconds")
        info = self.path.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise ValueError("Unix management requires a private socket owned by the current user")
        self.timeout = timeout

    def command(self, text):
        data = text.encode("ascii")
        if not 1 <= len(data) <= 160 or any(byte < 32 or byte > 126 for byte in data):
            raise ValueError("Unix command requires 1..160 printable ASCII bytes")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(self.timeout)
            connection.connect(str(self.path))
            _, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != os.geteuid():
                raise ValueError("Unix management server belongs to another user")
            connection.sendall(data + b"\n")
            with connection.makefile("rb") as stream:
                response = stream.readline(258)
            if not response.endswith(b"\n") or len(response) > 257:
                raise ValueError("Incomplete or oversized Unix management response")
            return response[:-1].decode("ascii")

    def close(self):
        pass

    def import_identity(self, role, native, expected):
        if role not in ("bot", "command-bot"):
            raise ValueError("Go Unix management imports only the native bot identity")
        public = check_native_identity(native, expected)
        pending = f"KEY {public} pending; apply required"
        active = f"KEY {public}; already active; no apply required"
        try:
            response = self.command(f"key bot {native.hex()}")
            readback = ""
            if response == pending:
                readback = self.command("key bot pending")
            elif response == active:
                readback = self.command("key bot")
        except (OSError, ValueError):
            raise ValueError("Bot identity stage/readback unavailable; inspect key bot pending; no apply requested") from None
        if response not in (pending, active) or readback != (pending if response == pending else f"KEY {public}"):
            raise ValueError("Bot identity stage/readback unavailable; inspect key bot pending; no apply requested")
        return response


class WebClient:
    def __init__(self, url, password):
        if not re.fullmatch(r"https?://[^/?#]+", url):
            raise ValueError("Use an explicit http(s)://host[:port] base URL")
        self.url = url
        self.token = self.post("/admin/login", password)
        if not re.fullmatch("[0-9a-f]{32}", self.token):
            raise ValueError("Invalid web session response")

    def post(self, path, text):
        request = urllib.request.Request(self.url + path, data=text.encode("ascii"), method="POST")
        if hasattr(self, "token"):
            request.add_header("X-Mast-Session", self.token)
        with urllib.request.urlopen(request, timeout=5) as response:
            data = response.read(164)
        if len(data) > 162:
            raise ValueError("Oversized web response")
        return data.decode("ascii")

    def command(self, text):
        if len(text.encode("ascii")) > 162:
            raise ValueError("CLI text exceeds 162 bytes")
        return self.post("/admin/command", text)

    def close(self):
        try:
            self.post("/admin/logout", "")
        except OSError as error:
            print(f"Web logout unavailable; session expires automatically: {error}", file=sys.stderr)


def checked(client, text):
    response = client.command(text)
    if response.startswith("Error:"):
        raise ValueError(response)
    return response


class RuntimeClient:
    """Select a runtime without changing authentication or transfer framing."""
    def __init__(self, client, runtime):
        self.client = client.client if isinstance(client, RuntimeClient) else client
        self.runtime = runtime

    def command(self, text):
        if (self.runtime == bot_packages.WASM_RUNTIME and text.startswith("source ") and
                not text.startswith("source wasm ")):
            text = "source wasm " + text[7:]
        return self.client.command(text)

    def __getattr__(self, name):
        return getattr(self.client, name)


def runtime_client(client, runtime):
    if runtime == bot_packages.WASM_RUNTIME:
        return RuntimeClient(client, runtime)
    return client.client if isinstance(client, RuntimeClient) else client


def download(client):
    manifest = checked(client, "source hash")
    if not re.fullmatch(r"SHA256 [0-9a-f]{64} gen=\d+", manifest):
        raise ValueError("Invalid source manifest")
    source = bytearray()
    for index in range((SOURCE_LIMIT + CHUNK - 1) // CHUNK):
        response = checked(client, f"source read {index}")
        if response == "EOF":
            break
        if not response.startswith("DATA ") or not re.fullmatch("[0-9a-f]{2,96}", response[5:]):
            raise ValueError("Invalid source read response")
        source.extend(bytes.fromhex(response[5:]))
        if len(source) > SOURCE_LIMIT:
            raise ValueError("Source read limit exceeded")
    if hashlib.sha256(source).hexdigest() != manifest.split()[1] or checked(client, "source hash") != manifest:
        raise ValueError("Source changed or failed its hash while downloading")
    return bytes(source)


def install(client, source, progress=print):
    wasm = source.startswith(b"\0asm\1\0\0\0")
    if wasm:
        client = runtime_client(client, bot_packages.WASM_RUNTIME)
        api = checked(client, "source api")
        if not api.startswith("API meshcore-v1 runtime=wamr-2.4.1 "):
            raise ValueError("Device does not support portable Wasm installation")
    if source.startswith(bot_packages.PREFIX):
        package = bot_packages.inspect(source)
        wasm = package.runtime == bot_packages.WASM_RUNTIME
        client = runtime_client(client, package.runtime)
    if not source or len(source) > SOURCE_LIMIT or source.startswith(b"\x1b") or (b"\0" in source and not wasm):
        raise ValueError("Expected 1..4096 bytes of Lua source or a packaged portable Wasm module")
    if not wasm and isinstance(client, RuntimeClient) and client.runtime == bot_packages.WASM_RUNTIME:
        raise ValueError("Selected Wasm runtime requires a portable binary module or package")
    digest = hashlib.sha256(source).hexdigest()
    identifier = digest[:16]
    response = checked(client, f"source begin {identifier} {len(source)} {digest}")
    match = re.fullmatch(r"ACK " + identifier + r" next=(\d+)", response)
    if not match:
        raise ValueError("Unexpected upload admission response")
    next_chunk = int(match[1])
    if next_chunk > (len(source) + CHUNK - 1) // CHUNK:
        raise ValueError("Invalid resume position")
    for index in range(next_chunk, (len(source) + CHUNK - 1) // CHUNK):
        data = source[index * CHUNK:(index + 1) * CHUNK]
        command = f"source chunk {identifier} {index} {data.hex()}"
        for attempt in range(3):
            try:
                response = checked(client, command)
                break
            except (TimeoutError, socket.timeout):
                if attempt == 2:
                    raise
                # Repeating this exact numbered chunk cannot reapply a mutation.
        if response != f"ACK {identifier} next={index + 1}":
            raise ValueError("Chunk not durably acknowledged")
        progress(f"Durable chunk {index + 1}/{(len(source) + CHUNK - 1) // CHUNK}")
    progress(checked(client, f"source commit {identifier}"))
    for _ in range(20):
        time.sleep(.5)
        status = checked(client, "source status")
        if "Error:" in status:
            raise ValueError(status)
        if "durably saved and active" in status or "bot disabled" in status:
            if not checked(client, "source hash").startswith("SHA256 " + digest + " "):
                raise ValueError("A different source is selected; installation was superseded")
            progress(status)
            return status
    raise TimeoutError("Activation still pending; inspect source status")


def check_device_compatibility(client, package):
    client = runtime_client(client, package.runtime)
    api = checked(client, "source api")
    runtime_field = (f"runtime={package.runtime}" if package.runtime == bot_packages.WASM_RUNTIME
                     else f"lua={package.runtime.removeprefix('lua-')}")
    if not api.startswith("API " + package.api + " ") or runtime_field not in api:
        raise ValueError("Device runtime or command API is incompatible with the package")
    package_api = checked(client, "source api package")
    match = re.fullmatch(
        r"Package(?: runtime=(?P<runtime>[^ ]+))? api=(?P<api>[^ ]+) caps=(?P<caps>[a-z.,-]+)",
        package_api,
    )
    if (not match or (match.group("runtime") and match.group("runtime") != package.runtime) or
            match.group("api") != package.api):
        raise ValueError("Invalid device package runtime/API/capability contract")
    available = set(match.group("caps").split(","))
    missing = set(package.capabilities) - available
    if missing:
        raise ValueError("Device does not provide required capabilities: " + ", ".join(sorted(missing)))
    current = checked(client, "source metadata")
    if current in ("BUNDLED schema=none", "EMPTY schema=none"):
        return None
    if current == "PLAIN schema=unknown":
        raise ValueError("Active plain Lua source has unknown data schema; restore bundled source before package install")
    match = re.fullmatch(r"META name=[a-z][a-z0-9-]{0,23} ver=[0-9]+\.[0-9]+\.[0-9]+ schema=(none|[a-z][a-z0-9-]{0,23}@[1-9][0-9]{0,4}) rollback=(none|[a-z][a-z0-9-]{0,23}@[1-9][0-9]{0,4})", current)
    if not match:
        raise ValueError("Invalid active package metadata response")
    active_schema = match[1]
    if active_schema == package.schema:
        return current
    bot_packages.validate_schema_transition(active_schema, package)
    return current


def package_install(client, source, signature=None, public_key=None, progress=print):
    package = bot_packages.inspect(source)
    client = runtime_client(client, package.runtime)
    if (signature is None) != (public_key is None):
        raise ValueError("Package signature and verification key must be supplied together")
    if signature is not None:
        digest = bot_packages.verify(source, signature, public_key)
        progress(f"Verified optional Ed25519 signature for SHA256 {digest}")
    active = check_device_compatibility(client, package)
    digest = hashlib.sha256(source).hexdigest()
    status = install(client, source, progress)
    if not checked(client, "source hash").startswith("SHA256 " + digest + " "):
        raise ValueError("Installed package SHA256 differs from the validated source")
    progress(f"Package {package.name} {package.version} active; previous metadata={active or 'bundled/plain'}")
    return status


def wait_for_source_change(client, previous_hash, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = checked(client, "source status")
        if "Error:" in status:
            raise ValueError(status)
        if "durably saved and active" in status or "bot disabled" in status:
            current = checked(client, "source hash")
            if current.startswith("SHA256 ") and current.split()[1] != previous_hash:
                return status, current
        time.sleep(.25)
    raise TimeoutError("Source lifecycle still pending; inspect source status and hash")


def lifecycle(client, action):
    if action == "status":
        for command in ("source status", "source hash", "source metadata", "source api", "source api package",
                        "bot status", "bot stats"):
            print(f"{command}: {checked(client, command)}")
        return
    if action == "diagnose":
        for command in ("source status", "source hash", "source metadata", "bot status", "bot stats"):
            print(f"{command}: {checked(client, command)}")
        return
    if action == "reboot":
        print(checked(client, "reboot"))
        return
    previous = checked(client, "source hash")
    match = re.fullmatch(r"SHA256 ([0-9a-f]{64}) gen=\d+", previous)
    if not match:
        raise ValueError("Invalid active source hash")
    command = {"rollback": "source rollback", "remove": "source remove"}[action]
    response = checked(client, command)
    if not response.startswith("Accepted"):
        raise ValueError(f"Source {action} was not accepted: {response}")
    status, current = wait_for_source_change(client, match[1])
    print(status)
    print(current)


def fetch_package(client, endpoint_alias, expected_hash, timeout=120):
    if endpoint_alias != "package":
        raise ValueError("Streaming package fetch requires the fixed 'package' endpoint alias")
    if not re.fullmatch("[0-9a-f]{64}", expected_hash):
        raise ValueError("Streaming package fetch requires a full lowercase expected SHA256")
    fetch_api = checked(client, "source api fetch")
    wasm_api = "Package GET runtime=wamr-2.4.1 alias=package raw=1..4096 type=application/wasm|application/octet-stream SHA256=source"
    expected_api = wasm_api if isinstance(client, RuntimeClient) and client.runtime == bot_packages.WASM_RUNTIME else PACKAGE_FETCH_API
    if fetch_api != expected_api:
        raise ValueError("Device does not advertise native streaming package fetch")
    before = checked(client, "source hash")
    match = re.fullmatch(r"SHA256 [0-9a-f]{64} gen=(\d+)", before)
    if not match:
        raise ValueError("Invalid active source hash")
    response = checked(client, f"source fetch {endpoint_alias} {expected_hash}")
    if not response.startswith("Accepted"):
        raise ValueError(f"Streaming package fetch was not accepted: {response}")
    previous_generation = int(match[1])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = checked(client, "source status")
        if "Error:" in status:
            raise ValueError(status)
        generation = re.search(r"\bgen=(\d+)", status)
        current = checked(client, "source hash")
        if (generation and int(generation[1]) > previous_generation and
                ("durably saved and active" in status or "bot disabled" in status) and
                current.startswith("SHA256 " + expected_hash + " ")):
            print(status)
            print(checked(client, "source metadata"))
            return current
        time.sleep(.25)
    raise TimeoutError("Streaming package fetch still pending or unknown; inspect source status before retrying")


def data_wait(client, expected):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        response = checked(client, "data status")
        if response.startswith(expected + " "):
            return response
        if not response.startswith(("BUSY ", "PENDING ")):
            raise ValueError(f"Bot-data operation failed: {response}")
        time.sleep(.1)
    raise TimeoutError("Bot-data status unavailable; outcome unknown, inspect data status before retry")


def data_export(client, scope, principal, kind="kv"):
    if kind not in DATA_MAGIC or (kind == "reminders" and scope != "caller"):
        raise ValueError("Invalid data kind or reminder scope")
    if scope not in ("caller", "conversation", "bot", "channel") or not re.fullmatch("[0-9a-f]{64}", principal):
        raise ValueError("Data export requires scope and full lowercase principal hex")
    if (scope == "bot") != (principal == "0" * 64):
        raise ValueError("Use zero principal only for bot scope")
    admitted = checked(client, f"data export {'' if kind == 'kv' else kind + ' '}{scope} {principal}")
    if not admitted.startswith("PENDING"):
        raise ValueError(f"Bot-data export not admitted: {admitted}")
    status = data_wait(client, "EXPORTED")
    match = re.fullmatch(r"EXPORTED ([0-9a-f]{64}) ([0-9a-f]{16})", status)
    if not match or match[2] != match[1][:16]:
        raise ValueError("Invalid bot-data export manifest")
    data = bytearray()
    for index in range((DATA_LIMIT + CHUNK - 1) // CHUNK):
        response = checked(client, f"data read {match[2]} {index}")
        if not re.fullmatch(r"DATA (?:[0-9a-f]{2}){1,48}", response):
            raise ValueError("Invalid bot-data export chunk")
        data.extend(bytes.fromhex(response[5:]))
    if len(data) != DATA_LIMIT or hashlib.sha256(data).hexdigest() != match[1] or checked(client, "data status") != status:
        raise ValueError("Bot-data export changed or failed SHA256")
    if (data[:4] != DATA_MAGIC[kind] or data[36] != ("caller", "conversation", "bot", "channel").index(scope)
            or data[37:69] != bytes.fromhex(principal)
            or hashlib.sha256(data[:-32]).digest() != data[-32:]):
        raise ValueError("Bot-data export scope, principal, version or content digest mismatch")
    return bytes(data)


def data_restore(client, data, no_rearm=False):
    if len(data) != DATA_LIMIT or data[:4] not in DATA_MAGIC.values() or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError("Bot-data file version, size or SHA256 invalid")
    scheduler = data[:4] != DATA_MAGIC["kv"]
    if scheduler and not no_rearm:
        raise ValueError("Scheduler restore requires --no-rearm: matching pending work is restored as cancelled")
    digest = hashlib.sha256(data).hexdigest()
    ident = digest[:16]
    if checked(client, f"data begin {ident} {digest}") != f"UPLOADING {ident} bytes={DATA_LIMIT}":
        raise ValueError("Bot-data upload was not admitted")
    for offset in range(0, len(data), CHUNK):
        reply = checked(client, f"data chunk {ident} {offset // CHUNK} {data[offset:offset + CHUNK].hex()}")
        if reply != f"RECEIVED {min(offset + CHUNK, len(data))}":
            raise ValueError("Bot-data upload acknowledgement mismatch")
    if checked(client, f"data stage {ident}") != f"PENDING {ident}":
        raise ValueError("Bot-data validation was not admitted")
    if data_wait(client, "STAGED") != f"STAGED {ident}":
        raise ValueError("Bot-data stage was superseded")
    if checked(client, f"data restore {ident}{' no-rearm' if scheduler else ''}") != f"PENDING {ident}":
        raise ValueError("Bot-data restore was not admitted; inspect data status")
    committed = data_wait(client, "COMMITTED")
    if not committed.endswith(" " + ident):
        raise ValueError("Bot-data restore was superseded; outcome unknown")
    return committed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway", help="independent KISS gateway host for native encrypted RF")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--target", help="full mast management public key")
    parser.add_argument("--seed-file", type=Path, help="disposable/trusted companion Ed25519 seed")
    parser.add_argument("--password-file", type=Path, help="0600 password file; omit for trusted-key RF login")
    parser.add_argument("--web", help="authenticated HTTP base URL instead of RF")
    parser.add_argument("--unix-socket", type=Path, help="private native host bot owner socket")
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--untagged", action="store_true",
                        help="native repeater console without host-style command tags, including Pine")
    parser.add_argument("--no-command-retry", action="store_true",
                        help="send each native command once; timeout leaves its outcome unknown")
    parser.add_argument("--runtime", choices=("lua", "wasm"), default="lua",
                        help="runtime for status, download, rollback or remove; package installation detects its runtime")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("command").add_argument("text")
    identity = sub.add_parser("key-import", help="stage a supplied native identity over Management RF or the Go bot owner socket; no apply")
    identity.add_argument("role", choices=IDENTITY_ROLES)
    identity.add_argument("--native-key-file", type=Path, required=True, help="0600 raw 64-byte expanded native key")
    identity.add_argument("--expected-public-key", required=True, help="full 64-hex-digit public key from the selected manifest")
    role_password = sub.add_parser("role-password", help="save/apply an active native role administrator password through encrypted Management RF only")
    role_password.add_argument("role", choices=("repeater", "room"))
    role_password.add_argument("--new-password-file", type=Path, required=True,
                               help="0600 file containing 1..15 printable ASCII bytes, without newline; separate from Management --password-file")
    sub.add_parser("install").add_argument("source", type=Path)
    sub.add_parser("download").add_argument("destination", type=Path)
    for action in ("package-install", "update"):
        package_install_parser = sub.add_parser(action, help="validate and atomically install a source package")
        package_install_parser.add_argument("source", type=Path)
        package_install_parser.add_argument("--signature", type=Path)
        package_install_parser.add_argument("--public-key-file", type=Path)
    fetch_parser = sub.add_parser("package-fetch", help="stream source from the fixed owner-configured HTTPS GET alias")
    fetch_parser.add_argument("endpoint_alias", choices=("package",), help="fixed named HTTPS GET alias")
    fetch_parser.add_argument("sha256")
    for action in ("status", "diagnose", "rollback", "remove", "reboot"):
        sub.add_parser(action, help="inspect or recover the on-device source lifecycle")
    export = sub.add_parser("data-export", help="export one scoped KV, timer or reminder record set; no credentials")
    export.add_argument("destination", type=Path)
    export.add_argument("--scope", choices=("caller", "conversation", "bot", "channel"), required=True)
    export.add_argument("--principal", required=True, help="full user/channel identity; 64 zeros for bot scope")
    export.add_argument("--kind", choices=tuple(DATA_MAGIC), default="kv")
    restore = sub.add_parser("data-restore", help="stage a scoped backup; KV replaces atomically, scheduler merges without rearming")
    restore.add_argument("source", type=Path)
    restore.add_argument("--no-rearm", action="store_true", help="required for scheduler import; cancels matching pending work, never replays side effects")
    package = sub.add_parser("package", help="create, inspect, validate or optionally sign Lua/Wasm packages")
    package_actions = package.add_subparsers(dest="package_action", required=True)
    create_parser = package_actions.add_parser("create")
    create_parser.add_argument("source", type=Path)
    create_parser.add_argument("destination", type=Path)
    create_parser.add_argument("--name", required=True)
    create_parser.add_argument("--version", required=True)
    create_parser.add_argument("--capability", action="append", default=[])
    create_parser.add_argument("--schema", default="none")
    create_parser.add_argument("--rollback", default="none")
    for action in ("inspect", "validate"):
        package_actions.add_parser(action).add_argument("source", type=Path)
    sign_parser = package_actions.add_parser("sign")
    sign_parser.add_argument("source", type=Path)
    sign_parser.add_argument("--private-key-file", type=Path, required=True)
    sign_parser.add_argument("--signature", type=Path)
    verify_parser = package_actions.add_parser("verify")
    verify_parser.add_argument("source", type=Path)
    verify_parser.add_argument("--signature", type=Path, required=True)
    verify_parser.add_argument("--public-key-file", type=Path, required=True)
    keygen_parser = package_actions.add_parser("keygen")
    keygen_parser.add_argument("private_key", type=Path)
    keygen_parser.add_argument("public_key", type=Path)
    args = parser.parse_args()
    if args.action == "package":
        try:
            if args.package_action == "create":
                source = read_file(args.source, SOURCE_LIMIT)
                created = bot_packages.create(source, args.name, args.version, args.capability,
                                              args.schema, args.rollback)
                write_new_file(args.destination, created.source)
                print(f"Created {created.name} {created.version}: {len(created.source)} source bytes "
                      f"SHA256 {created.digest}")
            elif args.package_action in ("inspect", "validate"):
                source = read_file(args.source, SOURCE_LIMIT)
                details = bot_packages.inspect(source)
                print(f"name={details.name} version={details.version} runtime={details.runtime} api={details.api}")
                print(f"capabilities={','.join(details.capabilities) or 'none'} schema={details.schema} "
                      f"rollback={details.rollback}")
                print(f"source_bytes={len(source)} sha256={details.digest}")
            elif args.package_action == "sign":
                source = read_file(args.source, SOURCE_LIMIT)
                signature = bot_packages.sign(source, private_file(args.private_key_file, 4096))
                destination = args.signature or Path(str(args.source) + ".sig")
                write_new_file(destination, signature)
                print(f"Wrote optional detached signature {destination}")
            elif args.package_action == "verify":
                source = read_file(args.source, SOURCE_LIMIT)
                signature = read_file(args.signature, 1024)
                public_key = read_file(args.public_key_file, 4096)
                digest = bot_packages.verify(source, signature, public_key)
                print(f"Verified optional Ed25519 signature for SHA256 {digest}")
            else:
                private = ed25519.Ed25519PrivateKey.generate()
                raw_private = private.private_bytes(serialization.Encoding.Raw,
                                                    serialization.PrivateFormat.Raw,
                                                    serialization.NoEncryption())
                raw_public = private.public_key().public_bytes(serialization.Encoding.Raw,
                                                               serialization.PublicFormat.Raw)
                write_new_file(args.private_key, raw_private, 0o600)
                write_new_file(args.public_key, raw_public)
                print(f"Wrote Ed25519 signing key {args.private_key} and public key {args.public_key}")
        except (OSError, ValueError) as error:
            parser.exit(2, f"Mast package command failed: {error}\n")
        return
    client = None
    try:
        if args.action == "role-password":
            if args.web or args.unix_socket:
                raise ValueError("Role administrator password changes require authenticated encrypted Management RF; web/Unix denied")
            new_password = private_file(args.new_password_file, 15)
            check_role_password(new_password)
        if args.action == "key-import":
            if args.web:
                raise ValueError("Identity import requires encrypted Management RF or the Go bot owner socket; web denied")
            native = private_file(args.native_key_file, 64)
            check_native_identity(native, args.expected_public_key)
        if args.action == "command":
            words = args.text.split()
            if words[:2] == ["role", "password"]:
                raise ValueError("Use role-password with --new-password-file; role passwords must not appear in command arguments")
            private_key = words[:1] == ["key"] and len(words) >= 3 and words[2:] not in (["pending"], ["cancel"], ["apply"])
            private_key = private_key or (words[:2] == ["role", "key"] and len(words) >= 4 and
                                          words[3:] not in (["pending"], ["rotate"], ["cancel"], ["apply"]))
            if private_key:
                raise ValueError("Use key-import with --native-key-file; private identities must not appear in command arguments")
        if args.unix_socket:
            if args.web or args.gateway or args.target or args.seed_file or args.password_file:
                raise ValueError("Unix management does not use RF/web targets or credentials")
            client = UnixClient(args.unix_socket, args.timeout)
        else:
            password = private_file(args.password_file, 15).decode("ascii") if args.password_file else ""
            if args.web:
                client = WebClient(args.web, password)
            else:
                if not args.gateway or not args.target or not args.seed_file or not 1 <= args.timeout <= 180:
                    raise ValueError("RF requires gateway, target, seed-file and timeout 1..180 seconds")
                client = NativeClient(args.gateway, args.port, args.seed_file, args.target, password, args.timeout,
                                      tagged=not args.untagged, retry_commands=not args.no_command_retry)
        if args.runtime == "wasm":
            client = runtime_client(client, bot_packages.WASM_RUNTIME)
        if args.action == "command":
            print(checked(client, args.text))
        elif args.action == "key-import":
            print(client.import_identity(args.role, native, args.expected_public_key))
        elif args.action == "role-password":
            print(client.set_role_password(args.role, new_password))
        elif args.action == "install":
            source = read_file(args.source, SOURCE_LIMIT)
            install(client, source)
        elif args.action in ("package-install", "update"):
            source = read_file(args.source, SOURCE_LIMIT)
            signature = read_file(args.signature, 1024) if args.signature else None
            public_key = read_file(args.public_key_file, 4096) if args.public_key_file else None
            package_install(client, source, signature, public_key)
        elif args.action == "package-fetch":
            fetch_package(client, args.endpoint_alias, args.sha256)
        elif args.action in ("status", "diagnose", "rollback", "remove", "reboot"):
            lifecycle(client, args.action)
        elif args.action == "data-export":
            data = data_export(client, args.scope, args.principal, args.kind)
            write_new_file(args.destination, data, 0o600)
            print(f"Saved {len(data)} scoped {args.kind} data bytes (0600)")
        elif args.action == "data-restore":
            print(data_restore(client, private_file(args.source, DATA_LIMIT), args.no_rearm))
        else:
            source = download(client)
            write_new_file(args.destination, source)
            print(f"Saved {len(source)} source bytes")
    except (OSError, ValueError) as error:
        parser.exit(2, f"Mast command failed: {error}\n")
    finally:
        if client:
            client.close()


if __name__ == "__main__":
    main()
