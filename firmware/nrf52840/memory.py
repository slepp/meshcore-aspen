#!/usr/bin/env python3
"""Report the application image and the linker's actual RAM/heap boundaries."""

import argparse
import json
import pathlib
import subprocess
import struct


def measure(elf, size, nm):
    sections = subprocess.check_output([size, "-A", str(elf)], text=True)
    sizes = {}
    for line in sections.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0].startswith("."):
            sizes[fields[0]] = int(fields[1])
    symbols = {}
    for line in subprocess.check_output([nm, "--defined-only", str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    flash_limit = 0xED000 - 0x27000
    flash_bytes = sum(sizes.get(s, 0) for s in (".text", ".ARM.extab", ".ARM.exidx", ".data", ".svc_data", ".fs_data"))
    if flash_bytes > flash_limit:
        raise ValueError(f"Image exceeds the nRF application flash region: {flash_bytes} > {flash_limit}")
    storage = {}
    binary = elf.with_suffix(".bin")
    if binary.exists():
        image = binary.read_bytes()
        for role, symbol in (
            ("session", "_ZN6onchip10BotSession12StorageBytesE"),
            ("worker", "_ZN6onchip9BotWorker12StorageBytesE"),
            ("command_bot", "_ZN6onchip10CommandBot12StorageBytesE"),
        ):
            address = symbols.get(symbol)
            if address is not None:
                offset = address - 0x27000
                if offset < 0 or offset + 4 > len(image):
                    raise ValueError(f"{role} storage constant is outside the application image")
                storage[role] = struct.unpack_from("<I", image, offset)[0]
    return {
        "elf": str(elf),
        "application_flash_limit_bytes": flash_limit,
        "application_flash_bytes": flash_bytes,
        "application_ram_region_bytes": 0x40000 - 0x6000,
        "static_ram_bytes": sum(sizes.get(s, 0) for s in (".data", ".bss", ".svc_data", ".fs_data")),
        "linker_heap_capacity_bytes": symbols["__HeapLimit"] - symbols["__HeapBase"],
        "main_stack_reserved_bytes": symbols["__StackTop"] - symbols["__StackLimit"],
        "runtime_heap_free_bytes": None,
        "runtime_heap_note": "Requires mem on the physical target after setup; linker capacity is not free heap.",
        "runtime_role_storage_bytes": storage,
        "binary_bytes": elf.with_suffix(".bin").stat().st_size if elf.with_suffix(".bin").exists() else None,
        "uf2_bytes": elf.with_suffix(".uf2").stat().st_size if elf.with_suffix(".uf2").exists() else None,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("elf", type=pathlib.Path)
    args = parser.parse_args()
    print(json.dumps(measure(args.elf, args.size, args.nm), indent=2))
