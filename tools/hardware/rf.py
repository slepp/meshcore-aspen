#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send a signed on-chip role selection through a separate MeshCore KISS radio."""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import stat
import struct
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.exceptions import InvalidSignature


DOMAIN = b"MCORE-ROLE-RF-V1"
RECEIPT_DOMAIN = b"MCORE-ROLE-RCPT-V1"
QUERY_DOMAIN = b"MCORE-ROLE-QRY-V1"
STATUS_DOMAIN = b"MCORE-ROLE-STAT-V1"
ROOT = Path(__file__).resolve().parents[2]
ROLES = {"repeater": 1, "room": 2, "companion": 4, "observer": 8}
P = (1 << 255) - 19
MAX_KISS_FRAME = 256
MAX_RX_BYTES = 8 * 1024 * 1024


class ReceiptContext:
    __slots__ = ("sender_key", "sender_public", "target", "generation", "nonce", "secret")

    def __init__(self, sender_key, sender_public, target, generation, nonce, secret):
        self.sender_key = sender_key
        self.sender_public = sender_public
        self.target = target
        self.generation = generation
        self.nonce = nonce
        self.secret = secret


def key_path(value):
    path = Path(value).expanduser().resolve()
    if not path.is_absolute() or path.is_relative_to(ROOT):
        raise ValueError("Keep the operator seed outside the repository")
    return path


def public_hex(seed):
    return ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    ).hex()


def init_key(path):
    path = key_path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    seed = secrets.token_bytes(32)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(seed)
    except OSError:
        path.unlink()
        raise
    print(f"Operator verification public key: {public_hex(seed)}")
    print(f"Operator seed stored at {path}; back it up securely, never copy it to the mast.")


