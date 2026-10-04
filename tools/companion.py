# SPDX-License-Identifier: Apache-2.0
"""Bounded MeshCore companion TCP framing for operator tools."""

import socket
import struct
import time


class Companion:
    def __init__(self, host, port=5000):
        self.host, self.port = host, port
        self.pushes = []

    def __enter__(self):
        self.socket = socket.create_connection((self.host, self.port), timeout=5)
        try:
            device = self.command(b"\x16\x0d")
            if len(device) < 82 or device[:2] != b"\x0d\x0d":
                raise ValueError("Companion device reply does not report protocol 13")
            self.width = device[81] + 1
            self.channels = device[3]
            info = self.command(b"\x01" + bytes(7) + b"meshcore-tools")
            if len(info) < 36 or info[0] != 5:
                raise ValueError("Companion AppStart failed")
            self.public_key = info[4:36].hex()
            return self
        except (OSError, ValueError):
            self.socket.close()
            raise

    def __exit__(self, *_):
        self.socket.close()

    def read(self, size, deadline):
        data = bytearray()
        while len(data) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Companion TCP read deadline exceeded")
            self.socket.settimeout(remaining)
            part = self.socket.recv(size - len(data))
            if not part:
                raise ValueError("Companion disconnected during TCP read")
            data.extend(part)
        return bytes(data)

    def frame(self, deadline):
        header = self.read(3, deadline)
        size = struct.unpack_from("<H", header, 1)[0]
        if header[0] != ord(">") or not 0 < size <= 512:
            raise ValueError("Malformed companion TCP frame")
        return self.read(size, deadline)

    def command(self, data, allow_error=False):
        self.socket.sendall(b"<" + struct.pack("<H", len(data)) + data)
        deadline = time.monotonic() + 5
        for _ in range(128):
            result = self.frame(deadline)
            if result[0] < 128:
                if result[0] == 1 and not allow_error:
                    raise ValueError("Companion command rejected: " + result.hex())
                return result
            if len(self.pushes) == 128:
                raise ValueError("Companion TCP push queue exceeded")
            self.pushes.append(result)
        raise ValueError("Companion TCP unsolicited frame budget exceeded")


def sent_reply(frame):
    if len(frame) != 10 or frame[0] != 6 or frame[1] not in (0, 1):
        raise ValueError("Native send did not return an admission receipt")
    return {"flood": bool(frame[1]), "tag": frame[2:6].hex(),
            "timeout_ms": struct.unpack_from("<I", frame, 6)[0]}
