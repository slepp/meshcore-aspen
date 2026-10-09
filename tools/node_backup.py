#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inspect or unpack an operator-encrypted MeshCore node backup."""
import argparse
import base64
import binascii
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tarfile
import time
import urllib.request
import urllib.error

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.hardware.admin import NativeClient, checked, private_file, write_new_file
from tools.hardware.rf import _shared_secret, public_hex

RAW_LIMIT = 2 * 1024 * 1024
FILE_LIMIT = RAW_LIMIT + (RAW_LIMIT + 127) // 128 + 120
ENCRYPTION_DOMAIN = b"meshcore-backup-v1 encryption"
AUTHENTICATION_DOMAIN = b"meshcore-backup-v1 authentication"


def unpack_rle(encoded):
    raw = bytearray()
    offset = 0
    while offset < len(encoded):
        control = encoded[offset]
        offset += 1
        count = (control & 127) + 1
        if count > RAW_LIMIT - len(raw):
            raise ValueError("Backup exceeds the 2 MiB decoded limit")
        if control & 128:
            if count < 3 or offset == len(encoded):
                raise ValueError("Backup contains an invalid RLE run")
            raw.extend(bytes((encoded[offset],)) * count)
            offset += 1
        else:
            if count > len(encoded) - offset:
                raise ValueError("Backup contains a truncated RLE literal")
            raw.extend(encoded[offset:offset + count])
            offset += count
    return bytes(raw)


def decrypt(raw, seed):
    if len(raw) < 120 or len(raw) > FILE_LIMIT or raw[:8] != b"MCB\x01\x01\0\0\0":
        raise ValueError("Expected a MeshCore node backup v1")
    if len(seed) != 32 or raw[40:72] != bytes.fromhex(public_hex(seed)):
        raise ValueError("This backup is encrypted to a different operator key")
    shared = _shared_secret(Ed25519PrivateKey.from_private_bytes(seed), raw[8:40])
    mac_key = hashlib.sha256(AUTHENTICATION_DOMAIN + shared).digest()
    expected = hmac.new(mac_key, raw[:-32], hashlib.sha256).digest()
    if not hmac.compare_digest(expected, raw[-32:]):
        raise ValueError("Backup authentication failed; file is damaged or was changed")
    key = hashlib.sha256(ENCRYPTION_DOMAIN + shared).digest()[:16]
    cipher = Cipher(algorithms.AES(key), modes.CTR(raw[72:88])).decryptor()
    return unpack_rle(cipher.update(raw[88:-32]) + cipher.finalize())


def entries(raw):
    if len(raw) < 1024 or len(raw) % 512 or raw[-1024:] != bytes(1024):
        raise ValueError("Backup tar archive is incomplete")
    result = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
        for member in archive:
            parts = member.name.split("/")
            if (not member.isfile() or len(result) >= 512 or member.name in result or
                    not member.name or len(member.name) > 99 or
                    any(part in ("", ".", "..") for part in parts) or
                    any(ord(c) < 32 or ord(c) > 126 or c == "\\" for c in member.name) or
                    (member.name != "manifest.json" and parts[0] not in ("files", "nvs", "config", "identities"))):
                raise ValueError("Backup contains an unsafe or unsupported archive entry")
            source = archive.extractfile(member)
            if source is None:
                raise ValueError("Backup file entry cannot be read")
            content = source.read()
            if len(content) != member.size:
                raise ValueError("Backup file entry is incomplete")
            result[member.name] = content
    manifest = json.loads(result.get("manifest.json", b"null"))
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1 or manifest.get("format") != "meshcore-node-backup":
        raise ValueError("Backup manifest is missing or has an unsupported format")
    return manifest, result


def extract(files, directory):
    directory.mkdir(mode=0o700)
    for name, content in files.items():
        target = directory / name
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        write_new_file(target, content, mode=0o600)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        raise ValueError("Node backup endpoint redirected; use its direct URL")


