#!/usr/bin/env python3
"""Capture a bounded native USB handshake and optional boot diagnostics."""

import argparse
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=2) as port:
        if args.reset:
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
        start = time.monotonic()
        next_query = start + 2
        received = 0
        framed_reply = False
        buffered = bytearray()
        while time.monotonic() - start < 10:
            if time.monotonic() >= next_query:
                port.write(b"<\x02\x00\x16\x03")
                port.write(b"<\x01\x00\x05")
                port.flush()
                print("TX DeviceQuery + GetDeviceTime", flush=True)
                next_query += 2
            data = port.read(256)
            if data:
                received += len(data)
                print(f"RX {data.hex()} {data!r}", flush=True)
                buffered.extend(data)
                while len(buffered) >= 3:
                    length = int.from_bytes(buffered[1:3], "little")
                    if buffered[0] != ord(">") or not 1 <= length <= 512:
                        del buffered[0]
                        continue
                    if len(buffered) < length + 3:
                        break
                    payload = buffered[3:length + 3]
                    del buffered[:length + 3]
                    if payload[0] == 13:
                        framed_reply = True
                    if payload[0] == 9 and len(payload) == 5:
                        print(f"Device epoch: {int.from_bytes(payload[1:], 'little')}", flush=True)
        if not received:
            raise SystemExit("No USB bytes received, including device-query replies.")
        if not framed_reply:
            raise SystemExit("USB output captured, but no native device-info reply received.")


if __name__ == "__main__":
    main()
