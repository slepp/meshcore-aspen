#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inspect the selected Linux x86_64 release ELF without executing it.

Uses the ELF64 headers/dynamic table and GNU version-needs records. Build-time
results are also compared with GNU readelf; verification needs only Python.
https://gabi.xinuos.com/elf/02-eheader.html
"""
import re
import struct


def inspect_elf(path):
    data = path.read_bytes()

    def unpack(fmt, offset, content=data):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(content):
            raise ValueError("Truncated ELF ABI structure")
        return struct.unpack_from(fmt, content, offset)

    header = unpack("<16sHHIQQQIHHHHHH", 0)
    if (header[0][:7] != b"\x7fELF\x02\x01\x01" or header[1] not in (2, 3) or
            header[2] != 62 or header[3] != 1 or header[8] != 64 or
            header[9] != 56 or header[11] != 64 or not header[12]):
        raise ValueError("Expected sectioned ELF64 little-endian x86_64 executable")

    def region(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            raise ValueError("ELF ABI region outside file")
        return data[offset:offset + size]

    def string(content, offset):
        end = content.find(b"\0", offset)
        if offset < 0 or end < offset:
            raise ValueError("Invalid ELF ABI string")
        return content[offset:end].decode("ascii")

    interpreter = None
    for i in range(header[10]):
        program = unpack("<IIQQQQQQ", header[5] + i * header[9])
        if program[0] == 3:  # PT_INTERP
            if interpreter is not None:
                raise ValueError("Duplicate ELF interpreter")
            interpreter = string(region(program[2], program[5]), 0)

    sections = [unpack("<IIQQQQIIQQ", header[6] + i * header[11]) for i in range(header[12])]

    def section_bytes(section):
        return region(section[4], section[5])

    def strings(section):
        if section[6] >= len(sections) or sections[section[6]][1] != 3:
            raise ValueError("ELF ABI section lacks linked string table")
        return section_bytes(sections[section[6]])

    needed, versions = set(), {}
    dynamic_count = 0
    for section in sections:
        if section[1] == 6:  # SHT_DYNAMIC / DT_NEEDED
            dynamic_count += 1
            content, names = section_bytes(section), strings(section)
            if section[9] != 16 or len(content) % 16:
                raise ValueError("Invalid ELF dynamic table")
            terminated = False
            for offset in range(0, len(content), 16):
                tag, value = unpack("<qQ", offset, content)
                if tag == 0:
                    terminated = True
                    break
                if tag == 1:
                    needed.add(string(names, value))
            if not terminated:
                raise ValueError("Unterminated ELF dynamic table")
        elif section[1] == 0x6ffffffe:  # SHT_GNU_verneed
            content, names = section_bytes(section), strings(section)
            offset = 0
            while offset < len(content):
                version, count, filename, aux, following = unpack("<HHIII", offset, content)
                if version != 1 or not count or aux < 16:
                    raise ValueError("Invalid ELF version-needs record")
                string(names, filename)
                entry = offset + aux
                for i in range(count):
                    _, _, _, name, next_aux = unpack("<IHHII", entry, content)
                    match = re.fullmatch(r"(GLIBCXX|GLIBC|CXXABI|OPENSSL)_([0-9]+(?:\.[0-9]+)+)", string(names, name))
                    if match:
                        family, value = match.groups()
                        if family not in versions or tuple(map(int, value.split("."))) > tuple(map(int, versions[family].split("."))):
                            versions[family] = value
                    if i + 1 < count:
                        if next_aux < 16:
                            raise ValueError("Invalid ELF version-needs auxiliary chain")
                        entry += next_aux
                if not following:
                    break
                if following < 16:
                    raise ValueError("Invalid ELF version-needs chain")
                offset += following
    if dynamic_count > 1 or (needed and interpreter is None):
        raise ValueError("Invalid executable ELF dynamic layout")
    return {"architecture": "x86_64", "interpreter": interpreter,
            "needed_sonames": sorted(needed), "required_symbol_versions": versions,
            "distribution_qualified": False}