class HttpClient:
    def __init__(self, url, password):
        if not re.fullmatch(r"https?://[^/?#]+", url):
            raise ValueError("Use an explicit http(s)://host[:port] base URL")
        self.url = url
        self.opener = urllib.request.build_opener(NoRedirect)
        with self.request("/admin/login", password) as response:
            self.token = response.read(65).decode("ascii")
        if not re.fullmatch(r"(?:[0-9a-f]{32}|[0-9a-f]{64})", self.token):
            raise ValueError("Node backup login returned an invalid session")

    def request(self, path, body=None):
        headers = {"X-Host-Intent": "admin-v1"}
        if hasattr(self, "token"):
            headers.update({"X-Mast-Session": self.token, "X-Host-Admin": self.token})
        request = urllib.request.Request(self.url + path, data=body, headers=headers)
        try:
            return self.opener.open(request, timeout=90)
        except urllib.error.HTTPError as error:
            detail = error.read(256).decode("utf-8", errors="replace").strip()
            raise ValueError(f"Node backup HTTP {error.code}: {detail}") from error

    def command(self, command):
        with self.request("/admin/command", command.encode("ascii")) as response:
            text = response.read(164)
            if len(text) > 162:
                raise ValueError("Node backup command reply exceeds 162 bytes")
            return text.decode("ascii")

    def chunks(self, identifier):
        with self.request("/admin/backup?id=" + identifier) as response:
            while chunk := response.read(4096):
                yield chunk

    def close(self):
        try:
            with self.request("/admin/logout", b"") as response:
                response.read(256)
        except (OSError, ValueError) as error:
            print(f"Node backup logout failed: {error}; session expires after ten minutes", file=sys.stderr)


def ready(client, radio):
    deadline = time.monotonic() + 180
    while True:
        status = checked(client, "backup status")
        match = re.fullmatch(r"READY ([0-9a-f]{16}) bytes=([0-9]+) sha=([0-9a-f]{64})", status)
        if match:
            identifier, size, digest = match.groups()
            if identifier != digest[:16] or not 120 <= int(size) <= FILE_LIMIT:
                raise ValueError("Node backup status contains an invalid id, size or checksum")
            return {"schema_version": 1, "id": identifier, "bytes": int(size), "sha256": digest}
        if not status.startswith("PREPARING"):
            raise ValueError("Unexpected node backup status: " + status)
        if time.monotonic() >= deadline:
            raise TimeoutError("Backup preparation timed out; inspect backup status before requesting another snapshot")
        time.sleep(5 if radio else 1)


def rf_chunks(client, metadata, offset):
    verb = "read64"
    while offset < metadata["bytes"]:
        response = client.command(f"backup {verb} {metadata['id']} {offset}")
        if verb == "read64" and response == "Error: unknown backup command; use backup help":
            print("Node backup RF: legacy firmware; using 48-byte hex replies", file=sys.stderr)
            verb = "read"
            continue
        if response.startswith("Error:"):
            raise ValueError(response)
        wait = re.fullmatch(r"WAIT ms=([0-9]+)", response)
        if wait:
            milliseconds = int(wait[1])
            if not 1 <= milliseconds <= 5000:
                raise ValueError("Node backup returned an invalid pacing interval")
            time.sleep(milliseconds / 1000 + 0.1)
            continue
        pattern = (r"CHUNK64 ([0-9a-f]{16}) ([0-9]+) ([A-Za-z0-9+/]{2,162})" if verb == "read64" else
                   r"CHUNK ([0-9a-f]{16}) ([0-9]+) ([0-9a-f]{2,96})")
        match = re.fullmatch(pattern, response)
        if len(response) > 162 or not match or match[1] != metadata["id"] or int(match[2]) != offset:
            raise ValueError("Node backup RF chunk has the wrong id, offset or encoding")
        text = match[3]
        try:
            chunk = (base64.b64decode(text + "=" * (-len(text) % 4), validate=True) if verb == "read64" else
                     bytes.fromhex(text))
        except (binascii.Error, ValueError) as error:
            raise ValueError("Node backup RF chunk has invalid encoding") from error
        if verb == "read64" and base64.b64encode(chunk).decode("ascii").rstrip("=") != text:
            raise ValueError("Node backup RF chunk has noncanonical Base64 encoding")
        if not chunk or len(chunk) > metadata["bytes"] - offset:
            raise ValueError("Node backup RF chunk has an unexpected length")
        if verb == "read" and len(chunk) != min(48, metadata["bytes"] - offset):
            raise ValueError("Node backup RF chunk has an unexpected length")
        yield chunk
        previous = offset
        offset += len(chunk)
        if previous // 1024 != offset // 1024 or offset == metadata["bytes"]:
            print(f"Node backup RF: {offset}/{metadata['bytes']} bytes saved", file=sys.stderr)
        if offset < metadata["bytes"]:
            time.sleep(5)


