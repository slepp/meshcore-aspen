#!/usr/bin/env python3
"""Report ELF allocation, retained map inputs, symbols and ESP partitions."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def output(tool, *args):
    return subprocess.check_output([tool, *map(str, args)], text=True)


def sections(elf, prefix):
    rows = {}
    lines = output(prefix + "objdump", "-h", elf).splitlines()
    for index, line in enumerate(lines[:-1]):
        match = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+", line)
        if match and "ALLOC" in lines[index + 1]:
            name, size, address = match.groups()
            rows[name] = {"bytes": int(size, 16), "address": int(address, 16),
                          "load": "LOAD" in lines[index + 1]}
    return rows


def category(origin):
    if "OnchipLua" in origin:
        return "lua"
    if "OnchipWamr" in origin or "BotWasm.cpp" in origin:
        return "wasm"
    if re.search(r"/(Repeater|Room|Companion|Observer)\.cpp", origin):
        return "role_" + re.search(r"/(Repeater|Room|Companion|Observer)\.cpp", origin)[1].lower()
    if re.search(r"/(BotStore|BotTimers|BotReminders|BotJournal|ScopedFS|RoleIdentity|RoleProfile)\.cpp", origin):
        return "storage"
    if re.search(r"/(Mast\w+|BotHttps|BotNetworkConfig|Telemetry\w*|RadioDashboard)\.cpp", origin):
        return "wifi_tls_web_admin"
    if re.search(r"lib(wifi|net80211|wpa|pp|phy|esp_wifi|lwip|mbedtls|esp_http\w*|http_parser|mqtt|tcpip_adapter|esp_netif)\.a", origin):
        return "wifi_tls_web_admin"
    if "/onchip/" in origin and re.search(r"/(Bot\w+|CommandBot)\.cpp", origin):
        return "bot_native_apis"
    if ("RadioLib" in origin or "Crypto" in origin or "ed25519" in origin or
            re.search(r"/src/(?!helpers/(esp32|sensors))", origin) or
            re.search(r"/(Runtime|LocalRadio|Management|Lifecycle|Clock|WifiKissMultiplexer|KissModem)\.cpp", origin)):
        return "mesh_radio_crypto_shared"
    return "platform_shared_unattributed"


def map_inputs(path, allocated):
    groups = defaultdict(lambda: defaultdict(int))
    events = defaultdict(list)
    section = None
    pending = None
    active = False
    for line in path.read_text(errors="replace").splitlines():
        if line == "Linker script and memory map":
            active = True
        if not active or line.startswith("Cross Reference Table"):
            continue
        match = re.match(r"^(\.\S+)\s*(?:0x[0-9a-f]+)?", line)
        if match:
            section = match[1] if match[1] in allocated else None
            pending = None
        if section is None:
            continue
        match = re.match(r"^ (\.\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$", line)
        if match:
            _, address, size, origin = match.groups()
        else:
            name = re.match(r"^ (\.\S+)\s*$", line)
            if name:
                pending = name[1]
                continue
            match = re.match(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$", line)
            if not match or not pending:
                continue
            address, size, origin = match.groups()
        pending = None
        if origin.startswith(("0x", "*")) or " = " in origin:
            continue
        start = max(int(address, 16), allocated[section]["address"])
        end = min(int(address, 16) + int(size, 16),
                  allocated[section]["address"] + allocated[section]["bytes"])
        if start < end:
            group = category(origin)
            events[section].extend(((start, 1, group), (end, -1, group)))
    # Merged string/constant input ranges can overlap. Charge each output byte
    # once; conflicting categories go to shared rather than either component.
    for section, changes in events.items():
        live = defaultdict(int)
        previous = None
        for position, delta, group in sorted(changes):
            if previous is not None and position > previous:
                owners = [key for key, count in live.items() if count]
                if owners:
                    owner = owners[0] if len(owners) == 1 else "merged_shared"
                    groups[owner][section] += position - previous
            live[group] += delta
            previous = position
    return {group: dict(values) for group, values in sorted(groups.items())}


def partitions(path):
    rows = []
    data = path.read_bytes()
    for offset in range(0, len(data) - 31, 32):
        magic, kind, subtype, address, size, name, flags = struct.unpack("<HBBII16sI", data[offset:offset + 32])
        if magic != 0x50AA:
            break
        rows.append({"name": name.rstrip(b"\0").decode(), "type": kind,
                     "subtype": subtype, "offset": address, "bytes": size, "flags": flags})
    return rows


def type_sizes(elf, prefix):
    wanted = {"RepeaterStorage", "RoomStorage", "CompanionStorage", "Storage", "Core",
              "Impl", "Control", "BotManifest", "BotEvent", "BotIoRequest", "BotIoResult",
              "Workspace", "AnchorCache", "Record", "Journal", "Files", "Cache", "Plan",
              "RadioDashboard", "WifiKissMultiplexer", "CompanionSessions", "Observer"}
    stack = {}
    found = {}
    current = None
    process = subprocess.Popen([prefix + "readelf", "--debug-dump=info", str(elf)],
                               stdout=subprocess.PIPE, text=True)
    def finish():
        if current and current.get("name") in wanted and "bytes" in current:
            parents = [stack[depth].get("name", "") for depth in sorted(stack)
                       if depth < current["depth"] and stack[depth]["tag"] in
                       ("namespace", "class_type", "structure_type")]
            name = "::".join(p for p in parents + [current["name"]] if p)
            found[name] = current["bytes"]
    for line in process.stdout:
        die = re.match(r"\s*<(\d+)><[0-9a-f]+>:.*DW_TAG_(\w+)", line)
        if die:
            finish()
            depth, tag = die.groups()
            depth = int(depth)
            for key in list(stack):
                if key >= depth:
                    del stack[key]
            current = {"depth": depth, "tag": tag}
            stack[depth] = current
        elif re.match(r"\s*<\d+><[0-9a-f]+>: Abbrev Number: 0", line):
            finish()
            current = None
        elif current:
            name = re.search(r"DW_AT_name\s*:\s*(?:\(indirect [^)]*\):\s*)?(.*)", line)
            size = re.search(r"DW_AT_byte_size\s*:\s*(0x[0-9a-f]+|\d+)", line)
            if name:
                current["name"] = name[1].strip()
            if size:
                current["bytes"] = int(size[1], 0)
    finish()
    if process.wait():
        raise ValueError("ELF debug type inspection failed")
    return dict(sorted(found.items()))


def inspect(elf, prefix, map_path=None, binary=None, partition_path=None):
    rows = sections(elf, prefix)
    report = {
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "sections": rows,
        "allocated_load_bytes": sum(r["bytes"] for r in rows.values() if r["load"]),
        "static_dram_data_bytes": rows.get(".dram0.data", rows.get(".data", {})).get("bytes", 0),
        "static_dram_bss_bytes": rows.get(".dram0.bss", rows.get(".bss", {})).get("bytes", 0),
    }
    if binary:
        report["binary_bytes"] = binary.stat().st_size
        report["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    if map_path:
        report["map_input_estimates"] = map_inputs(map_path, rows)
        used = defaultdict(int)
        for group in report["map_input_estimates"].values():
            for section, size in group.items():
                used[section] += size
        report["map_alignment_and_unattributed_bytes"] = {
            section: row["bytes"] - used[section] for section, row in rows.items()
        }
        if any(n < 0 for n in report["map_alignment_and_unattributed_bytes"].values()):
            raise ValueError("Map attribution exceeds an ELF section")
    symbol_rows = output(prefix + "nm", "-S", "--size-sort", "--defined-only", "-C", elf).splitlines()
    report["largest_symbols"] = []
    for line in reversed(symbol_rows):
        match = re.match(r"([0-9a-f]+)\s+([0-9a-f]+)\s+(\S)\s+(.+)", line)
        if match:
            address, size, kind, name = match.groups()
            report["largest_symbols"].append({"name": name, "bytes": int(size, 16),
                                              "type": kind, "address": int(address, 16)})
        if len(report["largest_symbols"]) == 20:
            break
    if partition_path:
        report["partitions"] = partitions(partition_path)
    report["debug_type_size_bytes"] = type_sizes(elf, prefix)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--map", type=Path)
    parser.add_argument("--bin", type=Path)
    parser.add_argument("--partitions", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.elf, args.prefix, args.map, args.bin, args.partitions), indent=2))


if __name__ == "__main__":
    main()
