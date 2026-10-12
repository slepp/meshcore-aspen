#!/usr/bin/env python3
"""Copy a frozen companion role to a new private directory without generating keys."""
import argparse
import base64
import ctypes
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat

LIMIT = 16 * 1024 * 1024


def fail(message):
    raise ValueError(message)


def private(path, directory=False):
    info = path.lstat()
    valid = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
    if not valid or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        fail(f"{path}: expected an owned private {'directory' if directory else 'file'}")
    if not directory and (info.st_nlink != 1 or info.st_size > LIMIT):
        fail(f"{path}: linked or oversized state file")
    return info


def document(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                fail(f"duplicate JSON field: {key}")
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique,
                      parse_constant=lambda _: fail("nonfinite JSON number"))


def binary(value, length=None):
    if isinstance(value, str):
        data = base64.b64decode(value, validate=True)
    elif isinstance(value, list) and all(type(n) is int and 0 <= n <= 255 for n in value):
        data = bytes(value)
    elif value is None:
        data = b""
    else:
        fail("invalid JSON byte sequence")
    if length is not None and len(data) != length:
        fail(f"expected {length}-byte sequence")
    return data


def public(key):
    sodium = ctypes.CDLL("libsodium.so.23")
    out = ctypes.create_string_buffer(32)
    if len(key) == 32:
        secret = ctypes.create_string_buffer(64)
        rc = sodium.crypto_sign_seed_keypair(out, secret, ctypes.c_char_p(key))
    elif len(key) == 64 and key[0] & 7 == 0 and key[31] & 0xC0 == 0x40:
        rc = sodium.crypto_scalarmult_ed25519_base_noclamp(out, ctypes.c_char_p(key[:32]))
    else:
        fail("identity must be a 32-byte seed or native 64-byte scalar-prefix key")
    if rc != 0:
        fail("identity public-key derivation failed")
    return out.raw


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        fail(f"{name}: expected integer in {low}..{high}")
    return value


def path_size(encoded):
    integer(encoded, 0, 255, "path length")
    if encoded == 255:
        return 0
    size = ((encoded >> 6) + 1) * (encoded & 63)
    if encoded > 191 or size > 64:
        fail("invalid ordinary route path")
    return size


def validate(doc, key):
    if not isinstance(doc, dict) or doc.get("Version") != 1:
        fail("companion document must have Version 1")
    if binary(doc.get("PublicKey"), 32) != public(key):
        fail("companion document public key does not match authoritative identity")
    name = doc.get("Name")
    if not isinstance(name, str) or not 1 <= len(name.encode()) <= 31 or "\0" in name:
        fail("invalid companion advert name")
    contacts = doc.get("Contacts") or []
    if not isinstance(contacts, list) or len(contacts) > 350:
        fail("contact capacity exceeds 350")
    keys = set()
    for contact in contacts:
        contact_key = binary(contact.get("PublicKey"), 32)
        if contact_key in keys:
            fail("duplicate permanent contact key")
        keys.add(contact_key)
        binary(contact.get("OutPath"), 64)
        path_size(contact.get("OutPathLen"))
        integer(contact.get("Type"), 0, 4, "contact type")
        integer(contact.get("Flags"), 0, 255, "contact flags")
        binary(contact.get("Advert"))
        binary(contact.get("HeardPath"))
    channels = doc.get("Channels")
    if not isinstance(channels, list) or len(channels) != 40:
        fail("companion requires exactly 40 channel slots")
    for channel in channels:
        if channel is not None:
            binary(channel.get("PSK"), 16)
            if not isinstance(channel.get("Name"), str) or "\0" in channel["Name"]:
                fail("invalid channel name")
    sequence = integer(doc.get("Sequence"), 0, 2**64 - 1, "Sequence")
    messages = doc.get("Messages") or []
    if not isinstance(messages, list) or len(messages) > 256:
        fail("message capacity exceeds 256")
    previous = 0
    for message in messages:
        previous = integer(message.get("Sequence"), previous + 1, sequence, "message Sequence")
        frame = binary(message.get("Frame"))
        if not frame or len(frame) > 176 or frame[0] not in (16, 17, 27):
            fail("unsupported journal frame")
        minimum = {16: 16, 17: 11, 27: 9}[frame[0]]
        if len(frame) < minimum or (frame[0] == 27 and (frame[8] > 167 or len(frame) != 9 + frame[8])):
            fail("malformed journal frame")
        binary(message.get("Digest"), 32)
    if doc.get("Retention", "") not in ("", "native-queue", "durable-replay"):
        fail("unknown companion retention profile")
    prefs = doc.get("Preferences")
    if not isinstance(prefs, dict) or prefs.get("version") != 1:
        fail("missing versioned companion preferences")
    integer(prefs.get("path_hash_mode"), 0, 2, "path_hash_mode")
    for field in ("rxdelay", "airtime_factor", "txdelay", "direct_txdelay"):
        value = prefs.get(field)
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            fail(f"{field}: expected a nonnegative finite number")
    return {"public_key": public(key).hex(), "name": name, "contacts": len(contacts),
            "channels": 40, "messages": len(messages), "sequence": sequence,
            "retention": doc.get("Retention") or "durable-replay",
            "path_hash_mode": prefs["path_hash_mode"]}


