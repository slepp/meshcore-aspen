#!/usr/bin/env python3
"""Compare native capture fixtures with the pinned public capture formatter."""
import ast
from datetime import datetime, timezone
import hashlib
import json
import logging
from pathlib import Path
import sys
from typing import Any, Dict, Optional
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[3]
REVISION = "375c180d5a819e5e9262f7b989586d7847abf9c8"
SOURCE_SHA256 = "de5d00d32e89aacb95cb69c8a84c591424fc01c0a1b33e3e1d8d79582d6175a3"
SOURCE = ROOT / ".tmp/observer-reference/capture.py"
URL = f"https://raw.githubusercontent.com/agessaman/meshcore-packet-capture/{REVISION}/src/meshcore_packet_capture/packet_capture.py"
METHODS = {"format_packet_data", "calculate_packet_hash", "_decode_packed_path_length"}


def main():
    directory = Path(sys.argv[1])
    SOURCE.parent.mkdir(parents=True, exist_ok=True)
    if not SOURCE.exists():
        with urlopen(URL, timeout=30) as response:
            content = response.read()
        if hashlib.sha256(content).hexdigest() != SOURCE_SHA256:
            raise ValueError("Pinned capture source checksum changed")
        SOURCE.write_bytes(content)
    content = SOURCE.read_bytes()
    if hashlib.sha256(content).hexdigest() != SOURCE_SHA256:
        raise ValueError("Cached capture source checksum changed")
    tree = ast.parse(content)
    owner = next(node for node in tree.body if isinstance(node, ast.ClassDef)
                 and any(isinstance(method, ast.FunctionDef)
                         and method.name == "format_packet_data" for method in node.body))
    methods = [node for node in owner.body
               if isinstance(node, ast.FunctionDef) and node.name in METHODS]
    module = ast.fix_missing_locations(ast.Module(body=[
        ast.ClassDef(name="CaptureContract", bases=[], keywords=[],
                     body=methods, decorator_list=[])], type_ignores=[]))
    namespace = dict(hashlib=hashlib, datetime=datetime, timezone=timezone,
                     Optional=Optional, Dict=Dict, Any=Any)
    exec(compile(module, str(SOURCE), "exec"), namespace)
    capture = namespace["CaptureContract"]()
    capture.debug = capture.decode_payloads = False
    capture.channel_key_store = None
    capture.logger = logging.getLogger("upstream-observer-contract")
    for name in ("capture.json", "capture-direct.json", "capture-zero.json"):
        packet = json.loads((directory / name).read_text())
        capture.device_name = packet["origin"]
        capture.device_public_key = packet["origin_id"]
        raw = bytes.fromhex(packet["raw"])
        offset = 5 if raw[0] & 3 in (0, 3) else 1
        packed = raw[offset]
        width, count = (packed >> 6) + 1, packed & 63
        path = raw[offset + 1:offset + 1 + width * count]
        # Feed the formatter the decoded envelope represented by each native
        # vector. Native/Go parser tests independently check these wire offsets.
        capture.decode_and_publish_message = lambda _: dict(
            route_type=("TRANSPORT_FLOOD", "FLOOD", "DIRECT", "TRANSPORT_DIRECT")[raw[0] & 3],
            payload_type="GRP_TXT", payload_type_value=5,
            path=[path[i:i + width].hex() for i in range(0, len(path), width)],
            path_byte_len=len(path))
        expected = capture.format_packet_data(packet["raw"], {
            "snr": float(packet["SNR"]), "rssi": int(packet["RSSI"])})
        for field in ("origin", "origin_id", "type", "direction", "len",
                      "packet_type", "route", "payload_len", "raw", "SNR", "RSSI",
                      "hash", "path"):
            if packet.get(field) != expected.get(field):
                raise ValueError(f"{name}: upstream {field} differs")
    print(f"PASS capture formatter {REVISION}: transport T, direct CSV/empty path, typed fields, quarter-dB SNR and packet hash")


if __name__ == "__main__":
    main()
