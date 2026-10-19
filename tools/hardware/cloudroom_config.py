#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Encode a private CloudRoom profile for the generic Aspen application."""
import hashlib
import ipaddress
import json
import re
import struct
from cryptography import x509

LIMIT = 32768
RECORD_SIZE = 9296
RECORD_SIZE_V2 = 9304
ALIAS = struct.Struct("<128s16s4097s257s65s33s32s")
ALIAS_V2 = struct.Struct("<128s16s4097s257s65s33s32sBBBB")
FIELDS = {"host", "address", "ca", "token", "id", "name", "public_key"}
# Schema 2 aliases may add these; every one has a default.
OPTIONAL_V2 = {"role", "ca_mode", "advert_once", "converted"}


def unique(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("Cloud room JSON contains a duplicate field")
        result[name] = value
    return result


def encode(content):
    """Return the record for a profile: version 1 whenever it can express the
    settings (older firmware also loads it), otherwise version 2."""
    if not isinstance(content, (str, bytes, bytearray)) or len(content) > LIMIT:
        raise ValueError("Cloud room JSON exceeds 32768 bytes or has the wrong input type")
    try:
        profile = json.loads(content, object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("Cloud room JSON is malformed") from error
    if (type(profile) is not dict or set(profile) != {"schema", "enabled", "aliases"} or
            type(profile["schema"]) is not int or profile["schema"] not in (1, 2) or
            type(profile["enabled"]) is not bool or type(profile["aliases"]) is not list):
        raise ValueError("Cloud room profile requires schema=1 or 2, boolean enabled and an alias list")
    schema = profile["schema"]
    if not (1 if schema == 1 else 0) <= len(profile["aliases"]) <= 2:
        raise ValueError("Cloud room profile requires one or two aliases (schema 2 also allows none)")
    entries, identifiers, keys = [], set(), set()
    for alias in profile["aliases"]:
        allowed = FIELDS | (OPTIONAL_V2 if schema == 2 else set())
        if type(alias) is not dict or not FIELDS <= set(alias) <= allowed:
            raise ValueError("Cloud room alias requires exactly the documented fields")
        role = alias.get("role", "frontend")
        mode = alias.get("ca_mode", "pem")
        advert = alias.get("advert_once", False)
        converted = alias.get("converted", False)
        if (role not in ("frontend", "owner") or mode not in ("system", "pem") or
                type(advert) is not bool or type(converted) is not bool or (converted and role != "owner")):
            raise ValueError("Cloud room role, ca_mode, advert_once or converted is invalid")
        system = mode == "system"
        if system and alias["ca"] != "":
            raise ValueError("Cloud room ca must be empty when ca_mode is system")
        values = {}
        for name, capacity in (("host", 128), ("address", 16), ("ca", 4097),
                               ("token", 257), ("id", 65), ("name", 33)):
            value = alias[name]
            minimum = int(name not in ("address",) and not (name == "ca" and system))
            if (type(value) is not str or not (minimum <= len(value) < capacity) or
                    any((ord(c) < 32 or ord(c) > 126) and
                        not (name == "ca" and c in "\r\n") for c in value)):
                raise ValueError(f"Cloud room {name} length or ASCII format invalid")
            values[name] = value.encode("ascii")
        host = alias["host"]
        if any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
               for label in host.split(".")):
            raise ValueError("Cloud room host must be a DNS hostname without scheme, port or path")
        if alias["address"]:
            try:
                ipaddress.IPv4Address(alias["address"])
            except ipaddress.AddressValueError as error:
                raise ValueError("Cloud room address must be empty or an IPv4 address") from error
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", alias["id"]):
            raise ValueError("Cloud room id requires 1..64 letters, digits, underscores or hyphens")
        if " " in alias["token"]:
            raise ValueError("Cloud room token must not contain whitespace")
        if not system:
            pem = alias["ca"]
            blocks = re.findall(r"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+-----END CERTIFICATE-----", pem)
            if not blocks or "".join(pem.split()) != "".join("".join(blocks).split()):
                raise ValueError("Cloud room ca must contain only PEM CA certificates")
            try:
                for block in blocks:
                    x509.load_pem_x509_certificate(block.encode("ascii"))
            except ValueError as error:
                raise ValueError("Cloud room CA certificate could not be parsed") from error
        public = alias["public_key"]
        if (type(public) is not str or not re.fullmatch(r"[0-9a-f]{64}", public) or
                public in ("0" * 64, "f" * 64)):
            raise ValueError("Cloud room public_key requires a full lowercase verification public key")
        identity = (host.lower(), alias["id"])
        if identity in identifiers or public in keys:
            raise ValueError("Cloud room aliases must have distinct endpoint/id pairs and public keys")
        identifiers.add(identity)
        keys.add(public)
        flags = int(advert) | (2 if converted else 0)
        entries.append((values, bytes.fromhex(public), int(role == "owner"), int(system), flags))
    # Version 1 only when no alias needs a role, system roots or a flag.
    wide = (not entries or any(role or system or flags for _, _, role, system, flags in entries))
    if wide and any(key[0] in (0, 255) for _, key, *_ in entries):
        raise ValueError("Cloud room public_key must not start with 00 or ff")
    count = len(entries)
    result = bytearray((b"CRC\2" if wide else b"CRC\1") + bytes((profile["enabled"], count, 0, 0)))
    for values, key, role, system, flags in entries:
        packed = (values["host"], values["address"], values["ca"], values["token"],
                  values["id"], values["name"], key)
        result.extend(ALIAS_V2.pack(*packed, role, 0 if system else 1, flags, 0) if wide
                      else ALIAS.pack(*packed))
    result.extend(bytes((ALIAS_V2.size if wide else ALIAS.size) * (2 - count)))
    result.extend(hashlib.sha256(result).digest())
    assert len(result) == (RECORD_SIZE_V2 if wide else RECORD_SIZE)
    return bytes(result)
