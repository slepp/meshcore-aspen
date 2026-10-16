# SPDX-License-Identifier: Apache-2.0
"""Prepare telemetry configuration for encrypted Management administration."""
import ipaddress
import re
import socket
import ssl
from urllib.parse import urlsplit

from tools.hardware.admin import private_file, read_file


def commands(args):
    endpoint = urlsplit(args.endpoint)
    if (endpoint.scheme != "https" or not endpoint.hostname or endpoint.username is not None or
            endpoint.password is not None or endpoint.fragment):
        raise ValueError("Use an HTTPS endpoint without URL credentials or a fragment")
    if not 30 <= args.interval <= 86400:
        raise ValueError("Telemetry interval must be 30..86400 seconds")
    port = endpoint.port if endpoint.port is not None else 443
    if not 1 <= port <= 65535 or not re.fullmatch(r"[A-Za-z0-9.-]{1,121}", endpoint.hostname):
        raise ValueError("Telemetry endpoint requires a hostname and port 1..65535")
    ca = read_file(args.ca_file, 4096)
    if not ca:
        raise ValueError("Telemetry CA file is empty")
    ssl.create_default_context(cadata=ca.decode("ascii"))
    token = private_file(args.token_file, 256) if args.token_file else b""
    if args.token_file and (not token or any(byte < 33 or byte > 126 for byte in token)):
        raise ValueError("Telemetry token requires 1..256 printable non-space bytes")
    path = endpoint.path or "/"
    if endpoint.query:
        path += "?" + endpoint.query
    if len(path) > 121 or path.startswith("//") or any(ord(c) < 33 or ord(c) > 126 or c == "\\" for c in path):
        raise ValueError("Telemetry path requires at most 121 printable URL characters")
    address = ipaddress.IPv4Address(args.address or socket.gethostbyname(endpoint.hostname))
    if address.is_unspecified or address.is_multicast or address.is_loopback:
        raise ValueError("Telemetry address must be a reachable unicast IPv4 address")
    result = ["telemetry off", "telemetry endpoint discard",
              f"telemetry endpoint address {address}",
              f"telemetry endpoint host {endpoint.hostname}",
              f"telemetry endpoint port {port}",
              f"telemetry endpoint path {path}",
              "telemetry endpoint ca clear", "telemetry endpoint token clear"]
    for name, data in (("ca", ca), ("token", token)):
        result.extend(f"telemetry endpoint {name} " + data[offset:offset + 56].hex()
                      for offset in range(0, len(data), 56))
    result.extend(["telemetry endpoint commit", f"telemetry interval {args.interval}",
                   "telemetry off" if args.disabled else "telemetry on"])
    if any(len(command.encode("ascii")) > 145 or any(c in command for c in "\r\n") for command in result):
        raise ValueError("Telemetry settings exceed the Management command size or contain a newline")
    return result