def download(client, output, seed, resume=False, radio=False, saved=False, transient=False):
    partial = Path(str(output) + ".part")
    metadata_path = Path(str(partial) + ".json")
    if output.exists() or output.is_symlink():
        raise FileExistsError("Backup destination exists; use a new filename")
    if not resume and (partial.exists() or metadata_path.exists()):
        raise FileExistsError("Partial backup exists; use --resume with this destination")
    if resume and not metadata_path.exists():
        raise ValueError("No partial backup metadata; use a new destination without --resume")
    if resume or saved:
        checked(client, "backup load-ram" if transient else "backup load")
    else:
        checked(client, ("backup start-ram " if transient else "backup start ") + public_hex(seed))
    metadata = ready(client, radio)
    metadata["recipient"] = public_hex(seed)
    if transient:
        metadata["volatile"] = True
    if resume:
        saved = json.loads(private_file(metadata_path, 1024))
        if saved != metadata:
            raise ValueError("Saved backup changed or belongs to another key; use a new destination")
    else:
        write_new_file(metadata_path, (json.dumps(metadata) + "\n").encode(), mode=0o600)
    flags = os.O_RDWR | os.O_NOFOLLOW
    if not partial.exists():
        flags |= os.O_CREAT | os.O_EXCL
    fd = os.open(partial, flags, 0o600)
    with os.fdopen(fd, "r+b") as file:
        info = os.fstat(file.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_size > metadata["bytes"]:
            raise ValueError("Partial backup must be a private regular file within the expected size")
        offset = info.st_size
        file.seek(offset)
        chunks = rf_chunks(client, metadata, offset) if radio else client.chunks(metadata["id"])
        skipped = 0
        for chunk in chunks:
            if not radio and skipped < offset:
                count = min(len(chunk), offset - skipped)
                file.seek(skipped)
                if file.read(count) != chunk[:count]:
                    raise ValueError("Saved WiFi backup prefix differs; partial file retained")
                skipped += count
                file.seek(0, os.SEEK_END)
                chunk = chunk[count:]
            if not chunk:
                continue
            if len(chunk) > metadata["bytes"] - file.tell():
                raise ValueError("Node backup download exceeds its advertised size")
            file.write(chunk); file.flush(); os.fsync(file.fileno())
            if file.tell() >= 88:
                position = file.tell()
                file.seek(0)
                header = file.read(88)
                file.seek(position)
                if header[:8] != b"MCB\x01\x01\0\0\0" or header[40:72] != bytes.fromhex(metadata["recipient"]):
                    raise ValueError("Saved node backup is not encrypted to this operator key")
        file.seek(0)
        raw = file.read(FILE_LIMIT + 1)
        if len(raw) != metadata["bytes"] or hashlib.sha256(raw).hexdigest() != metadata["sha256"]:
            raise ValueError("Node backup download is incomplete or its checksum failed; partial file retained")
        manifest, records = entries(decrypt(raw, seed))
    os.link(partial, output)
    partial.unlink()
    metadata_path.unlink()
    directory = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return manifest, len(records), len(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("download", "inspect", "extract"))
    parser.add_argument("backup", type=Path)
    parser.add_argument("--key-file", type=Path, required=True, help="Private operator Ed25519 seed file (0600)")
    parser.add_argument("--directory", type=Path, help="New private directory for extracted records")
    parser.add_argument("--url", help="Aspen or Birch http(s)://host[:port]")
    parser.add_argument("--gateway", help="Companion radio KISS host for encrypted admin DM")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--target", help="Management or repeater full public key for RF administration")
    parser.add_argument("--password-file", type=Path, help="Private admin password file; omit for trusted-key RF login")
    parser.add_argument("--volatile", action="store_true",
                        help="Aspen PSRAM snapshot, lost on restart; leaves the saved filesystem backup unchanged")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--resume", action="store_true", help="Resume this destination's partial download without creating a new snapshot")
    selection.add_argument("--saved", action="store_true", help="Download the saved snapshot to a new destination without creating another backup")
    args = parser.parse_args()
    try:
        os.umask(0o077)
        seed = private_file(args.key_file, 32)
        if args.action == "download":
            if bool(args.url) == bool(args.gateway) or (args.gateway and not args.target):
                raise ValueError("Choose --url or --gateway with --target")
            password = private_file(args.password_file, 256) if args.password_file else b""
            client = (HttpClient(args.url, password) if args.url else
                      NativeClient(args.gateway, args.port, args.key_file, args.target,
                                   password.decode("ascii"), timeout=40, retry_commands=False))
            try:
                _, count, size = download(client, args.backup, seed, args.resume, bool(args.gateway), args.saved,
                                          args.volatile)
            finally:
                client.close()
            print(f"Saved encrypted node backup: {args.backup} ({size} bytes, {count} records)")
            return
        if args.backup.stat().st_size > FILE_LIMIT:
            raise ValueError("Backup file exceeds the size limit")
        manifest, files = entries(decrypt(args.backup.read_bytes(), seed))
        if args.action == "extract":
            if args.directory is None:
                raise ValueError("Extraction requires --directory pointing to a new private directory")
            extract(files, args.directory)
            print(f"Extracted {len(files)} private records to {args.directory}")
        else:
            print(json.dumps({**manifest, "records": len(files),
                              "record_bytes": sum(len(value) for value in files.values())}, indent=2))
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f"Node backup: {error}\n")


if __name__ == "__main__":
    main()
