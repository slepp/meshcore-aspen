#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Named Lua sources in one atomically deployed, shared-namespace source set."""
import re

PREFIX = b"--@meshcore-sources/1\n"
# Largest firmware profile. Devices advertise their own limits in
# `source api sources`; firmware without them accepts 4096 bytes and 8 sources.
LIMIT = 32768
ENTRIES = 16
NAME = re.compile(r"[a-z][a-z0-9_-]{0,23}\Z")


def advertised_limits(capability):
    count = re.search(r"\bcount=([1-9][0-9]*)\b", capability)
    envelope = re.search(r"\benvelope=([1-9][0-9]*)\b", capability)
    return {"limit": int(envelope[1]) if envelope else 4096,
            "entries": int(count[1]) if count else 8}


def decode(source, limit=LIMIT, entries=ENTRIES):
    if not source or len(source) > limit or b"\0" in source or source.startswith(b"\x1b"):
        raise ValueError(f"Lua sources require 1..{limit} bytes of source text without NUL")
    if not source.startswith(PREFIX):
        if source.startswith(b"--@meshcore-sources"):
            raise ValueError("Unsupported Lua source-set version")
        return {"main": source}
    parts = {}
    offset = len(PREFIX)
    while offset < len(source):
        end = source.find(b"\n", offset)
        if end < 0 or end - offset > 64:
            raise ValueError("Lua source header is missing or too long")
        builtin = re.fullmatch(rb"--@builtin ([a-z][a-z0-9_-]{0,23})", source[offset:end])
        if builtin:
            name = builtin[1].decode("ascii")
            if name in parts or None in parts.values() or len(parts) == entries:
                raise ValueError(f"Duplicate builtin/source name or {entries}-source limit exceeded")
            parts[name] = None
            offset = end + 1
            continue
        match = re.fullmatch(rb"--@source ([a-z][a-z0-9_-]{0,23}) ([0-9]+)", source[offset:end])
        if not match:
            raise ValueError("Invalid Lua source name or byte length")
        name = match[1].decode("ascii")
        size = int(match[2])
        offset = end + 1
        if not size or offset + size >= len(source) or source[offset + size] != 10:
            raise ValueError("Lua source byte length or separator mismatch")
        if name in parts or len(parts) == entries:
            raise ValueError(f"Duplicate Lua source name or {entries}-source limit exceeded")
        text = source[offset:offset + size]
        if text.startswith(b"\x1b"):
            raise ValueError("Lua bytecode is not a source")
        parts[name] = text
        offset += size + 1
    if not parts:
        raise ValueError("Lua source set is empty")
    return parts


def encode(parts, limit=LIMIT, entries=ENTRIES):
    if not parts or len(parts) > entries:
        raise ValueError(f"Lua source set requires 1..{entries} named sources")
    for name, text in parts.items():
        if not NAME.fullmatch(name) or (text is not None and
                                      (not text or b"\0" in text or text.startswith(b"\x1b"))):
            raise ValueError("Invalid Lua source name or text")
    if sum(text is None for text in parts.values()) > 1:
        raise ValueError("Lua source set may reference bundled commands once")
    source = PREFIX + b"".join(
        b"--@builtin " + name.encode("ascii") + b"\n" if text is None else
        b"--@source " + name.encode("ascii") + b" " + str(len(text)).encode("ascii") +
        b"\n" + text + b"\n" for name, text in parts.items()
    )
    if len(source) > limit:
        raise ValueError(f"Combined Lua sources use {len(source)} bytes; the device limit is {limit}")
    return source
