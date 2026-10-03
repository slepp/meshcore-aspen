#!/usr/bin/env python3
"""Shared-radio KISS broker: one upstream modem, four isolated roles."""

import argparse
import socket
import threading
import time

import serial

from meshcore_kiss_broker_core import SharedKissBroker
from meshcore_kiss_monitor import split_tcp_endpoint

DEFAULT_ROLE_ENDPOINTS = {
    "router": "127.0.0.1:8101",
    "room": "127.0.0.1:8102",
    "client": "127.0.0.1:8103",
    "observer": "127.0.0.1:8104",
}


def parse_endpoint(endpoint: str) -> tuple[str, int]:
    host, separator, port = endpoint.rpartition(":")
    if not separator or not host or not port:
        raise ValueError(f"invalid endpoint: {endpoint}")
    return host, int(port)


class ReconnectingTcpUpstream:
    def __init__(self, endpoint: str):
        self.host, self.port = split_tcp_endpoint(endpoint)
        self.socket = None
        self.connect_lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.closed = False
        self.connected_event = False

    def _connection(self):
        with self.connect_lock:
            if self.closed:
                raise OSError("upstream modem is closed")
            if self.socket is None:
                connection = socket.create_connection((self.host, self.port), timeout=5)
                connection.settimeout(0.1)
                self.socket = connection
                self.connected_event = True
            return self.socket

    def take_connected_event(self):
        with self.connect_lock:
            connected = self.connected_event
            self.connected_event = False
            return connected

    def _disconnect(self, connection):
        with self.connect_lock:
            if self.socket is connection:
                connection.close()
                self.socket = None

    def read(self, size: int) -> bytes:
        try:
            connection = self._connection()
            data = connection.recv(size)
            if not data:
                raise ConnectionError("upstream modem disconnected")
            return data
        except socket.timeout:
            return b""
        except OSError:
            if "connection" in locals():
                self._disconnect(connection)
            time.sleep(0.25)
            return b""

    def write(self, data: bytes) -> int:
        connection = self._connection()
        try:
            with self.send_lock:
                connection.sendall(data)
        except OSError:
            self._disconnect(connection)
            raise
        return len(data)

    def close(self):
        with self.connect_lock:
            self.closed = True
            if self.socket is not None:
                self.socket.close()
                self.socket = None


def open_upstream(upstream: str):
    if not upstream:
        raise ValueError("an upstream modem is required")
    if upstream.startswith("tcp://"):
        return ReconnectingTcpUpstream(upstream[6:])
    return serial.Serial(
        upstream,
        115200,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--upstream",
        default="/dev/ttyACM0",
        help="serial device or tcp://HOST[:PORT] upstream KISS modem",
    )
    parser.add_argument(
        "--listen",
        action="append",
        metavar="ROLE=HOST:PORT",
        help=(
            "role endpoint; repeat for router, room, client, and observer "
            "(defaults: 8101 through 8104)"
        ),
    )
    parser.add_argument(
        "--state-dir",
        help="directory for persistent per-role identities",
    )
    return parser.parse_args()


def role_endpoints(values: list[str] | None) -> dict[str, tuple[str, int]]:
    configured = DEFAULT_ROLE_ENDPOINTS.copy()
    for value in values or ():
        role, separator, endpoint = value.partition("=")
        if not separator or role not in DEFAULT_ROLE_ENDPOINTS:
            raise ValueError(f"invalid role endpoint: {value}")
        configured[role] = endpoint
    return {role: parse_endpoint(endpoint) for role, endpoint in configured.items()}


def main() -> int:
    args = parse_args()
    from pathlib import Path

    upstream = open_upstream(args.upstream)
    broker = SharedKissBroker(
        upstream,
        role_endpoints(args.listen),
        state_dir=Path(args.state_dir) if args.state_dir else None,
    )
    try:
        broker.start()
        endpoints = ", ".join(
            f"{role}=tcp://{host}:{port}"
            for role, (host, port) in broker.endpoints().items()
        )
        print(
            f"meshcore-kiss-broker: {endpoints}; upstream={args.upstream}",
            flush=True,
        )
        while True:
            time.sleep(0.25)
    except KeyboardInterrupt:
        print("Stopped.", flush=True)
        return 0
    finally:
        broker.stop()
        try:
            upstream.close()
        except AttributeError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