def inventory(source):
    private(source, True)
    result = {}
    total = 0
    for path in sorted(source.iterdir()):
        if path.name in (".lock", "base-service.lock", "migration-base.json"):
            private(path)
            continue
        private(path)
        data = path.read_bytes()
        total += len(data)
        if total > 64 * 1024 * 1024:
            fail("companion snapshot exceeds 64 MiB")
        if path.name.endswith(".pending") or path.name.endswith(".lock"):
            fail("snapshot contains pending state or a worker lock; freeze/recover it first")
        result[path.name] = data
    return result


def authority(files):
    if "identity-state.json" in files:
        envelope = document(files["identity-state.json"])
        if not isinstance(envelope, dict) or envelope.get("version") != 1 or envelope.get("document") != "companion.json":
            fail("invalid authoritative companion envelope")
        if envelope.get("pending_identity") is not None:
            fail("companion has a pending identity transition; complete it before freezing")
        key = binary(envelope.get("identity"), 64)
        doc = envelope.get("state")
        filename = "identity-state.json"
    else:
        filename = "companion.json"
        if "companion.json" not in files:
            fail("frozen role has no companion document")
        doc = document(files["companion.json"])
        key = files.get("identity.expanded", files.get("identity.seed"))
        if key is None:
            fail("frozen role has no authoritative identity; migration never generates one")
    return filename, key, doc


def hashes(files):
    return {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}


def transfer(source, destination, direction):
    source, destination = Path(source), Path(destination)
    private(source, True)
    private(destination.parent, True)
    resolved_source = source.resolve()
    resolved_parent = destination.parent.resolve()
    if resolved_parent == resolved_source or resolved_source in resolved_parent.parents:
        fail("destination must not be inside the frozen source directory")
    if destination.exists() or destination.is_symlink():
        fail("destination must be new; existing state is never overwritten")
    source_locks = []
    staging = destination.with_name(destination.name + ".staging")
    created = False
    try:
        for name in (".lock", "base-service.lock"):
            lock_path = source / name
            if lock_path.exists():
                private(lock_path)
                fd = os.open(lock_path, os.O_RDONLY | os.O_NOFOLLOW)
                source_locks.append(fd)
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        files = inventory(source)
        filename, key, doc = authority(files)
        if filename == "identity-state.json" and doc is None:
            summary = {"public_key": public(key).hex(), "name": None, "contacts": 0,
                       "channels": 0, "messages": 0, "sequence": 0,
                       "retention": None, "path_hash_mode": None, "reset_marker": True}
        else:
            summary = validate(doc, key)
        if staging.exists() or staging.is_symlink():
            fail("staging directory already exists")
        staging.mkdir(mode=0o700)
        created = True
        for name, data in files.items():
            with open(staging / name, "xb") as stream:
                os.chmod(staging / name, 0o600)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
        manifest = {"version": 1, "direction": direction, "authority": filename,
                    "files": hashes(files), "summary": summary,
                    "validation": {"state_validated": True, "runtime_checked": False}}
        with open(staging / "migration-base.json", "x") as stream:
            os.chmod(staging / "migration-base.json", 0o600)
            json.dump(manifest, stream, sort_keys=True, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        if hashes(inventory(source)) != hashes(files):
            fail("source changed while staging; use a frozen snapshot")
        directory = os.open(staging, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        os.rename(staging, destination)
        parent = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent)
        finally:
            os.close(parent)
        return manifest
    except Exception:
        if created and staging.is_dir() and not staging.is_symlink():
            shutil.rmtree(staging)
        raise
    finally:
        for fd in source_locks:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direction", choices=("stage", "rollback"))
    parser.add_argument("--source", required=True, help="frozen companion role directory")
    parser.add_argument("--destination", required=True, help="new private directory under an owned private parent")
    args = parser.parse_args()
    try:
        result = transfer(args.source, args.destination, args.direction)
    except (ValueError, OSError, TypeError, KeyError) as error:
        parser.exit(1, f"Base migration rejected: {error}\n")
    print(json.dumps(result["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
