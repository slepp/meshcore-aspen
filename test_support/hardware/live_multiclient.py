#!/usr/bin/env python3
"""Exercise four KISS connections, three RF transmissions and a control soak."""

import argparse
from contextlib import ExitStack
import math
import secrets
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from meshcore_kiss_monitor import KissDecoder, kiss_frame, split_tcp_endpoint


class FrameReader:
    def __init__(self, sock):
        self.sock = sock
        self.decoder = KissDecoder()
        self.pending = []

    def receive_until(self, predicate, timeout=10.0):
        deadline = time.monotonic() + timeout
        observed = []
        while True:
            while self.pending:
                frame = self.pending.pop(0)
                observed.append(frame)
                if predicate(frame):
                    return frame, observed
                if frame[:2] == b"\x06\xf1":
                    raise RuntimeError(f"modem rejected request: {frame.hex()}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(min(0.1, remaining))
            try:
                data = self.sock.recv(1024)
            except socket.timeout:
                continue
            if not data:
                raise RuntimeError("KISS service disconnected")
            self.pending.extend(self.decoder.feed(data))
        raise RuntimeError(
            "timed out waiting for frame; observed="
            + ",".join(frame.hex() for frame in observed)
        )

    def assert_no_frame(self, predicate, timeout=0.5):
        deadline = time.monotonic() + timeout
        while True:
            while self.pending:
                frame = self.pending.pop(0)
                if predicate(frame):
                    raise RuntimeError(f"unexpected frame: {frame.hex()}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            self.sock.settimeout(min(0.1, remaining))
            try:
                data = self.sock.recv(1024)
            except socket.timeout:
                continue
            if not data:
                raise RuntimeError("KISS service disconnected")
            self.pending.extend(self.decoder.feed(data))


def exercise(endpoint, duration, clients=4):
    host, port = split_tcp_endpoint(endpoint)
    with ExitStack() as connections:
        def connect():
            connection = connections.enter_context(
                socket.create_connection((host, port), timeout=10)
            )
            connection.settimeout(0.1)
            return FrameReader(connection)

        readers = [connect() for _ in range(clients)]
        readers[0].sock.sendall(kiss_frame(6, b"\x0b"))
        configured, _ = readers[0].receive_until(
            lambda frame: len(frame) == 12 and frame[:2] == b"\x06\x8b"
        )
        radio = configured[2:]
        readers[0].sock.sendall(kiss_frame(6, b"\x09" + radio))
        readers[0].receive_until(lambda frame: frame == b"\x06\xf0")
        readers[0].sock.sendall(kiss_frame(6, b"\x0c"))
        power, _ = readers[0].receive_until(
            lambda frame: len(frame) == 3 and frame[:2] == b"\x06\x8c"
        )
        readers[0].sock.sendall(kiss_frame(6, b"\x0a" + power[2:]))
        readers[0].receive_until(lambda frame: frame == b"\x06\xf0")
        for reader in readers:
            reader.sock.sendall(kiss_frame(6, b"\x0b"))
            reader.receive_until(lambda frame: frame == b"\x06\x8b" + radio)
            reader.sock.sendall(kiss_frame(6, b"\x19\x01"))
            reader.receive_until(lambda frame: frame == b"\x06\x9a\x01")

        readers[0].sock.sendall(kiss_frame(6, b"\x17"))
        readers[0].receive_until(lambda frame: frame == b"\x06\x97")
        for reader in readers[1:]:
            reader.assert_no_frame(lambda frame: frame == b"\x06\x97", timeout=0.2)

        # Fragment one request while other connections submit complete frames.
        ping = kiss_frame(6, b"\x17")
        readers[0].sock.sendall(ping[:2])
        for reader in readers[1:]:
            reader.sock.sendall(ping)
        for reader in readers[1:]:
            reader.receive_until(lambda frame: frame == b"\x06\x97")
        readers[0].assert_no_frame(lambda frame: frame == b"\x06\x97", timeout=0.2)
        readers[0].sock.sendall(ping[2:])
        readers[0].receive_until(lambda frame: frame == b"\x06\x97")

        transmissions = min(3, clients)
        for sender in range(transmissions):
            payload = b"\x3c\x00KISS-test-" + secrets.token_bytes(8)
            data = b"\x00" + payload
            metadata = b"\x06\xf9\x80\x7f"
            readers[sender].sock.sendall(kiss_frame(0, payload))
            done, observed = readers[sender].receive_until(
                lambda frame: frame[:2] == b"\x06\xf8", timeout=15
            )
            if done != b"\x06\xf8\x01":
                raise RuntimeError(f"RF transmission failed: {done.hex()}")
            if data in observed:
                raise RuntimeError("sender received its own local reflection before TxDone")
            for index, reader in enumerate(readers):
                if index == sender:
                    reader.assert_no_frame(lambda frame: frame == data)
                    continue
                _, observed = reader.receive_until(lambda frame: frame == data, timeout=2)
                if any(frame[:2] == b"\x06\xf8" for frame in observed):
                    raise RuntimeError("TxDone was delivered to a different client")
                following, _ = reader.receive_until(lambda _frame: True, timeout=2)
                if following != metadata:
                    raise RuntimeError(f"loopback metadata not adjacent: {following.hex()}")
                reader.assert_no_frame(
                    lambda frame: frame == data or frame[:2] == b"\x06\xf8",
                    timeout=0.2,
                )

        readers[-1].sock.close()
        readers[-1] = connect()
        deadline = time.monotonic() + duration
        rounds = 0
        while time.monotonic() < deadline:
            for index, reader in enumerate(readers):
                reader.sock.sendall(kiss_frame(6, b"\x17" if index % 2 else b"\x0b"))
            for index, reader in enumerate(readers):
                expected = b"\x06\x97" if index % 2 else b"\x06\x8b" + radio
                reader.receive_until(lambda frame: frame == expected, timeout=5)
            rounds += 1
            time.sleep(0.1)
        print(
            f"multi-client KISS live test passed: {clients} connections, {transmissions} successful RF "
            f"transmissions, sender exclusion, local metadata, reconnect, "
            f"{rounds} control rounds over {duration:g}s"
        )


def verify_reception(endpoint, timeout):
    host, port = split_tcp_endpoint(endpoint)
    with ExitStack() as connections:
        readers = []
        for _ in range(2):
            connection = connections.enter_context(
                socket.create_connection((host, port), timeout=10)
            )
            reader = FrameReader(connection)
            connection.sendall(kiss_frame(6, b"\x19\x01"))
            reader.receive_until(lambda frame: frame == b"\x06\x9a\x01")
            readers.append(reader)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data, _ = readers[0].receive_until(
                lambda frame: frame[:1] == b"\x00",
                timeout=deadline - time.monotonic(),
            )
            metadata, _ = readers[0].receive_until(lambda _frame: True, timeout=2)
            if len(metadata) != 4 or metadata[:2] != b"\x06\xf9":
                raise RuntimeError("received RF data without adjacent signal metadata")
            if metadata == b"\x06\xf9\x80\x7f":
                continue
            readers[1].receive_until(lambda frame: frame == data, timeout=2)
            second_metadata, _ = readers[1].receive_until(lambda _frame: True, timeout=2)
            if second_metadata != metadata:
                raise RuntimeError("RF receive metadata differs between clients")
            snr, rssi = struct.unpack("bb", metadata[2:])
            print(
                f"physical RF reception passed: {len(data) - 1} bytes delivered "
                f"to both clients, RSSI {rssi} dBm, SNR {snr / 4:g} dB"
            )
            return
        raise RuntimeError("no physical RF reception before deadline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("endpoint", nargs="?", default="meshcore-radio.local:8001")
    parser.add_argument("--duration", type=float, default=60, help="control soak seconds")
    parser.add_argument("--clients", type=int, choices=(2, 3, 4), default=4,
                        help="test connections; leave slots for queued peers and other applications")
    parser.add_argument(
        "--receive-only", action="store_true",
        help="instead wait up to --duration seconds for real RF fanout to two clients; no TX",
    )
    args = parser.parse_args()
    if not math.isfinite(args.duration) or args.duration <= 0:
        parser.error("--duration must be positive and finite")
    try:
        if args.receive_only:
            verify_reception(args.endpoint, args.duration)
        else:
            exercise(args.endpoint, args.duration, args.clients)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"multi-client KISS live test failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
