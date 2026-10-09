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
ALIAS = struct.Struct("<128s16s4097s257s65s33s32s")
FIELDS = {"host", "address", "ca", "token", "id", "name", "public_key"}


def unique(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("Cloud room JSON contains a duplicate field")
        result[name] = value
    return result


def encode(content):
    if not isinstance(content, (str, bytes, bytearray)) or len(content) > LIMIT:
        raise ValueError("Cloud room JSON exceeds 32768 bytes or has the wrong input type")
    try:
        profile = json.loads(content, object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("Cloud room JSON is malformed") from error
    if (type(profile) is not dict or set(profile) != {"schema", "enabled", "aliases"} or
            type(profile["schema"]) is not int or profile["schema"] != 1 or
            type(profile["enabled"]) is not bool or type(profile["aliases"]) is not list or
            not 1 <= len(profile["aliases"]) <= 2):
        raise ValueError("Cloud room profile requires schema=1, boolean enabled and one or two aliases")
    result = bytearray(b"CRC\1" + bytes((profile["enabled"], len(profile["aliases"]), 0, 0)))
    identifiers, keys = set(), set()
    for alias in profile["aliases"]:
        if type(alias) is not dict or set(alias) != FIELDS:
            raise ValueError("Cloud room alias requires exactly the documented fields")
        values = {}
        for name, capacity in (("host", 128), ("address", 16), ("ca", 4097),
                               ("token", 257), ("id", 65), ("name", 33)):
            value = alias[name]
            if (type(value) is not str or not (int(name != "address") <= len(value) < capacity) or
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
        result.extend(ALIAS.pack(values["host"], values["address"], values["ca"],
                                 values["token"], values["id"], values["name"], bytes.fromhex(public)))
    result.extend(bytes(ALIAS.size * (2 - len(profile["aliases"]))))
    result.extend(hashlib.sha256(result).digest())
    assert len(result) == RECORD_SIZE
    return bytes(result)
