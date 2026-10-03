#!/usr/bin/env python3
"""Read framed transport diagnostics from a queued USB companion."""

import argparse
import json
import struct
import time

import serial


def response(port, pending):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        pending.extend(port.read(176))
        while len(pending) >= 3:
            length = int.from_bytes(pending[1:3], "little")
            if pending[0] != ord(">") or not 1 <= length <= 176:
                del pending[0]
                continue
            if len(pending) < length + 3:
                break
            payload = bytes(pending[3:length + 3])
            del pending[:length + 3]
            if payload[:2] == b"\x18\x80":
                return payload[2:]
            if payload[:1] == b"\x01":
                raise RuntimeError("Companion rejected the diagnostic request")
    raise RuntimeError("No framed diagnostic reply")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    try:
        with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=2) as port:
            pending = bytearray()
            count = 1
            for index in range(8):
                if index >= count:
                    break
                port.write(b"<\x03\x00\x38\x80" + bytes([index]))
                data = response(port, pending)
                if len(data) < 34 or data[0] != 1 or len(data) != 34 + data[33]:
                    raise RuntimeError("Invalid diagnostic record")
                count = min(data[10], 8)
                event = {
                    "total": int.from_bytes(data[2:6], "little"),
                    "overwritten": int.from_bytes(data[6:10], "little"),
                    "index": data[11],
                }
                if data[1] & 1:
                    event.update(
                        sequence=int.from_bytes(data[12:16], "little"),
                        at_ms=int.from_bytes(data[16:20], "little"),
                        code=data[20], details=list(struct.unpack("<III", data[21:33])),
                        message=data[34:].decode("ascii", errors="replace"),
                    )
                print(json.dumps(event))
    except (OSError, RuntimeError, serial.SerialException) as error:
        parser.exit(1, f"Queued companion diagnostics: {error}\n")


if __name__ == "__main__":
    main()