def read_key(path):
    path = key_path(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as input_file:
        info = os.fstat(input_file.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError("Operator seed must be a private regular file (mode 0600)")
        seed = input_file.read(33)
    if len(seed) != 32:
        raise ValueError("Operator seed must be exactly 32 bytes")
    return seed


def target_key(value):
    try:
        key = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError("Management target must be a 64-digit hex public key") from error
    if len(value) != 64 or len(key) != 32:
        raise ValueError("Management target must be a 64-digit hex public key")
    return key


def role_mask(value):
    if value == "none":
        return 0
    names = value.split(",")
    if not names or len(set(names)) != len(names) or any(name not in ROLES for name in names):
        raise ValueError("Roles must be a unique comma-separated list of repeater,room,companion,observer or none")
    return sum(ROLES[name] for name in names)


def _shared_secret(sender_key, target):
    sender_seed = sender_key.private_bytes(
        serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
        serialization.NoEncryption()
    )
    scalar = hashlib.sha512(sender_seed).digest()[:32]
    y = int.from_bytes(target, "little") & ((1 << 255) - 1)
    if y >= P or y == 1:
        raise ValueError("Invalid management public key")
    montgomery = ((1 + y) * pow(1 - y, -1, P) % P).to_bytes(32, "little")
    secret = x25519.X25519PrivateKey.from_private_bytes(scalar).exchange(
        x25519.X25519PublicKey.from_public_bytes(montgomery)
    )
    if not any(secret):
        raise ValueError("Invalid management ECDH result")
    return secret


def _request_with_context(seed, target, body, generation, nonce, sender_seed):
    signer = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
    envelope = body + signer.sign(body)
    envelope += bytes((-len(envelope)) % 16)
    sender = (ed25519.Ed25519PrivateKey.generate() if sender_seed is None else
              ed25519.Ed25519PrivateKey.from_private_bytes(sender_seed))
    sender_pub = sender.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    secret = _shared_secret(sender, target)
    encryptor = Cipher(algorithms.AES(secret[:16]), modes.ECB()).encryptor()
    encrypted = encryptor.update(envelope) + encryptor.finalize()
    mac = hmac.new(secret, encrypted, hashlib.sha256).digest()[:2]
    packet = bytes((0x1d, 0x80, target[0])) + sender_pub + mac + encrypted
    return packet, ReceiptContext(sender, sender_pub, target, generation, nonce, secret)


def frame_with_context(seed, target, generation, nonce, mask, sender_seed=None):
    if not 1 <= generation < 1 << 64 or not 1 <= nonce < 1 << 64:
        raise ValueError("Generation and nonce must be nonzero uint64 values")
    body = DOMAIN + target + struct.pack("<QQB", generation, nonce, mask)
    return _request_with_context(seed, target, body, generation, nonce, sender_seed)


def frame(seed, target, generation, nonce, mask, sender_seed=None):
    return frame_with_context(seed, target, generation, nonce, mask, sender_seed)[0]


def query_with_context(seed, target, nonce, sender_seed=None):
    if not 1 <= nonce < 1 << 64:
        raise ValueError("Query nonce must be a nonzero uint64 value")
    body = QUERY_DOMAIN + target + struct.pack("<Q", nonce)
    return _request_with_context(seed, target, body, 0, nonce, sender_seed)


def kiss(packet):
    data = bytes((0,)) + packet
    return b"\xc0" + data.replace(b"\xdb", b"\xdb\xdd").replace(
        b"\xc0", b"\xdb\xdc"
    ) + b"\xc0"


def _decrypt_response(packet, context):
    if len(packet) < 2 or packet[0] not in (0x04, 0x05, 0x06, 0x07):
        return None
    start = 5 if packet[0] & 3 in (0, 3) else 1
    if len(packet) <= start:
        return None
    encoded_path = packet[start]
    path_bytes = (encoded_path & 63) * ((encoded_path >> 6) + 1)
    if (encoded_path >> 6 == 3 or path_bytes > 64 or
            len(packet) != start + 1 + path_bytes + 180):
        return None
    payload = packet[start + 1 + path_bytes:]
    if payload[:2] != bytes((context.sender_public[0], context.target[0])):
        return None
    ciphertext = payload[4:]
    if not hmac.compare_digest(
        payload[2:4], hmac.new(context.secret, ciphertext, hashlib.sha256).digest()[:2]
    ):
        return None
    decryptor = Cipher(algorithms.AES(context.secret[:16]), modes.ECB()).decryptor()
    return decryptor.update(ciphertext) + decryptor.finalize()


def verify_receipt(packet, context):
    plaintext = _decrypt_response(packet, context)
    if plaintext is None:
        return None
    expected = (RECEIPT_DOMAIN + context.target + context.sender_public +
                struct.pack("<QQ", context.generation, context.nonce))
    if (not hmac.compare_digest(plaintext[:98], expected) or
            plaintext[98] not in (1, 2, 3) or plaintext[163:] != bytes(13)):
        return None
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(context.target).verify(
            plaintext[99:163], plaintext[:99]
        )
    except InvalidSignature:
        return None
    return plaintext[98]


def verify_status(packet, context):
    plaintext = _decrypt_response(packet, context)
    if plaintext is None:
        return None
    expected = (STATUS_DOMAIN + context.target + context.sender_public +
                struct.pack("<Q", context.nonce))
    flags = plaintext[100]
    if (not hmac.compare_digest(plaintext[:90], expected) or
            flags & ~3 or plaintext[165:] != bytes(11) or
            plaintext[98] & ~15 or plaintext[99] & ~15 or
            not (flags & 1) and plaintext[90:100] != bytes(10)):
        return None
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(context.target).verify(
            plaintext[101:165], plaintext[:101]
        )
    except InvalidSignature:
        return None
    return {
        "valid": bool(flags & 1),
        "sealed": bool(flags & 2),
        "profile_generation": struct.unpack_from("<Q", plaintext, 90)[0],
        "saved_mask": plaintext[98],
        "applied_mask": plaintext[99],
    }


def wait_receipt(connection, context, seconds, verifier=verify_receipt, kind="receipt"):
    deadline = time.monotonic() + seconds
    in_frame = False
    escaped = False
    discard = False
    frame_bytes = bytearray()
    received = 0
    tx_done = None
    while time.monotonic() < deadline:
        connection.settimeout(max(0.001, deadline - time.monotonic()))
        try:
            chunk = connection.recv(512)
        except socket.timeout:
            break
        if not chunk:
            raise ValueError(f"Gateway disconnected before a signed mast {kind}; outcome unknown; do not replay automatically")
        received += len(chunk)
        if received > MAX_RX_BYTES:
            raise ValueError(f"Gateway RX stream limit exceeded without a signed mast {kind}")
        for byte in chunk:
            if byte == 0xc0:
                if in_frame and not escaped and not discard:
                    if frame_bytes in (b"\x06\xf8\x01", b"\x06\xf8\x00"):
                        tx_done = frame_bytes[-1]
                    elif frame_bytes[:1] == b"\x00":
                        status = verifier(frame_bytes[1:], context)
                        if status is not None:
                            return status
                in_frame = True
                escaped = False
                discard = False
                frame_bytes.clear()
            elif in_frame:
                if discard:
                    continue
                if escaped:
                    if byte not in (0xdc, 0xdd):
                        discard = True
                        continue
                    byte = 0xc0 if byte == 0xdc else 0xdb
                    escaped = False
                elif byte == 0xdb:
                    escaped = True
                    continue
                if len(frame_bytes) >= MAX_KISS_FRAME:
                    discard = True
                    continue
                frame_bytes.append(byte)
    result = {None: "no gateway TX result", 0: "gateway TX failed or unknown",
              1: "gateway TX completed"}[tx_done]
    raise ValueError(f"No signed mast {kind} before deadline ({result}); outcome unknown; do not replay automatically")


def role_names(mask):
    return [role for role, flag in ROLES.items() if mask & flag]


def status_document(reply):
    if not reply["valid"]:
        raise ValueError("Mast cannot read its saved profile; state unknown")
    return {
        "profile_generation": str(reply["profile_generation"]),
        "saved_roles": role_names(reply["saved_mask"]),
        "applied_roles": role_names(reply["applied_mask"]),
        "sealed": reply["sealed"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    create = sub.add_parser("init-key", help="generate a private operator seed outside this repository")
    create.add_argument("--key-file", required=True)
    send = sub.add_parser("send", help="send once and await a signed mast receipt; never auto-retry")
    status = sub.add_parser("status", help="read saved and applied roles over RF, even without mast WiFi")
    for command in (send, status):
        command.add_argument("--key-file", required=True)
        command.add_argument("--target", required=True, help="full management public key from mast dashboard")
        command.add_argument("--nonce", type=int, default=None, help="nonzero uint64; random by default")
        command.add_argument("--gateway", help="independent KISS radio host, not the target mast")
        command.add_argument("--port", type=int, default=8001)
        command.add_argument("--wait-seconds", type=int, default=180,
                             help="wait for signed mast reply (1..3600, default 180)")
        command.add_argument("--dry-run", action="store_true", help="print frame, do not connect")
    send.add_argument("--generation", type=int, required=True, help="strictly greater than the saved generation")
    send.add_argument("--roles", required=True, help="comma-separated names or none")
    args = parser.parse_args()
    try:
        if args.action == "init-key":
            init_key(args.key_file)
            return
        if not args.dry_run and (not args.gateway or not 1 <= args.port <= 65535):
            raise ValueError("Provide --gateway and a valid --port, or select --dry-run")
        if not 1 <= args.wait_seconds <= 3600:
            raise ValueError("TX result wait must be 1..3600 seconds")
        nonce = args.nonce if args.nonce is not None else secrets.randbelow((1 << 64) - 1) + 1
        seed, target = read_key(args.key_file), target_key(args.target)
        if args.action == "send":
            packet, context = frame_with_context(
                seed, target, args.generation, nonce, role_mask(args.roles)
            )
            print(f"Generation {args.generation}; nonce {nonce}; roles {args.roles}", flush=True)
        else:
            packet, context = query_with_context(seed, target, nonce)
            print(f"Query nonce {nonce}", flush=True)
        if args.dry_run:
            print(f"MeshCore flood packet ({len(packet)} bytes): {packet.hex()}")
        else:
            with socket.create_connection((args.gateway, args.port), timeout=5) as connection:
                connection.settimeout(5)
                connection.sendall(kiss(packet))
                reply = wait_receipt(
                    connection, context, args.wait_seconds,
                    verify_receipt if args.action == "send" else verify_status,
                    "receipt" if args.action == "send" else "status",
                )
            if args.action == "status":
                print(json.dumps(status_document(reply), sort_keys=True))
            elif reply == 3:
                raise ValueError("Mast signed a failure receipt; role update was not confirmed")
            elif reply == 2:
                print("Mast signed an already-applied receipt for this request.")
            else:
                print("Mast signed a success receipt for this request.")
    except (OSError, ValueError) as error:
        parser.exit(2, f"RF role request failed: {error}\n")


if __name__ == "__main__":
    main()
